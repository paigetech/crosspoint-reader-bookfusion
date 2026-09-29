#pragma once
#include <Epub.h>

#include <cstdint>
#include <memory>
#include <string>

#include "BookFusionSyncClient.h"
#include "ProgressMapper.h"
#include "activities/Activity.h"
#include "components/UiAppHost.h"

/**
 * Syncs reading progress with BookFusion.
 *
 * Only books downloaded from BookFusion (which have a book_id sidecar) can be
 * synced. EpubReaderActivity releases the Epub before replacing itself with
 * this activity (to free heap for TLS), exactly like KOReaderSyncActivity, and
 * the reader is reopened via a silent restart on exit.
 *
 * Flow:
 *  1. Connect WiFi
 *  2. Fetch remote position
 *  3. Show comparison + options (Apply remote / Upload local)
 *  4. Apply (save progress, reopen reader) or upload
 */
class BookFusionSyncActivity final : public Activity, private UiAppHost {
 public:
  // BookFusion book id for epubPath, or 0 when the account is not linked or
  // the book was not downloaded from BookFusion.
  static uint32_t syncableBookId(const std::string& epubPath);

  explicit BookFusionSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& epubPath,
                                  uint32_t bookId, const CrossPointPosition& localPosition, float localPercentage,
                                  int spineCount, std::string localChapterName);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == CONNECTING || state == SYNCING || state == UPLOADING; }

 private:
  enum State {
    WIFI_SELECTION,
    CONNECTING,
    SYNCING,
    SHOWING_RESULT,
    UPLOADING,
    UPLOAD_COMPLETE,
    NO_REMOTE_PROGRESS,
    SYNC_FAILED,
  };

  std::shared_ptr<Epub> epub;  // null until lazy-loaded after TLS in performSync()
  std::string epubPath;
  uint32_t bookId;
  CrossPointPosition localPosition;
  float localPercentage;  // 0.0-1.0, pre-computed before the Epub was released
  int spineCount;
  std::string localChapterName;

  State state = WIFI_SELECTION;
  std::string statusMessage;

  BookFusionPosition remoteBfPosition;
  CrossPointPosition remotePosition{};

  int selectedOption = 0;  // 0 = Apply remote, 1 = Upload local
  bool wifiActivated = false;

  void onWifiSelectionComplete(bool success);
  void performSync();
  void performUpload();
  void ensureEpubLoaded();
  void saveProgressAndReturn(int spineIndex, int page);
  void returnToReader();
  void chooseResultOption();

  static void resultScreen(UiScreen& screen, void* user);
  static void onResultRow(const freeink::ui::ActionEvent& event, void* user);
  void buildResultScreen(UiScreen& screen);
};
