#pragma once
#include <cstdint>
#include <string>

#include "BookFusionSyncClient.h"

/**
 * Sends a book's unsynced highlight changes to BookFusion: new highlights are
 * created, deleted ones removed. Needs WiFi up; reuses one TLS session.
 */
namespace BookFusionHighlightSync {

struct Result {
  BookFusionSyncClient::Error error = BookFusionSyncClient::OK;  // first failure
  int created = 0;
  int deleted = 0;
  int failed = 0;
};

Result pushPending(const std::string& epubPath, uint32_t bookId);

}  // namespace BookFusionHighlightSync
