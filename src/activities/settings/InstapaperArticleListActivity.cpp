#include "InstapaperArticleListActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <LibraryBuilder.h>
#include <Logging.h>
#include <WiFi.h>

#include <cstdio>
#include <cstring>
#include <new>

#include "ArticleEpubBuilder.h"
#include "CrossPointState.h"
#include "InstapaperStore.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/BookCacheUtils.h"
#include "util/StringUtils.h"

namespace fui = freeink::ui;

namespace {
constexpr char ARTICLE_DIR[] = "/Instapaper";
constexpr char HTML_TMP_PATH[] = "/.crosspoint/ip_article.html";
constexpr unsigned long PROGRESS_MIN_UPDATE_MS = 1500;
}  // namespace

InstapaperArticleListActivity::InstapaperArticleListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                             const char* folderId, const StrId titleId)
    : UiListActivity("InstapaperArticleList", renderer, mappedInput), folderId(folderId), titleId(titleId) {}

std::string InstapaperArticleListActivity::epubPathFor(const InstapaperBookmark& b) {
  // The id keeps two articles with the same title apart.
  char suffix[16];
  snprintf(suffix, sizeof(suffix), " [%lu].epub", static_cast<unsigned long>(b.id));
  return std::string(ARTICLE_DIR) + "/" + StringUtils::sanitizeFilename(b.title, 80) + suffix;
}

void InstapaperArticleListActivity::onEnter() {
  UiListActivity::onEnter();
  if (auto* fcm = renderer.getFontCacheManager()) fcm->releaseSdFontCaches();

  list.reset(new (std::nothrow) InstapaperListResult());
  if (!list) {
    showError(tr(STR_DOWNLOAD_FAILED), "Out of memory");
    return;
  }
  if (WiFi.status() == WL_CONNECTED) {
    loadList();
    return;
  }
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled) {
                             finish();
                             return;
                           }
                           loadList();
                         });
}

void InstapaperArticleListActivity::showError(const char* message, const char* detail) {
  {
    RenderLock lock(*this);
    state = ERROR;
    snprintf(errorMsg, sizeof(errorMsg), "%s%s%s", message, detail[0] ? " - " : "", detail);
  }
  requestUpdate(true);
}

void InstapaperArticleListActivity::loadList() {
  {
    RenderLock lock(*this);
    state = LOADING;
  }
  requestUpdateAndWait();
  WiFi.setSleep(false);

  const auto err = InstapaperClient::listBookmarks(folderId, *list);
  if (err != InstapaperClient::OK) {
    showError(InstapaperClient::errorString(err), InstapaperClient::lastErrorMessage());
    return;
  }
  if (list->count == 0) {
    showError(tr(STR_IP_NO_ARTICLES));
    return;
  }
  {
    RenderLock lock(*this);
    state = BROWSING;
  }
  activeNav().reset();
  moveSelectionTo(0);
}

void InstapaperArticleListActivity::startDownload(const int index) {
  const InstapaperBookmark& b = list->items[index];
  {
    RenderLock lock(*this);
    state = DOWNLOADING;
    downloadedBytes = 0;
    strlcpy(currentTitle, b.title, sizeof(currentTitle));
  }
  requestUpdateAndWait();

  unsigned long lastRepaintMs = 0;
  const auto err = InstapaperClient::downloadText(b.id, HTML_TMP_PATH, [this, &lastRepaintMs](const size_t bytes) {
    downloadedBytes = bytes;
    // The loop is blocked for the whole transfer; poll Back here to cancel.
    mappedInput.update(true);
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) return false;
    const unsigned long now = millis();
    if (now - lastRepaintMs >= PROGRESS_MIN_UPDATE_MS) {
      lastRepaintMs = now;
      requestUpdate(true);
    }
    return true;
  });
  if (err == InstapaperClient::CANCELLED) {
    {
      RenderLock lock(*this);
      state = BROWSING;
    }
    requestUpdate(true);
    return;
  }
  if (err != InstapaperClient::OK) {
    showError(InstapaperClient::errorString(err), InstapaperClient::lastErrorMessage());
    return;
  }

  {
    RenderLock lock(*this);
    state = CONVERTING;
  }
  requestUpdateAndWait();

  Storage.mkdir(ARTICLE_DIR);
  const std::string path = epubPathFor(b);
  EpubPackager::ArticleMeta meta;
  meta.title = b.title;
  meta.author = b.site;
  meta.identifier = "urn:instapaper:bookmark:" + std::to_string(b.id);
  const auto result = ArticleEpubBuilder::build(HTML_TMP_PATH, path.c_str(), meta);
  Storage.remove(HTML_TMP_PATH);
  if (result != ArticleEpubBuilder::Result::OK) {
    showError(tr(STR_DOWNLOAD_FAILED), ArticleEpubBuilder::resultString(result));
    return;
  }

  clearBookCache(path);
  library::markLibraryIndexDirty();
  {
    RenderLock lock(*this);
    savedPath = path;
    state = DONE;
  }
  requestUpdate(true);
}

