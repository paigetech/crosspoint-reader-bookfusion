#pragma once

#include <I18n.h>

#include <cstddef>
#include <memory>
#include <string>

#include "InstapaperClient.h"
#include "activities/UiListActivity.h"

/**
 * One Instapaper folder (Unread / Starred / Archive). Choosing an article
 * downloads Instapaper's text version, converts it to an EPUB in
 * /Instapaper/ and offers to open it.
 */
class InstapaperArticleListActivity final : public UiListActivity {
 public:
  explicit InstapaperArticleListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const char* folderId,
                                         StrId titleId);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == LOADING || state == DOWNLOADING || state == CONVERTING; }

 private:
  enum State { LOADING, BROWSING, DOWNLOADING, CONVERTING, DONE, ERROR };

  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override;
  void drawFooter() override;

  void loadList();
  void startDownload(int index);
  void showError(const char* message, const char* detail = "");
  static std::string epubPathFor(const InstapaperBookmark& b);

  const char* folderId;
  const StrId titleId;
  State state = LOADING;

  // ~6 KB: heap-allocated, only while this screen is open.
  std::unique_ptr<InstapaperListResult> list;
  std::string rowSubtitles_[InstapaperListResult::MAX];
  freeink::ui::ListItem rowItems_[InstapaperListResult::MAX]{};

  char currentTitle[96] = {};
  std::string savedPath;
  size_t downloadedBytes = 0;
  char errorMsg[192] = {};
};
