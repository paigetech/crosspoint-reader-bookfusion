#include "BookFusionSyncActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_wifi.h>

#include <cassert>
#include <cstdio>

#include "BookFusionBookIdStore.h"
#include "BookFusionTokenStore.h"
#include "EpubReaderUtils.h"
#include "MappedInputManager.h"
#include "ProgressComparison.h"
#include "ReaderUtils.h"
#include "SilentRestart.h"
#include "activities/ActivityManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "fontIds.h"

namespace fui = freeink::ui;

namespace {
// The compare rows (SHOWING_RESULT) and the upload row (NO_REMOTE_PROGRESS)
// never coexist, so state disambiguates them in the handler.
constexpr fui::ActionId ACTION_ROW = 1;
}  // namespace

uint32_t BookFusionSyncActivity::syncableBookId(const std::string& epubPath) {
  if (!BF_TOKEN_STORE.hasToken()) return 0;
  return BookFusionBookIdStore::loadBookId(epubPath.c_str());
}

BookFusionSyncActivity::BookFusionSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                               const std::string& epubPath, const uint32_t bookId,
                                               const CrossPointPosition localPosition, const float localPercentage,
                                               const int spineCount, std::string localChapterName)
    : Activity("BookFusionSync", renderer, mappedInput),
      UiAppHost(renderer),
      epubPath(epubPath),
      bookId(bookId),
      localPosition(localPosition),
      localPercentage(localPercentage),
      spineCount(spineCount),
      localChapterName(std::move(localChapterName)) {}

void BookFusionSyncActivity::ensureEpubLoaded() {
  if (epub) return;
  LOG_DBG("BFSync", "Loading epub for progress mapping (heap: %u)", (unsigned)ESP.getFreeHeap());
  epub = std::make_shared<Epub>(epubPath, "/.crosspoint");
  epub->setupCacheDir();
  if (!epub->load(false, true)) {
    LOG_ERR("BFSync", "Failed to load epub for progress mapping");
    epub.reset();
  }
}

void BookFusionSyncActivity::saveProgressAndReturn(const int spineIndex, const int page) {
  assert(epub);
  if (!EpubReaderUtils::saveProgress(*epub, spineIndex, page, 0)) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = tr(STR_SAVE_PROGRESS_FAILED);
    }
    requestUpdate(true);
    return;
  }
  returnToReader();
}

void BookFusionSyncActivity::returnToReader() { activityManager.goToReader(epubPath); }

void BookFusionSyncActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    LOG_DBG("BFSync", "WiFi connection failed, exiting");
    returnToReader();
    return;
  }

  // Modem sleep can stall the short TLS exchange; WiFi is torn down on exit.
  WiFi.setSleep(false);

  {
    RenderLock lock(*this);
    state = SYNCING;
    statusMessage = tr(STR_FETCH_PROGRESS);
  }
  requestUpdateAndWait();

  performSync();
}

void BookFusionSyncActivity::performSync() {
  const auto result = BookFusionSyncClient::getProgress(bookId, remoteBfPosition);

  if (result == BookFusionSyncClient::NOT_FOUND) {
    {
      RenderLock lock(*this);
      state = NO_REMOTE_PROGRESS;
    }
    requestUpdate(true);
    return;
  }

  if (result != BookFusionSyncClient::OK) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = BookFusionSyncClient::errorString(result);
    }
    requestUpdate(true);
    return;
  }

  // The Epub was released before sync to free RAM for the TLS handshake.
  ensureEpubLoaded();
  if (!epub) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = "";
    }
    requestUpdate(true);
    return;
  }

  const float remotePercentage = remoteBfPosition.percentage / 100.0f;
  {
    RenderLock lock;
    GfxRenderer::FrameBufferLoan loan(renderer);
    // BookFusion only exposes a percentage (no XPath), so map by percentage.
    const SavedProgressPosition saved = {"", remotePercentage};
    remotePosition =
        ProgressMapper::toCrossPoint(epub, saved, renderer, localPosition.spineIndex, localPosition.totalPages);
  }

  const ProgressComparison comparison =
      compareProgress(localPosition, localPercentage, remotePosition, remotePercentage);
  LOG_DBG("BFSync", "local=%.4f remote=%.4f mapped=%d/%d comparison=%d", localPercentage, remotePercentage,
          remotePosition.spineIndex, remotePosition.pageNumber, static_cast<int>(comparison));

  {
    RenderLock lock(*this);
    state = SHOWING_RESULT;
    selectedOption = comparison == ProgressComparison::LocalAhead ? 1 : 0;
  }
  requestUpdate(true);
}

