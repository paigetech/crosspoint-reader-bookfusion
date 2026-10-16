#pragma once

#include <Epub/Page.h>
#include <Epub/Section.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "activities/Activity.h"

/**
 * Picks the text of a new highlight (fork feature, BookFusion books).
 *
 * Selection follows upstream PR #1742 ("text clipping selection"): a word
 * cursor moves with Left/Right (word) and Up/Down (line), Confirm marks the
 * start and Confirm again saves; the selection can run onto the next two
 * pages of the chapter. On touch screens a tap marks the start and a second
 * tap the end. Back clears the start mark, then leaves.
 */
class HighlightSelectActivity final : public Activity {
 public:
  struct Selection {
    bool saved = false;
    std::vector<std::string> words;        // as laid out, in reading order
    uint32_t approxStartCp = 0;            // chapter codepoint offset estimate of words[0]
    uint32_t pageStartCp = 0;              // start of the page words[0] is on
    std::vector<std::string> wordsBefore;  // that page's words before words[0]
  };

  HighlightSelectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, Section& section, int startPage,
                          int marginLeft, int marginTop, std::shared_ptr<Selection> out)
      : Activity("HighlightSelect", renderer, mappedInput),
        section(section),
        startPage(startPage),
        marginLeft(marginLeft),
        marginTop(marginTop),
        out(std::move(out)) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  static constexpr int MAX_PAGES = 3;

  struct LoadedPage {
    std::unique_ptr<Page> page;
    uint32_t startCp = 0;
  };
  // `text` points into the owning page's TextBlock arena.
  struct Word {
    int16_t x;
    int16_t y;
    int16_t width;
    uint16_t row;  // per page
    uint8_t pageIdx;
    EpdFontFamily::Style style;
    const char* text;
  };

  bool loadNextPage();
  int closestInRow(int pageIdx, int row, int centerX) const;
  int rowCount(int pageIdx) const;
  void moveVertical(int direction);
  void moveTo(int index);
  int wordAt(int x, int y) const;
  void save();
  void drawHints() const;

  Section& section;
  const int startPage;
  const int marginLeft;
  const int marginTop;
  std::shared_ptr<Selection> out;

  int fontId = 0;
  int lineHeight = 0;
  std::vector<LoadedPage> pages;
  std::vector<Word> words;
  int cursor = 0;
  int anchor = -1;  // start mark, -1 before Confirm
  unsigned long lastHorizontalMoveTime = 0;
};
