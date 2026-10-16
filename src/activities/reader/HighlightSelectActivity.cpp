#include "HighlightSelectActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include <climits>
#include <cstdlib>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"

namespace {

constexpr unsigned long WORD_REPEAT_START_MS = 500;
constexpr unsigned long WORD_REPEAT_INTERVAL_MS = 300;
constexpr int UNDERLINE_THICKNESS = 2;

// Tokens made only of spaces (indent em spaces) are layout, not text.
bool isBlankToken(const char* text) {
  for (const auto* p = reinterpret_cast<const uint8_t*>(text); *p != 0; p++) {
    if (*p == ' ') continue;
    if (p[0] == 0xE2 && p[1] == 0x80 && p[2] >= 0x80 && p[2] <= 0x8A) {  // U+2000-200A
      p += 2;
      continue;
    }
    return false;
  }
  return true;
}

size_t codepointCount(const char* text) {
  size_t n = 0;
  for (const auto* p = reinterpret_cast<const uint8_t*>(text); *p != 0; p++) {
    if ((*p & 0xC0) != 0x80) n++;
  }
  return n;
}

}  // namespace

void HighlightSelectActivity::onEnter() {
  Activity::onEnter();
  fontId = SETTINGS.getReaderFontId();
  lineHeight = renderer.getLineHeight(fontId);
  loadNextPage();
  if (!words.empty()) {
    // Start mid-page, like the dictionary cursor: any word is half a page away.
    const int initial = closestInRow(0, rowCount(0) / 2, renderer.getScreenWidth() / 2);
    if (initial >= 0) cursor = initial;
  }
  requestUpdate();
}

bool HighlightSelectActivity::loadNextPage() {
  // The render task reads `words`, and the SD card shares the display's bus.
  RenderLock lock(*this);
  const int pageNumber = startPage + static_cast<int>(pages.size());
  if (static_cast<int>(pages.size()) >= MAX_PAGES || pageNumber >= section.pageCount) return false;
  auto page = section.loadPage(pageNumber);
  if (!page) return false;

  const auto pageIdx = static_cast<uint8_t>(pages.size());
  std::string pageText;
  pageText.reserve(2048);
  uint8_t styleMask = 0;
  uint16_t row = 0;
  const size_t firstNew = words.size();
  for (const auto& element : page->elements) {
    if (element->getTag() != TAG_PageLine) continue;
    const auto* line = static_cast<const PageLine*>(element.get());
    const auto* block = line->getBlock();
    if (!block || !block->valid()) continue;
    bool rowHasWords = false;
    const int rubyShift = block->getRubyShift(renderer.getFontAscenderSize(fontId));
    for (uint16_t i = 0; i < block->wordCount(); i++) {
      const char* text = block->wordText(i);
      if (isBlankToken(text)) continue;
      Word word{};
      word.x = static_cast<int16_t>(line->xPos + block->wordXpos(i) + marginLeft);
      word.y = static_cast<int16_t>(line->yPos + marginTop + rubyShift);
      word.row = row;
      word.pageIdx = pageIdx;
      word.style = block->wordStyle(i);
      word.text = text;
      words.push_back(word);
      rowHasWords = true;
      pageText.append(text);
      pageText.push_back(' ');
      styleMask |= static_cast<uint8_t>(1u << (static_cast<uint8_t>(word.style) & 0x03));
    }
    if (rowHasWords) row++;
  }
  if (styleMask == 0) styleMask = 0x01;
  renderer.ensureSdCardFontReady(fontId, pageText.c_str(), styleMask);
  for (size_t i = firstNew; i < words.size(); i++) {
    words[i].width = static_cast<int16_t>(renderer.getTextAdvanceX(fontId, words[i].text, words[i].style));
  }

  LoadedPage loaded;
  loaded.startCp = page->visibleTextOffset;
  loaded.page = std::move(page);
  pages.push_back(std::move(loaded));
  return words.size() > firstNew;
}

int HighlightSelectActivity::rowCount(const int pageIdx) const {
  int rows = 0;
  for (const auto& w : words) {
    if (w.pageIdx == pageIdx) rows = std::max(rows, w.row + 1);
  }
  return rows;
}

int HighlightSelectActivity::closestInRow(const int pageIdx, const int row, const int centerX) const {
  int best = -1;
  int bestDistance = INT_MAX;
  for (int i = 0; i < static_cast<int>(words.size()); i++) {
    if (words[i].pageIdx != pageIdx || words[i].row != row) continue;
    const int distance = std::abs(words[i].x + words[i].width / 2 - centerX);
    if (distance < bestDistance) {
      bestDistance = distance;
      best = i;
    }
  }
  return best;
}

void HighlightSelectActivity::moveTo(const int index) {
  if (index < 0 || index >= static_cast<int>(words.size()) || index == cursor) return;
  cursor = index;
  requestUpdate();
}

void HighlightSelectActivity::moveVertical(const int direction) {
  const Word& current = words[cursor];
  const int centerX = current.x + current.width / 2;
  const int targetRow = current.row + direction;
  int pageIdx = current.pageIdx;
  int row = targetRow;
  if (targetRow < 0) {
    if (pageIdx == 0) return;
    pageIdx--;
    row = rowCount(pageIdx) - 1;
  } else if (targetRow >= rowCount(pageIdx)) {
    if (pageIdx + 1 >= static_cast<int>(pages.size()) && !loadNextPage()) return;
    pageIdx++;
    row = 0;
  }
  moveTo(closestInRow(pageIdx, row, centerX));
}

