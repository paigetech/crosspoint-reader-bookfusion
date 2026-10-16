#include "BookFusionHighlightAnchor.h"

#include <Epub/htmlEntities.h>
#include <Utf8.h>
#include <expat.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>

namespace BookFusionHighlightAnchor {
namespace {

constexpr size_t CONTEXT_CODEPOINTS = 32;  // quote_prefix / quote_suffix length
constexpr int MAX_DROPPED_EDGE_WORDS = 2;
// A shorter overlap at a page edge is too likely to be a coincidence.
constexpr size_t MIN_EDGE_OVERLAP = 4;

// Characters a page and its source can disagree on: layout whitespace,
// line-break and soft hyphens, zero-width marks, combining accents.
bool ignoredForMatch(const uint32_t cp) {
  switch (cp) {
    case ' ':
    case '\t':
    case '\n':
    case '\r':
    case '\f':
    case '-':
    case 0x00A0:  // no-break space
    case 0x00AD:  // soft hyphen
    case 0x2010:  // hyphen
    case 0x2011:  // non-breaking hyphen
    case 0x200B:  // zero-width space
    case 0x200C:
    case 0x200D:
    case 0x2060:  // word joiner
    case 0xFEFF:
      return true;
    default:
      break;
  }
  if (cp >= 0x2000 && cp <= 0x200A) return true;  // typographic spaces (em space indent)
  if (cp >= 0x0300 && cp <= 0x036F) return true;  // combining marks
  return false;
}

bool isDash(const uint32_t cp) { return cp == '-' || cp == 0x2010 || cp == 0x2011; }

uint32_t foldForMatch(uint32_t cp) {
  if (cp >= 'A' && cp <= 'Z') return cp + ('a' - 'A');
  if (cp >= 0x00C0) {
    // Precomposed letter -> base, so it matches a decomposed source (the mark is ignored).
    const uint32_t base = utf8DecomposedBase(cp);
    if (base != 0) cp = base;
    if (cp >= 'A' && cp <= 'Z') cp += 'a' - 'A';
  }
  return cp;
}

bool isSpace(const uint32_t cp) {
  return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == '\f' || cp == 0x00A0 ||
         (cp >= 0x2000 && cp <= 0x200A);
}

uint32_t utf16Units(const uint32_t cp) { return cp >= 0x10000 ? 2 : 1; }

// Comparison form of a text: the UTF-8 bytes of its kept, folded codepoints,
// and for each byte where it came from (a byte offset or a word index). About
// five bytes per source byte, so a chapter window stays small on the device.
struct Folded {
  std::string bytes;
  std::vector<uint32_t> origin;