int InstapaperArticleListActivity::listCount() const { return state == BROWSING && list ? list->count : 0; }

const char* InstapaperArticleListActivity::headerTitle() const { return I18N.get(titleId); }

void InstapaperArticleListActivity::activateIndex(const int index) {
  app.clearTapFlash();
  if (list && index >= 0 && index < list->count) startDownload(index);
}

void InstapaperArticleListActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  const int count = list ? list->count : 0;
  for (int i = 0; i < count; i++) {
    const InstapaperBookmark& b = list->items[i];
    rowSubtitles_[i] = b.site;
    if (b.progress > 0.01f) {
      char pct[16];
      snprintf(pct, sizeof(pct), "%s%d%%", rowSubtitles_[i].empty() ? "" : " \xC2\xB7 ",
               static_cast<int>(b.progress * 100.0f + 0.5f));
      rowSubtitles_[i] += pct;
    }
    rowItems_[i] = {};
    rowItems_[i].label = b.title;
    rowItems_[i].subtitle = rowSubtitles_[i].empty() ? nullptr : rowSubtitles_[i].c_str();
    rowItems_[i].actionValue = static_cast<int16_t>(i);
  }

  fui::ListProps props;
  props.items = rowItems_;
  props.count = static_cast<uint16_t>(count);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  syncListViewport(screen, props);
  screen.list(props);
}

void InstapaperArticleListActivity::drawFooter() {
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_DOWNLOAD), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void InstapaperArticleListActivity::loop() {
  int x = 0;
  int y = 0;
  switch (state) {
    case BROWSING:
      UiListActivity::loop();
      return;
    case DONE: {
      // Back (or the header back tap, which is also a screen tap) returns to
      // the list; Confirm or a tap anywhere else opens the article.
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        {
          RenderLock lock(*this);
          state = BROWSING;
        }
        requestUpdate(true);
        return;
      }
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(x, y)) {
        // Open via a silent restart into the reader: leaving the Instapaper
        // menus with WiFi up would otherwise reboot back to Settings.
        APP_STATE.openEpubPath = savedPath;
        APP_STATE.saveToFile();
        WiFi.disconnect(false);
        delay(30);
        silentRestartToReader();
      }
      return;
    }
    case ERROR:
      if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
          mappedInput.wasReleased(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(x, y)) {
        if (!list || list->count == 0) {
          finish();
          return;
        }
        {
          RenderLock lock(*this);
          state = BROWSING;
        }
        requestUpdate(true);
      }
      return;
    default:
      return;  // LOADING / DOWNLOADING / CONVERTING block in their own calls
  }
}

void InstapaperArticleListActivity::render(RenderLock&& lock) {
  if (state == BROWSING) {
    UiListActivity::render(std::move(lock));
    return;
  }

  renderer.clearScreen();
  drawChrome();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const char* backLabel = tr(STR_BACK);
  const char* confirmLabel = "";

  if (state == LOADING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_LOADING));
    backLabel = "";
  } else if (state == DOWNLOADING || state == CONVERTING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 40,
                              state == DOWNLOADING ? tr(STR_DOWNLOADING) : tr(STR_IP_CONVERTING));
    const auto title = renderer.truncatedText(UI_10_FONT_ID, currentTitle, pageWidth - 40);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 10, title.c_str());
    if (state == DOWNLOADING && downloadedBytes > 0) {
      char progressText[32];
      snprintf(progressText, sizeof(progressText), "%u KB", static_cast<unsigned>(downloadedBytes / 1024));
      renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 30, progressText);
    }
    backLabel = state == DOWNLOADING ? tr(STR_CANCEL) : "";
  } else if (state == DONE) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 15, tr(STR_IP_SAVED), true, EpdFontFamily::BOLD);
    const auto title = renderer.truncatedText(UI_10_FONT_ID, currentTitle, pageWidth - 40);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 15, title.c_str());
    confirmLabel = tr(STR_OPEN);
  } else if (state == ERROR) {
    const auto msg = renderer.truncatedText(UI_10_FONT_ID, errorMsg, pageWidth - 40);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, msg.c_str(), true, EpdFontFamily::BOLD);
  }

  const auto labels = mappedInput.mapLabels(backLabel, confirmLabel, "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