int HighlightSelectActivity::wordAt(const int x, const int y) const {
  constexpr int SLOP = 4;
  const int pageIdx = words.empty() ? 0 : words[cursor].pageIdx;
  for (int i = 0; i < static_cast<int>(words.size()); i++) {
    const Word& w = words[i];
    if (w.pageIdx != pageIdx) continue;
    if (x >= w.x - SLOP && x < w.x + w.width + SLOP && y >= w.y - SLOP && y < w.y + lineHeight + SLOP) return i;
  }
  return -1;
}

void HighlightSelectActivity::save() {
  const int from = std::min(anchor, cursor);
  const int to = std::max(anchor, cursor);
  out->words.clear();
  for (int i = from; i <= to; i++) out->words.emplace_back(words[i].text);

  // Page start plus the text before the first word: where the parser's
  // offsets put it, give or take source whitespace.
  const Word& first = words[from];
  out->pageStartCp = pages[first.pageIdx].startCp;
  out->wordsBefore.clear();
  uint32_t estimate = out->pageStartCp;
  for (int i = 0; i < from; i++) {
    if (words[i].pageIdx != first.pageIdx) continue;
    out->wordsBefore.emplace_back(words[i].text);
    estimate += static_cast<uint32_t>(codepointCount(words[i].text) + 1);
  }
  out->approxStartCp = estimate;
  out->saved = true;
  finish();
}

void HighlightSelectActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (anchor >= 0) {
      anchor = -1;
      requestUpdate();
    } else {
      finish();
    }
    return;
  }
  if (words.empty()) return;

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (anchor < 0) {
      anchor = cursor;
      requestUpdate();
    } else {
      save();
    }
    return;
  }

  int tx = 0;
  int ty = 0;
  if (mappedInput.wasScreenTouchDown(tx, ty)) {
    moveTo(wordAt(tx, ty));
    return;
  }
  if (mappedInput.wasScreenTapped(tx, ty)) {
    const int hit = wordAt(tx, ty);
    if (hit < 0) return;
    cursor = hit;
    if (anchor < 0) {
      anchor = hit;
      requestUpdate();
    } else {
      save();
    }
    return;
  }

  const unsigned long now = millis();
  const bool repeat =
      mappedInput.getHeldTime() >= WORD_REPEAT_START_MS && now - lastHorizontalMoveTime >= WORD_REPEAT_INTERVAL_MS;
  const bool moveLeft = mappedInput.wasPressed(MappedInputManager::Button::ScreenLeft) ||
                        (repeat && mappedInput.isPressed(MappedInputManager::Button::ScreenLeft));
  const bool moveRight = mappedInput.wasPressed(MappedInputManager::Button::ScreenRight) ||
                         (repeat && mappedInput.isPressed(MappedInputManager::Button::ScreenRight));
  if (moveLeft && cursor > 0) {
    lastHorizontalMoveTime = now;
    moveTo(cursor - 1);
  } else if (moveRight) {
    lastHorizontalMoveTime = now;
    if (cursor + 1 >= static_cast<int>(words.size()) && !loadNextPage()) return;
    moveTo(cursor + 1);
  } else if (mappedInput.wasPressed(MappedInputManager::Button::ScreenUp)) {
    moveVertical(-1);
  } else if (mappedInput.wasPressed(MappedInputManager::Button::ScreenDown)) {
    moveVertical(1);
  }
}

void HighlightSelectActivity::drawHints() const {
  if (words.empty()) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    return;
  }
  const auto labels =
      mappedInput.mapDirectionalLabels(tr(STR_BACK), anchor < 0 ? tr(STR_HL_START) : tr(STR_HL_SAVE), tr(STR_DIR_LEFT),
                                       tr(STR_DIR_RIGHT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void HighlightSelectActivity::render(RenderLock&&) {
  renderer.clearScreen();
  if (words.empty()) {
    if (!pages.empty()) pages.front().page->render(renderer, fontId, marginLeft, marginTop);
    drawHints();
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    return;
  }

  const int pageIdx = words[cursor].pageIdx;
  const Page& page = *pages[pageIdx].page;
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  page.render(renderer, fontId, marginLeft, marginTop);
  scope.endScanAndPrewarm();
  page.render(renderer, fontId, marginLeft, marginTop);

  // Selected range: underlined, like a saved highlight.
  const int from = anchor < 0 ? cursor : std::min(anchor, cursor);
  const int to = anchor < 0 ? cursor : std::max(anchor, cursor);
  for (int i = from; i <= to; i++) {
    const Word& w = words[i];
    if (w.pageIdx != pageIdx) continue;
    // Close the gap to the next selected word on the same line.
    int right = w.x + w.width;
    if (i < to && words[i + 1].pageIdx == pageIdx && words[i + 1].row == w.row) right = words[i + 1].x;
    const int y = w.y + lineHeight - UNDERLINE_THICKNESS;
    renderer.drawLine(w.x, y, right - 1, y, UNDERLINE_THICKNESS, true);
  }

  // Cursor: inverted word.
  const Word& c = words[cursor];
  renderer.fillRect(c.x - 2, c.y - 2, c.width + 4, lineHeight + 4, true);
  renderer.drawText(fontId, c.x, c.y, c.text, false, c.style);

  drawHints();
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
