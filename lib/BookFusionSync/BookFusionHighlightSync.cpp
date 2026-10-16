#include "BookFusionHighlightSync.h"

#include <Logging.h>

#include "BookFusionHighlightStore.h"

namespace BookFusionHighlightSync {

Result pushPending(const std::string& epubPath, const uint32_t bookId) {
  Result result;
  BookFusionHighlightStore store;
  if (!store.load(epubPath) || !store.hasPending()) return result;

  BookFusionSyncClient::beginSession();
  for (auto& hl : store.all()) {
    if (!hl.needsPush()) continue;
    BookFusionSyncClient::Error err;
    if (hl.deleted) {
      err = BookFusionSyncClient::deleteHighlight(hl.remoteId);
      if (err == BookFusionSyncClient::OK) {
        hl.remoteId.clear();  // tombstone done, dropped below
        result.deleted++;
      }
    } else {
      BookFusionHighlightPayload payload;
      payload.bookId = bookId;
      payload.chapterIndex = hl.chapterIndex;
      payload.startOffset = hl.u16Start;
      payload.endOffset = hl.u16End;
      payload.quote = hl.quote.c_str();
      payload.prefix = hl.prefix.c_str();
      payload.suffix = hl.suffix.c_str();
      payload.chapterTitle = hl.chapterTitle.c_str();
      payload.addedAt = hl.addedAt.c_str();
      err = BookFusionSyncClient::createHighlight(payload, hl.remoteId);
      if (err == BookFusionSyncClient::OK) result.created++;
    }
    if (err != BookFusionSyncClient::OK) {
      LOG_ERR("BFHL", "Highlight %lu push failed: %s", static_cast<unsigned long>(hl.localId),
              BookFusionSyncClient::errorString(err));
      if (result.failed++ == 0) result.error = err;
      if (err == BookFusionSyncClient::AUTH_FAILED || err == BookFusionSyncClient::NETWORK_ERROR) break;
    }
  }
  BookFusionSyncClient::endSession();

  store.dropSyncedTombstones();
  store.save();
  LOG_DBG("BFHL", "Highlights pushed: %d created, %d deleted, %d failed", result.created, result.deleted,
          result.failed);
  return result;
}

}  // namespace BookFusionHighlightSync
