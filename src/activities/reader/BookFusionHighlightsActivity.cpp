#include "BookFusionHighlightsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <iterator>

#include "BookFusionAutoSync.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "fontIds.h"

namespace fui = freeink::ui;

namespace {
constexpr int ENTER_ACTIONS_MODE_MS = 700;
}  // namespace

BookFusionHighlightsActivity::BookFusionHighlightsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                           const std::string& epubPath, const uint32_t bookId)
    : UiListActivity("BookFusionHighlights", renderer, mappedInput, /*wantsTouchLongPress=*/true),
      epubPath(epubPath),
      bookId(bookId) {}

void BookFusionHighlightsActivity::onEnter() {
  UiListActivity::onEnter();
  store.load(epubPath);
  rebuildRows();
}

void BookFusionHighlightsActivity::rebuildRows() {
  rows = store.visible();
  labels.clear();
  rowItems.clear();
  labels.reserve(rows.size());
  rowItems.reserve(rows.size());
  std::transform(rows.begin(), rows.end(), std::back_inserter(labels), [](const BookFusionHighlight* hl) {
    return "\xE2\x80\x9C" + hl->quote + "\xE2\x80\x9D";  // “quote”
  });
  for (size_t i = 0; i < rows.size(); i++) {
    fui::ListItem item;
    item.label = labels[i].c_str();
    item.subtitle = rows[i]->chapterTitle.empty() ? nullptr : rows[i]->chapterTitle.c_str();
    item.actionValue = static_cast<int16_t>(i);
    rowItems.push_back(item);
  }
}

void BookFusionHighlightsActivity::cancel() {
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
}

void BookFusionHighlightsActivity::openSelected() {
  if (rows.empty() || nav.selected < 0 || nav.selected >= listCount()) return;
  const BookFusionHighlight& hl = *rows[nav.selected];
  ProgressChangeResult result{};
  result.spineIndex = hl.chapterIndex;
  result.hasVisibleTextOffset = true;
  result.visibleTextOffset = hl.cpStart;
  setResult(std::move(result));
  finish();
}

void BookFusionHighlightsActivity::activateIndex(const int index) {
  if (popup.isActive() || index < 0 || index >= listCount()) return;
  app.clearTapFlash();
  nav.selected = index;
  openSelected();
}

void BookFusionHighlightsActivity::onRowLongPress(const int index) {
  if (popup.isActive() || index < 0 || index >= listCount()) return;
  app.clearTapFlash();
  nav.selected = index;
  showActions();
}

bool BookFusionHighlightsActivity::handleCustomInput() {
  if (popup.handleInput(mappedInput, [this] { requestUpdate(); })) return true;
  if (confirmingDelete) {  // dismissed without choosing
    confirmingDelete = false;
    requestUpdate();
    return true;
  }
  return false;
}

bool BookFusionHighlightsActivity::handleButtons() {
  if (mappedInput.wasLongPressed(MappedInputManager::Button::Confirm, ENTER_ACTIONS_MODE_MS)) {
    showActions();
    return true;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    cancel();
    return true;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    openSelected();
    return true;
  }
  return false;
}

void BookFusionHighlightsActivity::showActions() {
  if (rows.empty() || popup.isActive()) return;
  const StrId options[] = {StrId::STR_OPEN, StrId::STR_DELETE};
  popup.show(StrId::STR_HL_HIGHLIGHTS, options, 2, 0, [this](const int idx) {
    if (idx == 0) {
      openSelected();
    } else if (idx == 1) {
      showDeleteConfirmation();
    }
  });
  requestUpdate();
}

void BookFusionHighlightsActivity::showDeleteConfirmation() {
  if (rows.empty() || popup.isActive()) return;
  confirmingDelete = true;
  const char* options[] = {tr(STR_CANCEL), tr(STR_DELETE)};
  popup.show(tr(STR_HL_CONFIRM_DELETE), options, 2, 0, [this](const int idx) {
    confirmingDelete = false;
    if (idx == 1) deleteSelected();
    requestUpdate();
  });
  requestUpdate();
}

void BookFusionHighlightsActivity::deleteSelected() {
  if (nav.selected < 0 || nav.selected >= listCount()) return;
  store.remove(rows[nav.selected]->localId);
  // remove() may reallocate: rebuild the row pointers before the next render.
  rebuildRows();
  if (!store.save()) LOG_ERR("BFHL", "Failed to save highlights after delete");
  if (store.hasPending()) BookFusionAutoSync::noteHighlightsPending(epubPath, bookId);  // remote delete
  if (nav.selected >= listCount() && nav.selected > 0) nav.selected--;
  if (rows.empty()) {
    cancel();
    return;
  }
  nav.follow(listCount());
  requestUpdate(true);
}

void BookFusionHighlightsActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height)), static_cast<int16_t>(safe.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  if (rows.empty()) {
    screen.centeredText(tr(STR_HL_NONE), screen.theme().bodyText);
    return;
  }
  if (!mappedInput.hasTouch()) {
    const int helpLineHeight = renderer.getLineHeight(SMALL_FONT_ID);
    const fui::Rect band = screen.takeBottom(static_cast<int16_t>(helpLineHeight + metrics.verticalSpacing));
    GUI.drawHelpText(renderer, Rect{band.x, band.y + metrics.verticalSpacing, band.width, helpLineHeight},
                     tr(STR_HOLD_OPEN_FOR_ACTIONS));
  }

  fui::ListProps props;
  props.items = rowItems.data();
  props.count = static_cast<uint16_t>(rowItems.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch | fui::InputLongPress;
  props.labelText = screen.theme().bodyText;
  props.labelText.maxLines = 2;
  syncListViewport(screen, props);
  screen.list(props);
}

void BookFusionHighlightsActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  GUI.drawHeader(renderer, Rect{safe.x, safe.y + metrics.topPadding, safe.width, metrics.headerHeight},
                 tr(STR_HL_HIGHLIGHTS));
  renderUi();
  if (popup.processRender(renderer, mappedInput)) return;
  const auto hints =
      mappedInput.mapLabels(tr(STR_BACK), rows.empty() ? "" : tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, hints.btn1, hints.btn2, hints.btn3, hints.btn4);
  renderer.displayBuffer();
}
