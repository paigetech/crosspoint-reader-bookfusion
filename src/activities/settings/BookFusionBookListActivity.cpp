#include "BookFusionBookListActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <LibraryBuilder.h>
#include <Logging.h>
#include <WiFi.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>

#include "BookFusionBookIdStore.h"
#include "BookFusionLibraryActivity.h"
#include "BookFusionTokenStore.h"
#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "util/BookCacheUtils.h"
#include "util/StringUtils.h"

namespace fui = freeink::ui;

namespace {
// Download progress repaints are throttled: every e-ink refresh stalls the transfer.
constexpr unsigned long PROGRESS_MIN_UPDATE_MS = 1500;
}  // namespace

BookFusionBookListActivity::BookFusionBookListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                       const int categoryIndex)
    : UiListActivity("BookFusionBookList", renderer, mappedInput), categoryIndex(categoryIndex) {}

void BookFusionBookListActivity::onEnter() {
  UiListActivity::onEnter();

  // The TLS session and download need the heap more than cached SD glyphs do.
  if (auto* fcm = renderer.getFontCacheManager()) {
    fcm->releaseSdFontCaches();
  }

  if (!BF_TOKEN_STORE.hasToken()) {
    showError(tr(STR_BF_NO_TOKEN_MSG));
    return;
  }

  // Kept alive across pages (and re-entries); BookFusionSettingsActivity ends it.
  BookFusionSyncClient::beginSession();

  if (WiFi.status() == WL_CONNECTED) {
    loadPage(1);
    return;
  }

  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void BookFusionBookListActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    finish();
    return;
  }
  WiFi.setSleep(false);
  loadPage(1);
}

void BookFusionBookListActivity::showError(const char* message) {
  {
    RenderLock lock(*this);
    state = ERROR;
    strlcpy(errorMsg, message, sizeof(errorMsg));
  }
  requestUpdate(true);
}

void BookFusionBookListActivity::loadPage(const int page) {
  {
    RenderLock lock(*this);
    state = LOADING;
  }
  requestUpdateAndWait();

  const auto& cat = BookFusionLibraryActivity::category(categoryIndex);
  const auto err = BookFusionSyncClient::searchBooks(page, searchResult, cat.list, cat.sort);
  if (err != BookFusionSyncClient::OK) {
    searchResult.count = 0;
    showError(BookFusionSyncClient::errorString(err));
    return;
  }
  if (searchResult.count == 0) {
    showError(tr(STR_BF_NO_BOOKS));
    return;
  }

  {
    RenderLock lock(*this);
    currentPage = page;
    state = BROWSING;
  }
  activeNav().reset();
  moveSelectionTo(firstBookRow());
}

void BookFusionBookListActivity::startDownload(const int bookIndex) {
  const BookFusionBook& book = searchResult.books[bookIndex];

  {
    RenderLock lock(*this);
    state = DOWNLOADING;
    downloadProgress = 0;
    downloadTotal = 0;
    strlcpy(downloadTitle, book.title, sizeof(downloadTitle));
  }
  requestUpdateAndWait();

  const auto urlErr = BookFusionSyncClient::getDownloadUrl(book.id, downloadUrl, sizeof(downloadUrl));
  if (urlErr != BookFusionSyncClient::OK) {
    showError(urlErr == BookFusionSyncClient::NOT_FOUND ? tr(STR_BF_BOOK_UNAVAILABLE)
                                                        : BookFusionSyncClient::errorString(urlErr));
    return;
  }

  // Destination: "/Title - Author.ext" (sanitized) on the SD root.
  std::string baseName = book.title;
  if (book.authors[0] != '\0') {
    baseName += " - ";
    baseName += book.authors;
  }
  char ext[8] = "epub";
  if (book.format[0] != '\0') {
    size_t i = 0;
    for (; i < sizeof(ext) - 1 && book.format[i] != '\0'; i++) {
      ext[i] = static_cast<char>(tolower(static_cast<unsigned char>(book.format[i])));
    }
    ext[i] = '\0';
  }
  const std::string filename = "/" + StringUtils::sanitizeFilename(baseName) + "." + ext;
  LOG_DBG("BFB", "Downloading book_id=%lu -> %s (heap %u free, %u max block)", static_cast<unsigned long>(book.id),
          filename.c_str(), ESP.getFreeHeap(), ESP.getMaxAllocHeap());

  if (ESP.getFreeHeap() < HttpDownloader::MIN_TLS_FREE_HEAP ||
      ESP.getMaxAllocHeap() < HttpDownloader::MIN_TLS_MAX_ALLOC) {
    LOG_ERR("BFB", "Low heap for download");
    showError(tr(STR_DOWNLOAD_FAILED));
    return;
  }

  cancelRequested = false;
  unsigned long lastRepaintMs = 0;
  const auto dlResult = HttpDownloader::downloadToFile(
      downloadUrl, filename,
      [this, &lastRepaintMs](const size_t downloaded, const size_t total) {
        downloadProgress = downloaded;
        downloadTotal = total;
        // The activity loop is blocked for the whole download; pump input here
        // so Back (or the header back tap) can cancel mid-transfer.
        mappedInput.update(true);
        if (mappedInput.wasReleased(MappedInputManager::Button::Back)) cancelRequested = true;
        const unsigned long now = millis();
        if (lastRepaintMs == 0 || now - lastRepaintMs >= PROGRESS_MIN_UPDATE_MS || (total > 0 && downloaded >= total)) {
          lastRepaintMs = now;
          requestUpdate(true);
        }
      },
      &cancelRequested);

  if (dlResult == HttpDownloader::ABORTED) {
    LOG_INF("BFB", "Download cancelled");
    {
      RenderLock lock(*this);
      state = BROWSING;
    }
    requestUpdate(true);
    return;
  }
  if (dlResult != HttpDownloader::OK) {
    showError(tr(STR_DOWNLOAD_FAILED));
    return;
  }

  // The sidecar is what makes the book syncable from the reader menu.
  BookFusionBookIdStore::saveBookId(filename.c_str(), book.id);
  clearBookCache(filename);
  library::markLibraryIndexDirty();
  LOG_DBG("BFB", "Download complete, sidecar saved for book_id=%lu", static_cast<unsigned long>(book.id));

  {
    RenderLock lock(*this);
    state = DOWNLOAD_COMPLETE;
  }
  requestUpdate(true);
}

