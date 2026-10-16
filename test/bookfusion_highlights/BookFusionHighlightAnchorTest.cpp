#include <gtest/gtest.h>

#include <climits>
#include <string>
#include <vector>

#include "BookFusionHighlightAnchor.h"

using namespace BookFusionHighlightAnchor;

namespace {

struct Collected {
  bool ok;
  std::string text;
  uint32_t startCp;
  uint32_t startUtf16;
  const std::string& windowText() const { return text; }
  uint32_t windowStartCp() const { return startCp; }
  uint32_t windowStartUtf16() const { return startUtf16; }
};

// Feeds the document in small chunks, as the zip stream does.
Collected collect(const std::string& xhtml, const uint32_t from, const uint32_t to, const size_t chunk = 7) {
  BodyTextCollector c(from, to);
  for (size_t i = 0; i < xhtml.size(); i += chunk) {
    if (!c.feed(xhtml.data() + i, std::min(chunk, xhtml.size() - i))) break;
  }
  const bool ok = c.finish();
  return {ok, c.windowText(), c.windowStartCp(), c.windowStartUtf16()};
}

const std::string kChapter =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.1//EN\" \"http://www.w3.org/TR/xhtml11/DTD/xhtml11.dtd\">\n"
    "<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>T</title></head>\n"
    "<body>\n"
    "  <h1>Chapter One</h1>\n"
    "  <p>It was a bright cold day in April, and the clocks were\n"
    "     striking thirteen.</p>\n"
    "  <p>Winston&nbsp;Smith slipped quickly through the glass doors.</p>\n"
    "</body></html>";

std::vector<std::string> split(const std::string& s) {
  std::vector<std::string> out;
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && s[i] == ' ') i++;
    size_t j = i;
    while (j < s.size() && s[j] != ' ') j++;
    if (j > i) out.push_back(s.substr(i, j - i));
    i = j;
  }
  return out;
}

// Offsets as a browser's Range.toString() over <body> would give them.
std::string bodyText(const std::string& xhtml) {
  const auto c = collect(xhtml, 0, UINT32_MAX);
  return c.windowText();
}

}  // namespace

TEST(BodyTextCollector, CollectsRawBodyTextWithEntities) {
  const std::string text = bodyText(kChapter);
  EXPECT_EQ(text.rfind("\n  Chapter One\n", 0), 0u);                // whitespace text nodes are kept
  EXPECT_NE(text.find("Winston\xC2\xA0Smith"), std::string::npos);  // &nbsp; expanded
  EXPECT_EQ(text.find("T\n"), std::string::npos);                   // <title> is outside <body>
}

TEST(BodyTextCollector, KeepsOnlyTheWindowAndCountsUtf16Before) {
  const std::string full = bodyText(kChapter);
  const auto c = collect(kChapter, 10, 20, 3);
  EXPECT_TRUE(c.ok);
  EXPECT_EQ(c.windowText(), full.substr(10, 10));  // ASCII so far: bytes == codepoints
  EXPECT_EQ(c.windowStartCp(), 10u);
  EXPECT_EQ(c.windowStartUtf16(), 10u);
}

TEST(BodyTextCollector, AstralCharactersTakeTwoUtf16Units) {
  const std::string doc = "<html><body><p>\xF0\x9F\x98\x80 abc</p></body></html>";  // emoji + " abc"
  const auto c = collect(doc, 2, 10);
  EXPECT_EQ(c.windowText(), "abc");
  EXPECT_EQ(c.windowStartUtf16(), 3u);  // surrogate pair + space
}

TEST(Locate, FindsSelectionAcrossSourceLineBreaks) {
  const std::string full = bodyText(kChapter);
  Anchor a;
  ASSERT_TRUE(locate(full, 0, 0, split("the clocks were striking thirteen."), 40, a));
  EXPECT_EQ(a.quote, "the clocks were striking thirteen.");
  const size_t start = full.find("the clocks");
  EXPECT_EQ(a.cpStart, start);
  EXPECT_EQ(a.u16Start, start);
  EXPECT_EQ(a.cpEnd, full.find("thirteen.") + 9);
  EXPECT_EQ(a.u16End - a.u16Start, a.cpEnd - a.cpStart);
  EXPECT_FALSE(a.prefix.empty());
  EXPECT_EQ(a.suffix.rfind("</p>", 0), std::string::npos);  // text, not markup
}

TEST(Locate, IgnoresLayoutHyphensAndCase) {
  const std::string full = bodyText(kChapter);
  Anchor a;
  // Hyphenated across a line ("strik-" / "ing") and a small-caps heading.
  ASSERT_TRUE(locate(full, 0, 0, {"strik-", "ing", "thir-", "teen."}, 0, a));
  EXPECT_EQ(a.quote, "striking thirteen.");
  ASSERT_TRUE(locate(full, 0, 0, {"CHAPTER", "ONE"}, 0, a));
  EXPECT_EQ(a.quote, "Chapter One");
}

TEST(Locate, MatchesAcrossNonBreakingSpace) {
  const std::string full = bodyText(kChapter);
  Anchor a;
  ASSERT_TRUE(locate(full, 0, 0, {"Winston", "Smith", "slipped"}, 0, a));
  EXPECT_EQ(a.quote, "Winston Smith slipped");  // no-break space collapsed like other whitespace
}

