#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

/**
 * Highlight anchoring between CrossPoint's page layout and BookFusion's
 * character offsets. Free of Arduino types so host tests cover it.
 *
 * Offset spaces:
 *  - BookFusion: UTF-16 code units into the concatenated text of every text
 *    node inside the chapter's <body> (a browser's Range.toString()).
 *  - CrossPoint ("cp"): Unicode codepoints of the same text, as counted by the
 *    chapter parser for Page::visibleTextOffset. It skips text inside <style>,
 *    <script> and <rp>, so it can trail BookFusion's count slightly; every
 *    lookup here therefore matches text, and offsets only rank candidates.
 */
namespace BookFusionHighlightAnchor {

// Streaming collector of a chapter's body text (BookFusion's offset space).
// Keeps only codepoints [windowStart, windowEnd) and stops once past it.
class BodyTextCollector {
 public:
  // htmlDialect: the chapter is a .html/.htm file, which BookFusion's web
  // reader parses as HTML: CDATA is not text, and a line break right after
  // <pre>, <listing> or <textarea> is dropped, and text after </body> still
  // belongs to the body.
  BodyTextCollector(uint32_t windowStartCp, uint32_t windowEndCp, bool htmlDialect = false);
  ~BodyTextCollector();
  BodyTextCollector(const BodyTextCollector&) = delete;
  BodyTextCollector& operator=(const BodyTextCollector&) = delete;

  // Feeds XHTML bytes. Returns false once the window is complete (the caller
  // may stop streaming) or the parser failed.
  bool feed(const char* data, size_t len);
  bool finish();

  bool ok() const { return parseOk; }
  const std::string& windowText() const { return window; }  // UTF-8
  uint32_t windowStartCp() const { return windowStart; }
  // UTF-16 units before the window: maps window codepoints to BookFusion offsets.
  uint32_t windowStartUtf16() const { return utf16BeforeWindow; }

 private:
  static void startElement(void* user, const char* name, const char** atts);
  static void endElement(void* user, const char* name);
  static void characterData(void* user, const char* s, int len);
  static void defaultHandler(void* user, const char* s, int len);
  static void startCdata(void* user);
  static void endCdata(void* user);
  void addText(const char* s, size_t len);

  void* parser = nullptr;
  bool parseOk = true;
  bool stopped = false;
  int bodyDepth = 0;  // > 0 inside <body>
  bool htmlDialect;
  bool inCdata = false;
  bool dropNextNewline = false;  // just opened <pre>/<listing>/<textarea> (HTML)
  bool afterBody = false;        // </body> seen (HTML keeps counting)
  uint32_t windowStart;
  uint32_t windowEnd;
  uint32_t cpCount = 0;
  uint32_t utf16BeforeWindow = 0;
  std::string window;
};

struct Anchor {
  uint32_t cpStart = 0;  // codepoints, BookFusion text
  uint32_t cpEnd = 0;
  uint32_t u16Start = 0;  // BookFusion offsets
  uint32_t u16End = 0;
  std::string quote;   // whitespace-collapsed
  std::string prefix;  // raw text right before the quote
  std::string suffix;  // raw text right after the quote
};

// Where the selection sits on its first page, to tell repeated text apart.
struct PageContext {
  uint32_t pageStartCp = 0;
  std::vector<std::string> wordsBefore;  // the page's words before the selection
};

// Finds the selected words in a chapter text window. Layout artifacts are
// ignored when comparing: whitespace, hyphens (line-break and real), soft
// hyphens, accents and ASCII case. Leading/trailing words that are not in the
// source (list numbers the parser inserts) are dropped if needed.
//
// Repeated text: the selection is the Nth occurrence of its text on the page,
// so the Nth occurrence after the page start wins. Without page context (or
// if that fails) the match closest to approxStartCp wins.
bool locate(const std::string& windowText, uint32_t windowStartCp, uint32_t windowStartUtf16,
            const std::vector<std::string>& words, uint32_t approxStartCp, Anchor& out,
            const PageContext* page = nullptr);

// Matches highlights against one rendered page; prepares the page once.
class PageMatcher {
 public:
  explicit PageMatcher(const std::vector<std::string>& pageWords);
  ~PageMatcher();
  PageMatcher(const PageMatcher&) = delete;
  PageMatcher& operator=(const PageMatcher&) = delete;
  // As coveredWords() below.
  bool covered(uint32_t pageStartCp, uint32_t nextPageStartCp, uint32_t hlStartCp, uint32_t hlEndCp,
               const std::string& quote, int& first, int& last) const;

 private:
  struct Impl;
  Impl* impl;
};

// Which of a page's words a highlight covers. pageStartCp / nextPageStartCp
// bound the page (UINT32_MAX when unknown). Returns false when the highlight
// is not on this page; otherwise [first, last] are word indices.
bool coveredWords(const std::vector<std::string>& pageWords, uint32_t pageStartCp, uint32_t nextPageStartCp,
                  uint32_t hlStartCp, uint32_t hlEndCp, const std::string& quote, int& first, int& last);

// Collapses whitespace runs to single spaces and trims.
std::string collapseWhitespace(const std::string& text);

}  // namespace BookFusionHighlightAnchor
