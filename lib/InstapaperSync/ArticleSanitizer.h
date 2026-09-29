#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>

/**
 * Streaming HTML → XHTML body converter for read-later articles.
 *
 * CrossPoint's chapter parser is a strict XML (expat) parser, so arbitrary web
 * HTML has to become well-formed XHTML before it can go into an EPUB. This
 * keeps the reading structure (paragraphs, headings, lists, quotes, pre/code,
 * emphasis, line breaks, rules) and drops everything else: attributes, links,
 * images, tables (cells become text), scripts, styles and embedded media.
 *
 * Guarantees for any input, fed in chunks of any size:
 *  - output tags are balanced and properly nested,
 *  - only &amp; &lt; &gt; are emitted as entities (HTML named and numeric
 *    entities are decoded to UTF-8),
 *  - output is valid UTF-8 with no XML-illegal control characters
 *    (invalid bytes become U+FFFD).
 *
 * Memory is O(1) in input size: a small tag buffer and an element stack of at
 * most MAX_DEPTH entries (the object is ~0.5 KB, so heap-allocate it rather
 * than putting it on a task stack). The output is only the body content; the
 * caller wraps it in the XHTML document.
 */
class ArticleSanitizer {
 public:
  using Sink = std::function<bool(const char* data, size_t len)>;

  explicit ArticleSanitizer(Sink sink) : sink(std::move(sink)) {}

  // Feed the next chunk of HTML. Returns false once the sink has failed.
  bool feed(const char* data, size_t len);
  // Flush pending state and close every open element.
  bool finish();

  // Number of characters of visible text emitted (0 means an empty article).
  size_t textLength() const { return textChars; }

  static constexpr int MAX_DEPTH = 24;

  enum class El : uint8_t { P, H1, H2, H3, H4, H5, H6, PRE, BLOCKQUOTE, UL, OL, LI, EM, STRONG, CODE, SUB, SUP };

 private:
  enum class State : uint8_t { Text, TagOpen, Tag, Comment, Entity, RawSkip };

  Sink sink;
  bool ok = true;
  State state = State::Text;

  // Tag being collected (name + attributes, without the angle brackets).
  static constexpr size_t TAG_BUF = 96;
  char tagBuf[TAG_BUF] = {};
  size_t tagLen = 0;
  char tagQuote = 0;  // inside a quoted attribute value

  // Comment terminator matching ("-->").
  uint8_t commentDashes = 0;

  // Entity being collected, including the leading '&'.
  static constexpr size_t ENTITY_BUF = 34;
  char entityBuf[ENTITY_BUF] = {};
  size_t entityLen = 0;

  // Raw-text skipping for <script>/<style>: match "</name".
  char rawCloseName[8] = {};
  size_t rawMatch = 0;

  // Content skipping for other dropped elements (svg, iframe, head, ...).
  char skipName[12] = {};
  int skipDepth = 0;

  // UTF-8 validation state for text.
  uint8_t utfPending[4] = {};
  uint8_t utfHave = 0;
  uint8_t utfNeed = 0;

  El stack[MAX_DEPTH] = {};
  // Blocks are opened lazily: a start tag is written only once content
  // arrives, so <p><img></p> or a link-only <ul> leaves no empty element.
  bool emitted[MAX_DEPTH] = {};
  int depth = 0;

  bool pendingSpace = false;  // collapsed whitespace not yet written
  bool atBlockStart = true;   // suppress leading whitespace in a block
  size_t textChars = 0;

  // Batches sink writes (the device sink is an SD file).
  static constexpr size_t OUT_BUF = 256;
  char outBuf[OUT_BUF] = {};
  size_t outLen = 0;

  bool out(const char* s, size_t n);
  bool out(const char* s);
  bool flushOut();

  void onTextByte(uint8_t c);
  void emitCodepointText(uint32_t cp);
  void emitValidatedUtf8(const uint8_t* bytes, size_t n);
  void emitTextChar(const char* bytes, size_t n, bool whitespace);
  void flushUtf8Invalid();

  void handleTag();
  void handleEntity();

  bool inPre() const;
  int textBlockIndex() const;
  bool ensureTextBlock();
  void openEl(El el);
  void materialize();
  void closeTo(int index);
  void closeTextBlocks();
  void closeInlines();
  void openBlock(El el);
  void closeNamed(El el);
};