  void add(const uint32_t cp, const uint32_t from) {
    if (ignoredForMatch(cp)) return;
    const size_t before = bytes.size();
    utf8AppendCodepoint(foldForMatch(cp), bytes);
    origin.insert(origin.end(), bytes.size() - before, from);
  }
};

// Origins are byte offsets into `text`.
Folded foldText(const std::string& text) {
  Folded f;
  f.bytes.reserve(text.size());
  f.origin.reserve(text.size());
  const auto* base = reinterpret_cast<const unsigned char*>(text.c_str());
  const unsigned char* p = base;
  const unsigned char* end = base + text.size();
  while (p < end) {
    const auto offset = static_cast<uint32_t>(p - base);
    f.add(utf8NextCodepoint(&p), offset);
  }
  return f;
}

// Origins are word indices.
Folded foldWords(const std::vector<std::string>& words, const size_t from, const size_t to) {
  Folded f;
  for (size_t w = from; w < to; w++) {
    const auto* p = reinterpret_cast<const unsigned char*>(words[w].c_str());
    while (*p) f.add(utf8NextCodepoint(&p), static_cast<uint32_t>(w));
  }
  return f;
}

std::vector<size_t> findAll(const std::string& hay, const std::string& needle) {
  std::vector<size_t> out;
  if (needle.empty()) return out;
  for (size_t at = hay.find(needle); at != std::string::npos; at = hay.find(needle, at + 1)) {
    // Only codepoint-aligned hits: a folded byte string matches whole codepoints.
    if ((static_cast<unsigned char>(hay[at]) & 0xC0) != 0x80) out.push_back(at);
  }
  return out;
}

// Codepoint at byte offset `at`, and the offset after it.
uint32_t codepointAt(const std::string& text, size_t at, size_t* next) {
  const auto* p = reinterpret_cast<const unsigned char*>(text.c_str()) + at;
  const auto* start = p;
  const uint32_t cp = utf8NextCodepoint(&p);
  *next = at + static_cast<size_t>(p - start);
  return cp;
}

// Byte offset of the codepoint `count` codepoints before `at`.
size_t backCodepoints(const std::string& text, size_t at, size_t count) {
  while (at > 0 && count > 0) {
    at--;
    while (at > 0 && (static_cast<unsigned char>(text[at]) & 0xC0) == 0x80) at--;
    count--;
  }
  return at;
}

size_t forwardCodepoints(const std::string& text, size_t at, size_t count) {
  while (at < text.size() && count > 0) {
    at++;
    while (at < text.size() && (static_cast<unsigned char>(text[at]) & 0xC0) == 0x80) at++;
    count--;
  }
  return at;
}

// Dashes at the end of the selection, inside words that are otherwise all
// ignored characters ("wins -", "wins --"), and in the last real word.
size_t trailingDashCount(const std::vector<std::string>& words, size_t end) {
  size_t total = 0;
  while (end-- > 0) {
    size_t dashes = 0;
    bool onlyIgnored = true;
    const auto* p = reinterpret_cast<const unsigned char*>(words[end].c_str());
    while (*p) {
      const uint32_t cp = utf8NextCodepoint(&p);
      if (isDash(cp)) {
        dashes++;
      } else if (!ignoredForMatch(cp)) {
        onlyIgnored = false;
        dashes = 0;
      }
    }
    total += dashes;
    if (!onlyIgnored) break;
  }
  return total;
}

}  // namespace

// --- BodyTextCollector --------------------------------------------------------

BodyTextCollector::BodyTextCollector(const uint32_t windowStartCp, const uint32_t windowEndCp, const bool htmlDialect)
    : htmlDialect(htmlDialect), windowStart(windowStartCp), windowEnd(windowEndCp) {
  XML_Parser p = XML_ParserCreate(nullptr);
  parser = p;
  if (!p) {
    parseOk = false;
    return;
  }
  XML_SetUserData(p, this);
  XML_SetElementHandler(p, &BodyTextCollector::startElement, &BodyTextCollector::endElement);
  XML_SetCharacterDataHandler(p, &BodyTextCollector::characterData);
  // HTML entities (&nbsp; ...) that XML does not define, as the chapter parser does.
  XML_SetDefaultHandlerExpand(p, &BodyTextCollector::defaultHandler);
  if (htmlDialect) XML_SetCdataSectionHandler(p, &BodyTextCollector::startCdata, &BodyTextCollector::endCdata);
}

void BodyTextCollector::startCdata(void* user) { static_cast<BodyTextCollector*>(user)->inCdata = true; }
void BodyTextCollector::endCdata(void* user) { static_cast<BodyTextCollector*>(user)->inCdata = false; }

BodyTextCollector::~BodyTextCollector() {
  if (parser) XML_ParserFree(static_cast<XML_Parser>(parser));
}

bool BodyTextCollector::feed(const char* data, const size_t len) {
  if (!parser || !parseOk || stopped) return false;
  auto* p = static_cast<XML_Parser>(parser);
  if (XML_Parse(p, data, static_cast<int>(len), XML_FALSE) != XML_STATUS_OK) {
    if (XML_GetErrorCode(p) != XML_ERROR_ABORTED) parseOk = false;
    return false;
  }
  return !stopped;
}

bool BodyTextCollector::finish() {
  if (!parser || !parseOk) return false;
  if (stopped) return true;
  auto* p = static_cast<XML_Parser>(parser);
  if (XML_Parse(p, "", 0, XML_TRUE) == XML_STATUS_ERROR && XML_GetErrorCode(p) != XML_ERROR_ABORTED) {
    parseOk = false;
  }
  return parseOk;
}

void BodyTextCollector::startElement(void* user, const char* name, const char**) {
  auto* self = static_cast<BodyTextCollector*>(user);
  const char* local = strrchr(name, ':');
  local = local ? local + 1 : name;
  if (self->bodyDepth > 0) {
    self->bodyDepth++;
  } else if (strcmp(local, "body") == 0) {
    self->bodyDepth = 1;
  }
  self->dropNextNewline = self->htmlDialect && (strcmp(local, "pre") == 0 || strcmp(local, "listing") == 0 ||
                                                strcmp(local, "textarea") == 0);
}

void BodyTextCollector::endElement(void* user, const char*) {
  auto* self = static_cast<BodyTextCollector*>(user);
  self->dropNextNewline = false;
  if (self->bodyDepth > 0 && --self->bodyDepth == 0) self->afterBody = true;
}

void BodyTextCollector::characterData(void* user, const char* s, const int len) {
  static_cast<BodyTextCollector*>(user)->addText(s, static_cast<size_t>(len));
}

void BodyTextCollector::defaultHandler(void* user, const char* s, const int len) {
  auto* self = static_cast<BodyTextCollector*>(user);
  // HTML: whitespace after </html> also lands in the body.
  if (self->htmlDialect && self->afterBody && self->bodyDepth == 0 && len > 0 &&
      std::all_of(s, s + len, [](const char c) { return c == ' ' || c == '\n' || c == '\t' || c == '\r'; })) {
    self->addText(s, static_cast<size_t>(len));
    return;
  }
  if (len >= 3 && s[0] == '&' && s[len - 1] == ';') {
    const char* value = lookupHtmlEntity(s, static_cast<size_t>(len));
    if (value) {
      static_cast<BodyTextCollector*>(user)->addText(value, strlen(value));
    } else {
      static_cast<BodyTextCollector*>(user)->addText(s, static_cast<size_t>(len));
    }
  }
}

void BodyTextCollector::addText(const char* s, const size_t len) {
  if ((bodyDepth == 0 && !(htmlDialect && afterBody)) || stopped || inCdata) return;
  const auto* p = reinterpret_cast<const unsigned char*>(s);
  const unsigned char* end = p + len;
  if (dropNextNewline && p < end) {
    dropNextNewline = false;
    if (*p == '\n') p++;
  }
  while (p < end) {
    const unsigned char* start = p;
    const uint32_t cp = utf8NextCodepoint(&p);
    if (cpCount < windowStart) {
      utf16BeforeWindow += utf16Units(cp);
    } else if (cpCount < windowEnd) {
      window.append(reinterpret_cast<const char*>(start), static_cast<size_t>(p - start));
    }
    cpCount++;
    if (cpCount >= windowEnd) {
      stopped = true;
      XML_StopParser(static_cast<XML_Parser>(parser), XML_FALSE);
      return;
    }
  }
}

// --- locate -------------------------------------------------------------------

bool locate(const std::string& windowText, const uint32_t windowStartCp, const uint32_t windowStartUtf16,
            const std::vector<std::string>& words, const uint32_t approxStartCp, Anchor& out, const PageContext* page) {
  if (words.empty() || windowText.empty()) return false;
  const Folded hay = foldText(windowText);

  // Codepoint index of each folded byte's source codepoint, computed on demand.
  const auto cpIndexOf = [&windowText](const size_t byteOffset) {
    uint32_t n = 0;
    for (size_t i = 0; i < byteOffset; i++) {
      if ((static_cast<unsigned char>(windowText[i]) & 0xC0) != 0x80) n++;
    }
    return n;
  };

  // Try the whole selection, then without parser-inserted edge words.
  for (int dropFront = 0; dropFront <= MAX_DROPPED_EDGE_WORDS; dropFront++) {
    for (int dropBack = 0; dropBack <= MAX_DROPPED_EDGE_WORDS; dropBack++) {
      if (static_cast<size_t>(dropFront + dropBack) >= words.size()) continue;
      const Folded needle = foldWords(words, dropFront, words.size() - dropBack);
      if (needle.bytes.empty()) continue;
      const auto matches = findAll(hay.bytes, needle.bytes);
      if (matches.empty()) continue;

      size_t best = matches.front();
      bool chosen = false;
      if (page) {
        // The selection is the Nth occurrence of its text from the page start.
        std::vector<std::string> pageWords = page->wordsBefore;
        const size_t selectionFrom = pageWords.size() + static_cast<size_t>(dropFront);
        pageWords.insert(pageWords.end(), words.begin(), words.end());
        const Folded onPage = foldWords(pageWords, 0, pageWords.size());
        size_t selectionByte = onPage.bytes.size();
        for (size_t i = 0; i < onPage.origin.size(); i++) {
          if (onPage.origin[i] >= selectionFrom) {
            selectionByte = i;
            break;
          }
        }
        size_t earlier = 0;
        for (const size_t m : findAll(onPage.bytes, needle.bytes)) {
          if (m < selectionByte) earlier++;
        }
        size_t seen = 0;
        for (const size_t m : matches) {
          if (windowStartCp + cpIndexOf(hay.origin[m]) < page->pageStartCp) continue;
          if (seen++ == earlier) {
            best = m;
            chosen = true;
            break;
          }
        }
      }
      if (!chosen) {  // closest to the expected start
        uint32_t bestDistance = UINT32_MAX;
        for (const size_t m : matches) {
          const uint32_t cp = windowStartCp + cpIndexOf(hay.origin[m]);
          const uint32_t distance = cp > approxStartCp ? cp - approxStartCp : approxStartCp - cp;
          if (distance < bestDistance) {
            bestDistance = distance;
            best = m;
          }
        }
      }

      const size_t quoteFrom = hay.origin[best];
      size_t quoteTo = 0;
      codepointAt(windowText, hay.origin[best + needle.bytes.size() - 1], &quoteTo);
      // Dashes ending the selection ("wins -") are ignored by the match; take
      // as many as the source has right after it.
      for (size_t dashes = trailingDashCount(words, words.size() - dropBack); dashes > 0; dashes--) {
        size_t at = quoteTo;  // spaces may sit between the dashes
        size_t next = at;
        uint32_t cp = 0;
        while (at < windowText.size()) {
          cp = codepointAt(windowText, at, &next);
          if (!isSpace(cp)) break;
          at = next;
        }
        if (at >= windowText.size() || !isDash(cp)) break;
        quoteTo = next;
      }

      out.cpStart = windowStartCp + cpIndexOf(quoteFrom);
      uint32_t u16 = windowStartUtf16;
      uint32_t cpCount = 0;
      for (size_t i = 0; i < quoteTo;) {
        size_t next = 0;
        const uint32_t cp = codepointAt(windowText, i, &next);
        if (i == quoteFrom) out.u16Start = u16;
        u16 += utf16Units(cp);
        cpCount++;
        i = next;
      }
      if (quoteFrom == 0) out.u16Start = windowStartUtf16;
      out.u16End = u16;
      out.cpEnd = windowStartCp + cpCount;

      out.quote = collapseWhitespace(windowText.substr(quoteFrom, quoteTo - quoteFrom));
      const size_t prefixFrom = backCodepoints(windowText, quoteFrom, CONTEXT_CODEPOINTS);
      out.prefix = windowText.substr(prefixFrom, quoteFrom - prefixFrom);
      const size_t suffixTo = forwardCodepoints(windowText, quoteTo, CONTEXT_CODEPOINTS);
      out.suffix = windowText.substr(quoteTo, suffixTo - quoteTo);
      return true;
    }
  }
  return false;
}

// --- PageMatcher --------------------------------------------------------------

struct PageMatcher::Impl {
  Folded page;
  size_t wordCount = 0;
};

PageMatcher::PageMatcher(const std::vector<std::string>& pageWords) : impl(new Impl) {
  impl->page = foldWords(pageWords, 0, pageWords.size());
  impl->wordCount = pageWords.size();
}

PageMatcher::~PageMatcher() { delete impl; }

bool PageMatcher::covered(const uint32_t pageStartCp, const uint32_t nextPageStartCp, const uint32_t hlStartCp,
                          const uint32_t hlEndCp, const std::string& quote, int& first, int& last) const {
  const Folded& page = impl->page;
  const auto wordCount = static_cast<int>(impl->wordCount);
  if (wordCount == 0 || hlEndCp <= pageStartCp || hlStartCp >= nextPageStartCp) return false;
  const Folded q = foldText(quote);
  if (page.bytes.empty() || q.bytes.empty()) return false;

  const bool startsHere = hlStartCp >= pageStartCp;
  // UINT32_MAX: the next page is not laid out yet, so whether the highlight
  // ends here is only known from the text.
  const bool nextKnown = nextPageStartCp != UINT32_MAX;
  const bool endsHere = hlEndCp <= nextPageStartCp;

  if (!startsHere && !endsHere) {  // the highlight runs through the whole page
    first = 0;
    last = wordCount - 1;
    return true;
  }

  if (startsHere) {
    const auto matches = findAll(page.bytes, q.bytes);
    if (!matches.empty()) {
      // Rank by position: the highlight's share of the way into the page.
      size_t best = matches.front();
      if (matches.size() > 1 && nextKnown && nextPageStartCp > pageStartCp) {
        const double expected = static_cast<double>(hlStartCp - pageStartCp) /
                                static_cast<double>(nextPageStartCp - pageStartCp) *
                                static_cast<double>(page.bytes.size());
        double bestDistance = 1e18;
        for (const size_t m : matches) {
          const double distance = std::abs(static_cast<double>(m) - expected);
          if (distance < bestDistance) {
            bestDistance = distance;
            best = m;
          }
        }
      }
      first = static_cast<int>(page.origin[best]);
      last = static_cast<int>(page.origin[best + q.bytes.size() - 1]);
      return true;
    }
    if (endsHere && nextKnown) return false;  // should be whole on this page, but is not
  }

  // Crosses one page edge: the longest piece of the quote that sits at that
  // edge of the page (whole codepoints only).
  const size_t maxLen = std::min(page.bytes.size(), q.bytes.size());
  const size_t minLen = std::min(MIN_EDGE_OVERLAP, maxLen);
  if (startsHere) {  // the quote's head ends the page
    for (size_t len = maxLen; len >= minLen && len > 0; len--) {
      const size_t at = page.bytes.size() - len;
      if ((static_cast<unsigned char>(page.bytes[at]) & 0xC0) == 0x80) continue;
      if (page.bytes.compare(at, len, q.bytes, 0, len) == 0) {
        first = static_cast<int>(page.origin[at]);
        last = wordCount - 1;
        return true;
      }
    }
    return false;
  }
  for (size_t len = maxLen; len >= minLen && len > 0; len--) {  // the quote's tail starts the page
    const size_t at = q.bytes.size() - len;
    if ((static_cast<unsigned char>(q.bytes[at]) & 0xC0) == 0x80) continue;
    if (page.bytes.compare(0, len, q.bytes, at, len) == 0) {
      first = 0;
      last = static_cast<int>(page.origin[len - 1]);
      return true;
    }
  }
  return false;
}

bool coveredWords(const std::vector<std::string>& pageWords, const uint32_t pageStartCp, const uint32_t nextPageStartCp,
                  const uint32_t hlStartCp, const uint32_t hlEndCp, const std::string& quote, int& first, int& last) {
  return PageMatcher(pageWords).covered(pageStartCp, nextPageStartCp, hlStartCp, hlEndCp, quote, first, last);
}

std::string collapseWhitespace(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  const auto* p = reinterpret_cast<const unsigned char*>(text.c_str());
  const unsigned char* end = p + text.size();
  bool pendingSpace = false;
  while (p < end) {
    const unsigned char* start = p;
    const uint32_t cp = utf8NextCodepoint(&p);
    if (isSpace(cp)) {
      pendingSpace = !out.empty();
      continue;
    }
    if (pendingSpace) {
      out.push_back(' ');
      pendingSpace = false;
    }
    out.append(reinterpret_cast<const char*>(start), static_cast<size_t>(p - start));
  }
  return out;
}

}  // namespace BookFusionHighlightAnchor