int BookFusionBookListActivity::listCount() const {
  if (state != BROWSING) return 0;
  return firstBookRow() + searchResult.count + (searchResult.hasMore ? 1 : 0);
}

const char* BookFusionBookListActivity::headerTitle() const {
  return I18N.get(BookFusionLibraryActivity::category(categoryIndex).nameId);
}

void BookFusionBookListActivity::activateIndex(const int index) {
  app.clearTapFlash();
  if (hasPrevRow() && index == 0) {
    loadPage(currentPage - 1);
    return;
  }
  const int bookIndex = index - firstBookRow();
  if (bookIndex >= 0 && bookIndex < searchResult.count) {
    startDownload(bookIndex);
  } else if (searchResult.hasMore) {
    loadPage(currentPage + 1);
  }
}

void BookFusionBookListActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  int row = 0;
  if (hasPrevRow()) {
    rowItems_[row] = {};
    rowItems_[row].label = tr(STR_PREV_PAGE);
    rowItems_[row].actionValue = static_cast<int16_t>(row);
    row++;
  }
  for (int i = 0; i < searchResult.count; i++, row++) {
    rowItems_[row] = {};
    rowItems_[row].label = searchResult.books[i].title;
    rowItems_[row].subtitle = searchResult.books[i].authors[0] != '\0' ? searchResult.books[i].authors : nullptr;
    rowItems_[row].actionValue = static_cast<int16_t>(row);
  }
  if (searchResult.hasMore) {
    rowItems_[row] = {};
    rowItems_[row].label = tr(STR_NEXT_PAGE);
    rowItems_[row].actionValue = static_cast<int16_t>(row);
    row++;
  }

  fui::ListProps props;
  props.items = rowItems_;
  props.count = static_cast<uint16_t>(row);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  syncListViewport(screen, props);
  screen.list(props);
}

void BookFusionBookListActivity::drawFooter() {
  const int bookIndex = activeNav().selected - firstBookRow();
  const bool onBook = bookIndex >= 0 && bookIndex < searchResult.count;
  const auto labels =
      mappedInput.mapLabels(tr(STR_BACK), onBook ? tr(STR_DOWNLOAD) : tr(STR_OPEN), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void BookFusionBookListActivity::loop() {
  switch (state) {
    case BROWSING:
      UiListActivity::loop();
      return;
    case ERROR:
    case DOWNLOAD_COMPLETE: {
      int x = 0;
      int y = 0;
      if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
          mappedInput.wasReleased(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(x, y)) {
        if (searchResult.count == 0) {
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
    }
    default:
      return;  // CONNECTING / LOADING / DOWNLOADING block in their own calls
  }
}

void BookFusionBookListActivity::render(RenderLock&& lock) {
  if (state == BROWSING) {
    UiListActivity::render(std::move(lock));
    return;
  }

  renderer.clearScreen();
  drawChrome();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const char* backLabel = tr(STR_BACK);

  if (state == CONNECTING || state == LOADING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_LOADING));
    backLabel = "";
  } else if (state == ERROR) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, errorMsg, true, EpdFontFamily::BOLD);
  } else if (state == DOWNLOADING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 40, tr(STR_DOWNLOADING));
    const auto title = renderer.truncatedText(UI_10_FONT_ID, downloadTitle, pageWidth - 40);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 10, title.c_str());
    if (downloadTotal > 0) {
      GUI.drawProgressBar(renderer, Rect{50, pageHeight / 2 + 20, pageWidth - 100, 20}, downloadProgress,
                          downloadTotal);
    } else if (downloadProgress > 0) {
      // Chunked transfer: no Content-Length, so show a byte counter instead of a bar.
      char progressText[32];
      const float kb = downloadProgress / 1024.0f;
      if (kb < 1024.0f) {
        snprintf(progressText, sizeof(progressText), "%.0f KB", kb);
      } else {
        snprintf(progressText, sizeof(progressText), "%.1f MB", kb / 1024.0f);
      }
      renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 30, progressText);
    }
    backLabel = tr(STR_CANCEL);
  } else if (state == DOWNLOAD_COMPLETE) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 15, tr(STR_BF_DOWNLOAD_COMPLETE), true,
                              EpdFontFamily::BOLD);
    const auto title = renderer.truncatedText(UI_10_FONT_ID, downloadTitle, pageWidth - 40);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 15, title.c_str());
  }

  const auto labels = mappedInput.mapLabels(backLabel, "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