TEST(Locate, PicksTheOccurrenceNearestTheExpectedPosition) {
  const std::string text = "the end. Again the end. And the end.";
  Anchor a;
  ASSERT_TRUE(locate(text, 100, 100, {"the", "end."}, 115, a));
  EXPECT_EQ(a.cpStart, 100 + text.find("the end.", 9));
  ASSERT_TRUE(locate(text, 100, 100, {"the", "end."}, 135, a));
  EXPECT_EQ(a.cpStart, 100 + text.rfind("the end."));
}

TEST(Locate, PageContextPicksTheRightRepeat) {
  // "I" appears three times; the reader picked the second one on a page that
  // starts at "Then". The position estimate alone (drifted) would pick the first.
  const std::string text = "Before. Then I said that I would go, and I went.";
  PageContext page;
  page.pageStartCp = static_cast<uint32_t>(text.find("Then"));
  page.wordsBefore = {"Then", "I", "said", "that"};
  Anchor a;
  ASSERT_TRUE(locate(text, 0, 0, {"I"}, page.pageStartCp + 3, a, &page));
  EXPECT_EQ(a.cpStart, text.find("I would"));
}

TEST(Locate, KeepsTrailingDashes) {
  const std::string text = "Parent wins -- the end.";
  Anchor a;
  ASSERT_TRUE(locate(text, 0, 0, {"Parent", "wins", "--"}, 0, a));
  EXPECT_EQ(a.quote, "Parent wins --");
  ASSERT_TRUE(locate(text, 0, 0, {"Parent", "wins"}, 0, a));
  EXPECT_EQ(a.quote, "Parent wins");
}

TEST(Locate, DropsParserInsertedListNumbers) {
  const std::string text = "Ingredients\n<li>flour</li> two cups of flour";
  Anchor a;
  ASSERT_TRUE(locate(text, 0, 0, {"1.", "two", "cups"}, 0, a));
  EXPECT_EQ(a.quote, "two cups");
}

TEST(Locate, FailsWhenTextIsAbsent) {
  Anchor a;
  EXPECT_FALSE(locate("some other text", 0, 0, {"missing", "words"}, 0, a));
}

TEST(CoveredWords, HighlightInsideThePage) {
  const auto words = split("It was a bright cold day in April, and the clocks were");
  int first = -1, last = -1;
  ASSERT_TRUE(coveredWords(words, 100, 200, 120, 140, "bright cold day", first, last));
  EXPECT_EQ(first, 3);
  EXPECT_EQ(last, 5);
}

TEST(CoveredWords, HighlightStartingOnThisPageAndContinuing) {
  const auto words = split("It was a bright cold day in April, and the clocks were");
  int first = -1, last = -1;
  ASSERT_TRUE(coveredWords(words, 100, 160, 150, 190, "the clocks were striking thirteen.", first, last));
  EXPECT_EQ(first, 9);
  EXPECT_EQ(last, 11);
}

TEST(CoveredWords, ContinuingHighlightWhileNextPageIsUnknown) {
  const auto words = split("It was a bright cold day in April, and the clocks were");
  int first = -1, last = -1;
  ASSERT_TRUE(coveredWords(words, 100, UINT32_MAX, 150, 190, "the clocks were striking thirteen.", first, last));
  EXPECT_EQ(first, 9);
  EXPECT_EQ(last, 11);
  // Whole quote on the page: matched normally.
  ASSERT_TRUE(coveredWords(words, 100, UINT32_MAX, 120, 140, "bright cold day", first, last));
  EXPECT_EQ(first, 3);
  EXPECT_EQ(last, 5);
}

TEST(CoveredWords, HighlightEndingOnThisPage) {
  const auto words = split("striking thirteen. Winston Smith slipped quickly");
  int first = -1, last = -1;
  ASSERT_TRUE(coveredWords(words, 200, 300, 150, 215, "the clocks were striking thirteen.", first, last));
  EXPECT_EQ(first, 0);
  EXPECT_EQ(last, 1);
}

TEST(CoveredWords, MiddlePageIsFullyCovered) {
  const auto words = split("all of these words");
  int first = -1, last = -1;
  ASSERT_TRUE(coveredWords(words, 200, 300, 100, 400, "anything", first, last));
  EXPECT_EQ(first, 0);
  EXPECT_EQ(last, 3);
}

TEST(CoveredWords, NotOnThisPage) {
  const auto words = split("all of these words");
  int first = -1, last = -1;
  EXPECT_FALSE(coveredWords(words, 200, 300, 300, 320, "later text", first, last));
  EXPECT_FALSE(coveredWords(words, 200, 300, 150, 200, "earlier text", first, last));
}

TEST(CoveredWords, RepeatedPhraseUsesPosition) {
  const auto words = split("the end the end the end");  // 6 words, 23 chars
  int first = -1, last = -1;
  ASSERT_TRUE(coveredWords(words, 0, 23, 16, 23, "the end", first, last));
  EXPECT_EQ(first, 4);
  EXPECT_EQ(last, 5);
}

TEST(CollapseWhitespace, CollapsesAndTrims) {
  EXPECT_EQ(collapseWhitespace("  a\n   b\tc  "), "a b c");
  EXPECT_EQ(collapseWhitespace("x\xC2\xA0y"), "x y");
}
