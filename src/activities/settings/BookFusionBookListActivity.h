#pragma once

#include <cstddef>

#include "BookFusionSyncClient.h"
#include "activities/UiListActivity.h"

/**
 * One BookFusion library category, a page at a time. Choosing a book fetches
 * its pre-signed download URL, streams the file to the SD root, and writes a
 * BookFusion book-id sidecar (BookFusionBookIdStore) so progress sync works
 * as soon as the book is opened.
 *
 * Rows: [Previous page] books... [Next page]. The page rows only appear when
 * there is a page to go to.
 */
class BookFusionBookListActivity final : public UiListActivity {
 public:
  explicit BookFusionBookListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, int categoryIndex);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == CONNECTING || state == LOADING || state == DOWNLOADING; }

 private:
  enum State { CONNECTING, LOADING, BROWSING, DOWNLOADING, DOWNLOAD_COMPLETE, ERROR };

  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override;
  void drawFooter() override;

  void onWifiSelectionComplete(bool success);
  void loadPage(int page);
  void startDownload(int bookIndex);
  void showError(const char* message);
  bool hasPrevRow() const { return currentPage > 1; }
  int firstBookRow() const { return hasPrevRow() ? 1 : 0; }

  const int categoryIndex;
  State state = CONNECTING;

  BookFusionSearchResult searchResult;
  int currentPage = 1;

  static constexpr int MAX_ROWS = BookFusionSearchResult::MAX_BOOKS + 2;
  freeink::ui::ListItem rowItems_[MAX_ROWS]{};

  // Pre-signed S3 URLs from BookFusion run to ~1450+ chars; strlcpy in
  // getDownloadUrl silently truncates if this is too small.
  char downloadUrl[2048] = {};
  char downloadTitle[64] = {};
  size_t downloadProgress = 0;
  size_t downloadTotal = 0;
  bool cancelRequested = false;

  char errorMsg[128] = {};
};
