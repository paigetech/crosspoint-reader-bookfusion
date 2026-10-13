#include "ArticleSanitizer.h"

#include <cctype>
#include <cstring>

#include "Epub/htmlEntities.h"

namespace {

using El = ArticleSanitizer::El;

enum class Kind : uint8_t {
  Ignore,       // drop the tag, keep its content (span, a, img, font, ...)
  SkipContent,  // drop the tag and everything inside it
  RawSkip,      // like SkipContent, but the content is raw text (script/style)
  Break,        // generic container: ends the current paragraph
  Block,
  Inline,
  Br,
  Hr,
  Cell,  // table cell: separate its text with a space
};

struct TagInfo {
  const char* name;
  Kind kind;
  El el;
};

constexpr TagInfo TAGS[] = {
    {"p", Kind::Block, El::P},
    {"h1", Kind::Block, El::H1},
    {"h2", Kind::Block, El::H2},
    {"h3", Kind::Block, El::H3},
    {"h4", Kind::Block, El::H4},
    {"h5", Kind::Block, El::H5},
    {"h6", Kind::Block, El::H6},
    {"pre", Kind::Block, El::PRE},
    {"blockquote", Kind::Block, El::BLOCKQUOTE},
    {"ul", Kind::Block, El::UL},
    {"ol", Kind::Block, El::OL},
    {"li", Kind::Block, El::LI},
    {"dt", Kind::Block, El::P},
    {"dd", Kind::Block, El::P},
    {"figcaption", Kind::Block, El::P},
    {"caption", Kind::Block, El::P},
    {"summary", Kind::Block, El::P},
    {"em", Kind::Inline, El::EM},
    {"i", Kind::Inline, El::EM},
    {"cite", Kind::Inline, El::EM},
    {"dfn", Kind::Inline, El::EM},
    {"var", Kind::Inline, El::EM},
    {"strong", Kind::Inline, El::STRONG},
    {"b", Kind::Inline, El::STRONG},
    {"code", Kind::Inline, El::CODE},
    {"kbd", Kind::Inline, El::CODE},
    {"samp", Kind::Inline, El::CODE},
    {"tt", Kind::Inline, El::CODE},
    {"sub", Kind::Inline, El::SUB},
    {"sup", Kind::Inline, El::SUP},
    {"br", Kind::Br, El::P},
    {"hr", Kind::Hr, El::P},
    {"td", Kind::Cell, El::P},
    {"th", Kind::Cell, El::P},
    {"div", Kind::Break, El::P},
    {"section", Kind::Break, El::P},
    {"article", Kind::Break, El::P},
    {"main", Kind::Break, El::P},
    {"header", Kind::Break, El::P},
    {"footer", Kind::Break, El::P},
    {"aside", Kind::Break, El::P},
    {"figure", Kind::Break, El::P},
    {"center", Kind::Break, El::P},
    {"table", Kind::Break, El::P},
    {"thead", Kind::Break, El::P},
    {"tbody", Kind::Break, El::P},
    {"tfoot", Kind::Break, El::P},
    {"tr", Kind::Break, El::P},
    {"dl", Kind::Break, El::P},
    {"details", Kind::Break, El::P},
    {"address", Kind::Break, El::P},
    {"fieldset", Kind::Break, El::P},
    {"nav", Kind::Break, El::P},
    {"body", Kind::Break, El::P},
    {"html", Kind::Break, El::P},
    {"script", Kind::RawSkip, El::P},
    {"style", Kind::RawSkip, El::P},
    {"head", Kind::SkipContent, El::P},
    {"title", Kind::SkipContent, El::P},
    {"noscript", Kind::SkipContent, El::P},
    {"svg", Kind::SkipContent, El::P},
    {"math", Kind::SkipContent, El::P},
    {"iframe", Kind::SkipContent, El::P},
    {"template", Kind::SkipContent, El::P},
    {"form", Kind::SkipContent, El::P},
    {"select", Kind::SkipContent, El::P},
    {"button", Kind::SkipContent, El::P},
    {"textarea", Kind::SkipContent, El::P},
    {"object", Kind::SkipContent, El::P},
    {"video", Kind::SkipContent, El::P},
    {"audio", Kind::SkipContent, El::P},
    {"canvas", Kind::SkipContent, El::P},
};

constexpr const char* EL_NAMES[] = {"p",  "h1", "h2", "h3", "h4",     "h5",   "h6",  "pre", "blockquote",
                                    "ul", "ol", "li", "em", "strong", "code", "sub", "sup"};

const char* elName(const El el) { return EL_NAMES[static_cast<int>(el)]; }

bool isInline(const El el) { return el >= El::EM; }
bool isTextBlock(const El el) { return (el >= El::P && el <= El::PRE) || el == El::LI; }
bool isContainer(const El el) { return el == El::BLOCKQUOTE || el == El::UL || el == El::OL; }

const TagInfo* lookupTag(const char* name) {
  for (const auto& t : TAGS) {
    if (strcmp(t.name, name) == 0) return &t;
  }
  return nullptr;
}

bool isXmlChar(const uint32_t cp) {
  return cp == 0x9 || cp == 0xA || cp == 0xD || (cp >= 0x20 && cp <= 0xD7FF) || (cp >= 0xE000 && cp <= 0xFFFD) ||
         (cp >= 0x10000 && cp <= 0x10FFFF);
}

size_t encodeUtf8(const uint32_t cp, char out[4]) {
  if (cp < 0x80) {
    out[0] = static_cast<char>(cp);
    return 1;
  }
  if (cp < 0x800) {
    out[0] = static_cast<char>(0xC0 | (cp >> 6));
    out[1] = static_cast<char>(0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp < 0x10000) {
    out[0] = static_cast<char>(0xE0 | (cp >> 12));
    out[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out[2] = static_cast<char>(0x80 | (cp & 0x3F));
    return 3;
  }
  out[0] = static_cast<char>(0xF0 | (cp >> 18));
  out[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
  out[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
  out[3] = static_cast<char>(0x80 | (cp & 0x3F));
  return 4;
}

bool isHtmlSpace(const char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

}  // namespace

// --- output ------------------------------------------------------------------

bool ArticleSanitizer::flushOut() {
  if (ok && outLen > 0) {
    ok = sink(outBuf, outLen);
  }
  outLen = 0;
  return ok;
}

bool ArticleSanitizer::out(const char* s, const size_t n) {
  if (!ok) return false;
  if (n >= OUT_BUF) {
    if (!flushOut()) return false;
    ok = sink(s, n);
    return ok;
  }
  if (outLen + n > OUT_BUF && !flushOut()) return false;
  memcpy(outBuf + outLen, s, n);
  outLen += n;
  return true;
}

bool ArticleSanitizer::out(const char* s) { return out(s, strlen(s)); }

// --- element stack -----------------------------------------------------------

bool ArticleSanitizer::inPre() const {
  for (int i = depth - 1; i >= 0; i--) {
    if (stack[i] == El::PRE) return true;
    if (!isInline(stack[i])) return false;
  }
  return false;
}

int ArticleSanitizer::textBlockIndex() const {
  for (int i = depth - 1; i >= 0; i--) {
    if (isTextBlock(stack[i])) return i;
    if (isContainer(stack[i])) return -1;
  }
  return -1;
}

void ArticleSanitizer::materialize() {
  for (int i = 0; i < depth; i++) {
    if (!emitted[i]) {
      out("<");
      out(elName(stack[i]));
      out(">");
      emitted[i] = true;
    }
  }
}

void ArticleSanitizer::openEl(const El el) {
  // Text blocks may use the last slots; everything else leaves headroom so a
  // paragraph can always open, however deep the input nests.
  const int limit = isTextBlock(el) ? MAX_DEPTH : MAX_DEPTH - 2;
  if (depth >= limit) return;
  if (isInline(el)) {
    materialize();
    // Whitespace before an inline element belongs outside it.
    if (pendingSpace && !atBlockStart) out(" ");
    pendingSpace = false;
  }
  // Only inline elements are written immediately; blocks and containers wait
  // for content, so link-only lists or image-only quotes leave nothing behind.
  const bool eager = isInline(el);
  if (eager) {
    materialize();
    out("<");
    out(elName(el));
    out(">");
  }
  emitted[depth] = eager;
  stack[depth++] = el;
  if (!isInline(el)) {
    atBlockStart = true;
    pendingSpace = false;
  }
}

void ArticleSanitizer::closeTo(const int index) {
  bool closedBlock = false;
  while (depth > index && depth > 0) {
    depth--;
    const El el = stack[depth];
    if (emitted[depth]) {
      out("</");
      out(elName(el));
      out(">");
    }
    if (!isInline(el)) closedBlock = true;
  }
  if (closedBlock) {
    atBlockStart = true;
    pendingSpace = false;
  }
}

void ArticleSanitizer::closeInlines() {
  int i = depth;
  while (i > 0 && isInline(stack[i - 1])) i--;
  closeTo(i);
}

void ArticleSanitizer::closeTextBlocks() {
  const int idx = textBlockIndex();
  if (idx >= 0 && stack[idx] == El::LI) {
    // <li><div>text</div></li>: a container break inside a list item must
    // not end the item, or the text lands in a new, second bullet.
    closeTo(idx + 1);
  } else if (idx >= 0) {
    closeTo(idx);
  } else {
    closeInlines();
  }
}

bool ArticleSanitizer::ensureTextBlock() {
  if (textBlockIndex() >= 0) return true;
  closeInlines();
  const El container = depth > 0 ? stack[depth - 1] : El::P;
  const bool inList = depth > 0 && (container == El::UL || container == El::OL);
  openEl(inList ? El::LI : El::P);
  return textBlockIndex() >= 0;
}

void ArticleSanitizer::openBlock(const El el) {
  const int textIdx = textBlockIndex();
  switch (el) {
    case El::P:
    case El::H1:
    case El::H2:
    case El::H3:
    case El::H4:
    case El::H5:
    case El::H6:
    case El::PRE:
      // <li><p>text</p></li>: the list item already is the paragraph.
      if (textIdx >= 0 && stack[textIdx] == El::LI && el == El::P) {
        closeTo(textIdx + 1);
        return;
      }
      if (textIdx >= 0) {
        closeTo(textIdx);
      } else {
        closeInlines();
      }
      openEl(el);
      return;
    case El::BLOCKQUOTE:
    case El::UL:
    case El::OL:
      // Lists may nest inside a list item; everything else ends the paragraph.
      if (textIdx >= 0 && stack[textIdx] == El::LI && el != El::BLOCKQUOTE) {
        closeTo(textIdx + 1);
      } else if (textIdx >= 0) {
        closeTo(textIdx);
      } else {
        closeInlines();
      }
      openEl(el);
      return;
    case El::LI: {
      int listIdx = -1;
      for (int i = depth - 1; i >= 0; i--) {
        if (stack[i] == El::UL || stack[i] == El::OL) {
          listIdx = i;
          break;
        }
      }
      if (listIdx < 0) {
        openBlock(El::P);
        return;
      }
      closeTo(listIdx + 1);
      openEl(El::LI);
      return;
    }
    default:
      return;
  }
}

void ArticleSanitizer::closeNamed(const El el) {
  for (int i = depth - 1; i >= 0; i--) {
    if (stack[i] == el) {
      closeTo(i);
      return;
    }
    // A closing inline never reaches past its paragraph.
    if (isInline(el) && !isInline(stack[i])) return;
  }
}

// --- text --------------------------------------------------------------------

void ArticleSanitizer::emitTextChar(const char* bytes, const size_t n, const bool whitespace) {
  if (skipDepth > 0) return;
  if (inPre()) {
    if (bytes[0] == '\r') return;  // keep \n line breaks only
    atBlockStart = false;
  } else if (whitespace) {
    if (!atBlockStart) pendingSpace = true;
    return;
  } else {
    if (!ensureTextBlock()) return;
    if (pendingSpace && !atBlockStart) out(" ");
    pendingSpace = false;
    atBlockStart = false;
  }
  materialize();
  if (n == 1 && bytes[0] == '&') {
    out("&amp;");
  } else if (n == 1 && bytes[0] == '<') {
    out("&lt;");
  } else if (n == 1 && bytes[0] == '>') {
    out("&gt;");
  } else {
    out(bytes, n);
  }
  if (!whitespace) textChars++;
}

void ArticleSanitizer::emitCodepointText(const uint32_t cp) {
  if (!isXmlChar(cp)) {
    emitCodepointText(0xFFFD);
    return;
  }
  char buf[4];
  const size_t n = encodeUtf8(cp, buf);
  const bool ws = cp == 0x20 || cp == 0x9 || cp == 0xA || cp == 0xD;
  emitTextChar(buf, n, ws);
}

void ArticleSanitizer::emitValidatedUtf8(const uint8_t* bytes, const size_t n) {
  emitTextChar(reinterpret_cast<const char*>(bytes), n, false);
}

void ArticleSanitizer::flushUtf8Invalid() {
  if (utfNeed > 0) {
    utfNeed = 0;
    utfHave = 0;
    emitCodepointText(0xFFFD);
  }
}

void ArticleSanitizer::onTextByte(const uint8_t c) {
  if (utfNeed > 0) {
    bool valid = (c & 0xC0) == 0x80;
    if (valid && utfHave == 1) {
      // Reject overlong forms, surrogates and code points above U+10FFFF.
      const uint8_t lead = utfPending[0];
      if ((lead == 0xE0 && c < 0xA0) || (lead == 0xED && c > 0x9F) || (lead == 0xF0 && c < 0x90) ||
          (lead == 0xF4 && c > 0x8F)) {
        valid = false;
      }
    }
    if (!valid) {
      flushUtf8Invalid();
      onTextByte(c);  // re-examine this byte as a fresh lead byte
      return;
    }
    utfPending[utfHave++] = c;
    if (utfHave == utfNeed + 1) {
      emitValidatedUtf8(utfPending, utfHave);
      utfNeed = 0;
      utfHave = 0;
    }
    return;
  }

  if (c < 0x80) {
    if (isHtmlSpace(static_cast<char>(c))) {
      const char ch = static_cast<char>(c);
      emitTextChar(&ch, 1, true);
    } else if (c >= 0x20 && c != 0x7F) {
      const char ch = static_cast<char>(c);
      emitTextChar(&ch, 1, false);
    }
    // Other C0 controls are not legal XML characters: drop them.
    return;
  }
  if (c >= 0xC2 && c <= 0xDF) {
    utfNeed = 1;
  } else if (c >= 0xE0 && c <= 0xEF) {
    utfNeed = 2;
  } else if (c >= 0xF0 && c <= 0xF4) {
    utfNeed = 3;
  } else {
    emitCodepointText(0xFFFD);
    return;
  }
  utfPending[0] = c;
  utfHave = 1;
}

// --- entities ----------------------------------------------------------------

void ArticleSanitizer::handleEntity() {
  entityBuf[entityLen] = '\0';
  const bool terminated = entityLen > 0 && entityBuf[entityLen - 1] == ';';

  if (entityLen >= 3 && entityBuf[1] == '#') {
    const bool hex = entityBuf[2] == 'x' || entityBuf[2] == 'X';
    uint32_t cp = 0;
    bool any = false;
    bool overflow = false;
    for (size_t i = hex ? 3 : 2; i < entityLen && entityBuf[i] != ';'; i++) {
      const char c = entityBuf[i];
      int v = -1;
      if (c >= '0' && c <= '9') {
        v = c - '0';
      } else if (hex && c >= 'a' && c <= 'f') {
        v = c - 'a' + 10;
      } else if (hex && c >= 'A' && c <= 'F') {
        v = c - 'A' + 10;
      }
      if (v < 0) break;
      any = true;
      cp = cp * (hex ? 16 : 10) + static_cast<uint32_t>(v);
      if (cp > 0x10FFFF) overflow = true;
    }
    if (any) {
      emitCodepointText(overflow ? 0xFFFD : cp);
      return;
    }
  } else if (entityLen >= 2) {
    char lookup[ENTITY_BUF + 1];
    memcpy(lookup, entityBuf, entityLen);
    size_t lookupLen = entityLen;
    if (!terminated) lookup[lookupLen++] = ';';
    if (const char* value = lookupHtmlEntity(lookup, lookupLen)) {
      for (const char* p = value; *p; p++) onTextByte(static_cast<uint8_t>(*p));
      return;
    }
  }

  // Unknown entity: keep it as literal text (the '&' gets escaped).
  for (size_t i = 0; i < entityLen; i++) onTextByte(static_cast<uint8_t>(entityBuf[i]));
}

// --- tags --------------------------------------------------------------------

void ArticleSanitizer::handleTag() {
  tagBuf[tagLen] = '\0';
  const char* p = tagBuf;
  if (*p == '!' || *p == '?') return;  // doctype, processing instruction, CDATA

  bool closing = false;
  if (*p == '/') {
    closing = true;
    p++;
  }
  char name[12];
  size_t nameLen = 0;
  while (*p && (isalnum(static_cast<unsigned char>(*p)) || *p == '-' || *p == ':') && nameLen < sizeof(name) - 1) {
    name[nameLen++] = static_cast<char>(tolower(static_cast<unsigned char>(*p)));
    p++;
  }
  name[nameLen] = '\0';
  if (nameLen == 0) return;

  size_t end = tagLen;
  while (end > 0 && isHtmlSpace(tagBuf[end - 1])) end--;
  const bool selfClosing = !closing && end > 0 && tagBuf[end - 1] == '/';

  if (skipDepth > 0) {
    if (strcmp(name, skipName) == 0) {
      if (closing) {
        skipDepth--;
      } else if (!selfClosing) {
        skipDepth++;
      }
    }
    return;
  }

  const TagInfo* info = lookupTag(name);
  if (!info) return;

  if (closing) {
    switch (info->kind) {
      case Kind::Block:
      case Kind::Inline:
        closeNamed(info->el);
        break;
      case Kind::Break:
        closeTextBlocks();
        break;
      case Kind::Cell:
        if (textBlockIndex() >= 0 && !atBlockStart) pendingSpace = true;
        break;
      default:
        break;
    }
    return;
  }

  switch (info->kind) {
    case Kind::RawSkip:
      if (!selfClosing) {
        strncpy(rawCloseName, name, sizeof(rawCloseName) - 1);
        rawCloseName[sizeof(rawCloseName) - 1] = '\0';
        rawMatch = 0;
        state = State::RawSkip;
      }
      break;
    case Kind::SkipContent:
      if (!selfClosing) {
        strncpy(skipName, name, sizeof(skipName) - 1);
        skipName[sizeof(skipName) - 1] = '\0';
        skipDepth = 1;
      }
      break;
    case Kind::Break:
      closeTextBlocks();
      break;
    case Kind::Block:
      openBlock(info->el);
      break;
    case Kind::Inline:
      if (ensureTextBlock()) openEl(info->el);
      break;
    case Kind::Br:
      if (textBlockIndex() >= 0) {
        materialize();
        out("<br/>");
        pendingSpace = false;
        atBlockStart = true;
      }
      break;
    case Kind::Hr:
      closeTextBlocks();
      materialize();
      out("<hr/>");
      break;
    case Kind::Cell:
      if (textBlockIndex() >= 0 && !atBlockStart) pendingSpace = true;
      break;
    case Kind::Ignore:
      break;
  }
}

// --- state machine -----------------------------------------------------------

bool ArticleSanitizer::feed(const char* data, const size_t len) {
  for (size_t i = 0; i < len && ok; i++) {
    const char c = data[i];
    switch (state) {
      case State::Text:
        if (c == '<') {
          state = State::TagOpen;
        } else if (c == '&') {
          flushUtf8Invalid();
          entityBuf[0] = '&';
          entityLen = 1;
          state = State::Entity;
        } else {
          onTextByte(static_cast<uint8_t>(c));
        }
        break;

      case State::TagOpen:
        if (isalpha(static_cast<unsigned char>(c)) || c == '/' || c == '!' || c == '?') {
          flushUtf8Invalid();
          tagLen = 0;
          tagQuote = 0;
          tagBuf[tagLen++] = c;
          state = State::Tag;
        } else {
          // A bare '<' in text ("a < b").
          onTextByte('<');
          state = State::Text;
          i--;  // re-examine c as text
        }
        break;

      case State::Tag:
        if (tagQuote) {
          if (c == tagQuote) tagQuote = 0;
        } else if (c == '>') {
          handleTag();
          if (state == State::Tag) state = State::Text;
          break;
        } else if ((c == '"' || c == '\'') && tagLen > 0 && tagBuf[0] != '!') {
          tagQuote = c;
        }
        if (tagLen < TAG_BUF - 1) tagBuf[tagLen++] = c;
        if (tagLen == 3 && tagBuf[0] == '!' && tagBuf[1] == '-' && tagBuf[2] == '-') {
          commentDashes = 0;
          state = State::Comment;
        }
        break;

      case State::Comment:
        if (c == '>' && commentDashes >= 2) {
          state = State::Text;
        } else if (c == '-') {
          commentDashes++;
        } else {
          commentDashes = 0;
        }
        break;

      case State::Entity:
        if (c == ';') {
          entityBuf[entityLen++] = c;
          handleEntity();
          state = State::Text;
        } else if ((isalnum(static_cast<unsigned char>(c)) || (c == '#' && entityLen == 1)) &&
                   entityLen < ENTITY_BUF - 2) {
          entityBuf[entityLen++] = c;
        } else {
          handleEntity();
          state = State::Text;
          i--;  // re-examine c as text
        }
        break;

      case State::RawSkip: {
        // Match "</" + rawCloseName, case-insensitively.
        const size_t nameLen = strlen(rawCloseName);
        char expected;
        if (rawMatch == 0) {
          expected = '<';
        } else if (rawMatch == 1) {
          expected = '/';
        } else {
          expected = rawCloseName[rawMatch - 2];
        }
        if (tolower(static_cast<unsigned char>(c)) == expected) {
          rawMatch++;
          if (rawMatch == nameLen + 2) {
            // Hand the rest of the closing tag to the tag parser.
            tagLen = 0;
            tagQuote = 0;
            tagBuf[tagLen++] = '/';
            for (size_t k = 0; k < nameLen; k++) tagBuf[tagLen++] = rawCloseName[k];
            state = State::Tag;
          }
        } else {
          rawMatch = (c == '<') ? 1 : 0;
        }
        break;
      }
    }
  }
  return ok;
}

bool ArticleSanitizer::finish() {
  if (state == State::Entity) handleEntity();
  if (state == State::TagOpen) onTextByte('<');
  state = State::Text;
  flushUtf8Invalid();
  closeTo(0);
  return flushOut();
}