void BookFusionSyncActivity::performUpload() {
  {
    RenderLock lock(*this);
    state = UPLOADING;
    statusMessage = tr(STR_UPLOAD_PROGRESS);
  }
  requestUpdateAndWait();

  // Nothing below needs the Epub; free its heap before the TLS handshake.
  epub.reset();

  BookFusionPosition pos;
  pos.percentage = localPercentage * 100.0f;
  pos.chapterIndex = localPosition.spineIndex;
  const float intraSpine =
      localPosition.totalPages > 0 ? static_cast<float>(localPosition.pageNumber) / localPosition.totalPages : 0.0f;
  pos.pagePositionInBook =
      spineCount > 0 ? (localPosition.spineIndex + intraSpine) / static_cast<float>(spineCount) : 0.0f;

  const auto result = BookFusionSyncClient::setProgress(bookId, pos);

  // Drop the radio while the user reads the result; full teardown happens at silent reboot.
  esp_wifi_stop();

  {
    RenderLock lock(*this);
    if (result != BookFusionSyncClient::OK) {
      state = SYNC_FAILED;
      statusMessage = BookFusionSyncClient::errorString(result);
    } else {
      state = UPLOAD_COMPLETE;
    }
  }
  requestUpdate(true);
}

void BookFusionSyncActivity::onEnter() {
  Activity::onEnter();
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

  resetUi();
  app.on(ACTION_ROW, &BookFusionSyncActivity::onResultRow, this);
  app.setScreen(&BookFusionSyncActivity::resultScreen, this);

  wifiActivated = true;

  if (WiFi.status() == WL_CONNECTED) {
    onWifiSelectionComplete(true);
    return;
  }

  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void BookFusionSyncActivity::onExit() {
  Activity::onExit();

  if (wifiActivated) {
    WiFi.disconnect(false);
    delay(30);
    silentRestartToReader();
  }
}

void BookFusionSyncActivity::chooseResultOption() {
  if (selectedOption == 0) {
    saveProgressAndReturn(remotePosition.spineIndex, remotePosition.pageNumber);
  } else {
    performUpload();
  }
}

void BookFusionSyncActivity::onResultRow(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<BookFusionSyncActivity*>(user);
  self->app.clearTapFlash();
  if (self->state == SHOWING_RESULT) {
    if (event.value < 0 || event.value > 1) return;
    self->selectedOption = event.value;
    self->chooseResultOption();
  } else if (self->state == NO_REMOTE_PROGRESS) {
    self->performUpload();
  }
}

void BookFusionSyncActivity::resultScreen(UiScreen& screen, void* user) {
  static_cast<BookFusionSyncActivity*>(user)->buildResultScreen(screen);
}

void BookFusionSyncActivity::buildResultScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  // Non-touch hardware (X3/X4) keeps the denser row height, as in KOReaderSyncActivity.
  int16_t actionRowHeight = screen.theme().rowHeight;
  const bool denseRows = !mappedInput.hasTouch();
  if (denseRows) {
    actionRowHeight = static_cast<int16_t>(metrics.listRowHeight);
  }

  if (state == SHOWING_RESULT) {
    const int remoteTocIndex = epub->getTocIndexForSpineIndex(remotePosition.spineIndex);
    const std::string remoteChapter =
        (remoteTocIndex >= 0) ? epub->getTocItem(remoteTocIndex).title
                              : (std::string(tr(STR_SECTION_PREFIX)) + std::to_string(remotePosition.spineIndex + 1));
    char localChapterFallback[32];
    const char* localChapter = localChapterName.c_str();
    if (localChapterName.empty()) {
      snprintf(localChapterFallback, sizeof(localChapterFallback), "%s%d", tr(STR_SECTION_PREFIX),
               localPosition.spineIndex + 1);
      localChapter = localChapterFallback;
    }

    char remoteVal[64];
    snprintf(remoteVal, sizeof(remoteVal), tr(STR_PAGE_OVERALL_FORMAT), remotePosition.pageNumber + 1,
             remoteBfPosition.percentage);
    char localVal[64];
    snprintf(localVal, sizeof(localVal), tr(STR_PAGE_TOTAL_OVERALL_FORMAT), localPosition.pageNumber + 1,
             localPosition.totalPages, localPercentage * 100);

    auto labelStyle = screen.theme().bodyText;
    labelStyle.bold = true;
    auto detailStyle = screen.theme().smallText;
    const int16_t labelH = screen.target().lineHeight(labelStyle.font);
    const int16_t detailH = screen.target().lineHeight(detailStyle.font);
    const int16_t labelIndent = static_cast<int16_t>(screen.theme().listInset + screen.theme().listSidePadding);
    const int16_t detailIndent = static_cast<int16_t>(labelIndent + screen.theme().spaceMd);
    const auto textLine = [&](const char* text, const fui::TextStyle& style, int16_t height, int16_t indent,
                              int16_t gap) {
      fui::Rect r = screen.takeTop(height, gap);
      r.x = static_cast<int16_t>(r.x + indent);
      r.width = static_cast<int16_t>(r.width - indent);
      screen.target().text(r, text, style);
    };
    const auto labelLine = [&](const char* text) {
      textLine(text, labelStyle, labelH, labelIndent, screen.theme().spaceSm);
    };
    const auto detailLine = [&](const char* text) {
      textLine(text, detailStyle, detailH, detailIndent, screen.theme().spaceXs);
    };

    labelLine(tr(STR_REMOTE_LABEL));
    detailLine(remoteChapter.c_str());
    detailLine(remoteVal);
    screen.spacer(screen.theme().spaceLg);
    labelLine(tr(STR_LOCAL_LABEL));
    detailLine(localChapter);
    detailLine(localVal);

    screen.spacer(screen.theme().spaceMd);
    fui::ListItem actions[2];
    actions[0].label = tr(STR_APPLY_REMOTE);
    actions[0].icon = fui::bitmapFromIcon(icon_download_24);
    actions[0].actionValue = 0;
    actions[1].label = tr(STR_UPLOAD_LOCAL);
    actions[1].icon = fui::bitmapFromIcon(icon_upload_24);
    actions[1].actionValue = 1;
    fui::ListProps actionProps;
    actionProps.items = actions;
    actionProps.count = 2;
    actionProps.selectedIndex = static_cast<int16_t>(selectedOption);
    actionProps.action = ACTION_ROW;
    actionProps.inputMask = fui::InputTouch;  // physical buttons stay in loop()
    actionProps.scrollIndicator = false;
    if (denseRows) actionProps.rowHeight = actionRowHeight;
    const auto actionsBand =
        static_cast<int16_t>(actionRowHeight * 2 + screen.theme().listRowGap + screen.theme().spaceSm);
    screen.list(actionProps, actionsBand);
    return;
  }

  if (state == NO_REMOTE_PROGRESS) {
    auto centered = screen.theme().bodyText;
    centered.align = fui::TextAlign::Center;
    auto centeredBold = centered;
    centeredBold.bold = true;
    const int16_t lineH = screen.target().lineHeight(centered.font);
    screen.target().text(screen.takeTop(lineH, screen.theme().spaceSm), tr(STR_NO_REMOTE_MSG), centeredBold);
    screen.target().text(screen.takeTop(lineH, screen.theme().spaceMd), tr(STR_UPLOAD_PROMPT), centered);

    fui::ListItem action;
    action.label = tr(STR_UPLOAD_LOCAL);
    action.actionValue = 0;
    fui::ListProps actionProps;
    actionProps.items = &action;
    actionProps.count = 1;
    actionProps.selectedIndex = 0;
    actionProps.action = ACTION_ROW;
    actionProps.inputMask = fui::InputTouch;
    actionProps.scrollIndicator = false;
    if (denseRows) actionProps.rowHeight = actionRowHeight;
    const auto actionsBand = static_cast<int16_t>(actionRowHeight + screen.theme().spaceMd);
    screen.list(actionProps, actionsBand, fui::LayoutAnchor::Bottom);
  }
}

void BookFusionSyncActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);

  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 state == SHOWING_RESULT ? tr(STR_PROGRESS_FOUND) : tr(STR_BF_SYNC));

  const int top = screen.y + screen.height / 2 - 40;

  if (state == SYNCING || state == UPLOADING) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top, statusMessage.c_str(), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  if (state == SHOWING_RESULT) {
    renderUi();
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == NO_REMOTE_PROGRESS) {
    renderUi();
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_UPLOAD), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == UPLOAD_COMPLETE) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top, tr(STR_UPLOAD_SUCCESS), true, EpdFontFamily::BOLD);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_DONE), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == SYNC_FAILED) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top, tr(STR_SYNC_FAILED_MSG), true, EpdFontFamily::BOLD);
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top + 40, statusMessage.c_str());
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  renderer.displayBuffer();
}

void BookFusionSyncActivity::loop() {
  if (state == SYNC_FAILED || state == UPLOAD_COMPLETE) {
    int x = 0;
    int y = 0;
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(x, y)) {
      returnToReader();
    }
    return;
  }

  if (state == SHOWING_RESULT) {
    const auto route = routeTouch(mappedInput);
    if (route.routed && app.invalidated()) requestUpdate();
    if (route) return;  // dispatched to onResultRow

    if (mappedInput.wasReleased(MappedInputManager::Button::Up) ||
        mappedInput.wasReleased(MappedInputManager::Button::Left) ||
        mappedInput.wasReleased(MappedInputManager::Button::Down) ||
        mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      selectedOption = (selectedOption + 1) % 2;
      requestUpdate();
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      chooseResultOption();
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      returnToReader();
    }
    return;
  }

  if (state == NO_REMOTE_PROGRESS) {
    const auto route = routeTouch(mappedInput);
    if (route.routed && app.invalidated()) requestUpdate();
    if (route) return;  // dispatched to onResultRow -> performUpload

    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      performUpload();
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      returnToReader();
    }
  }
}
