#pragma once
#include <cstdint>
#include <string>

#include "BookFusionSyncClient.h"

class GfxRenderer;

/**
 * Automatic BookFusion progress sync (fork feature, see BOOKFUSION.md).
 *
 *  - The reader reports its position after every saved page (noteReading).
 *  - Going to sleep pushes that position if it changed since the last push,
 *    unless another device is further ahead (syncBeforeSleep).
 *  - Waking into a BookFusion book first checks whether another device is
 *    further ahead (checkOnWake); if so the reader offers to jump there
 *    (takePendingRemote).
 *
 * Network work runs without UI on a saved WiFi network and gives up quietly
 * when none is in range. State lives in /.crosspoint/bf_autosync.json.
 */
namespace BookFusionAutoSync {

// Cheap: keeps the latest position in RAM only.
void noteReading(const std::string& epubPath, uint32_t bookId, const BookFusionPosition& position);

// Records a push made elsewhere (manual sync) so sleep does not repeat it.
void recordPushed(uint32_t bookId, float percentage);

// Called by enterDeepSleep() after the sleep screen is drawn.
void syncBeforeSleep();

// Called by setup() before resuming epubPath after a wake. Returns true when it
// used TLS; the caller then silent-restarts into the reader to defragment.
bool checkOnWake(const std::string& epubPath, GfxRenderer& renderer);

// Reader: the newer position found on wake for this book, consumed once.
bool takePendingRemote(const std::string& epubPath, BookFusionPosition& out);

}  // namespace BookFusionAutoSync
