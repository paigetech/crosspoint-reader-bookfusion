#pragma once
#include <cmath>
#include <cstdint>

/**
 * Decisions behind BookFusion auto sync (push on sleep, check on wake), kept
 * free of Arduino types so host tests can cover them.
 *
 * All percentages are BookFusion's 0-100 scale.
 */
namespace BookFusionAutoSyncPolicy {

// Positions closer than this are the same position (float noise, re-layout).
constexpr float SAME_POSITION_PCT = 0.05f;
// How far another device must be ahead before it counts as "ahead". Smaller
// gaps are page-layout differences between devices, not real reading.
constexpr float AHEAD_MARGIN_PCT = 0.5f;
// A wake this soon after sleeping skips the check: nobody read elsewhere.
constexpr int64_t SHORT_NAP_SECONDS = 10 * 60;

inline bool samePosition(const float a, const float b) { return std::fabs(a - b) < SAME_POSITION_PCT; }

inline bool remoteAhead(const float remotePct, const float localPct) { return remotePct > localPct + AHEAD_MARGIN_PCT; }

// Whether the position needs pushing at all: false when this exact position
// was already pushed for this book.
inline bool needsPush(const bool pushedThisBook, const float pushedPct, const float localPct) {
  return !pushedThisBook || !samePosition(pushedPct, localPct);
}

// Whether to overwrite the server with the local position. Never overwrite a
// position another device moved ahead; the exception is the server still
// holding this device's own last push (the user paged back on purpose).
inline bool shouldPush(const bool remoteKnown, const float remotePct, const float localPct, const bool pushedThisBook,
                       const float pushedPct) {
  if (!remoteKnown) return true;
  if (!remoteAhead(remotePct, localPct)) return true;
  return pushedThisBook && samePosition(remotePct, pushedPct);
}

// Clock values are seconds; a reset clock reads earlier than sleptAt and is
// treated as a long sleep.
inline bool isShortNap(const int64_t sleptAt, const int64_t now) {
  return sleptAt > 0 && now >= sleptAt && now - sleptAt < SHORT_NAP_SECONDS;
}

}  // namespace BookFusionAutoSyncPolicy
