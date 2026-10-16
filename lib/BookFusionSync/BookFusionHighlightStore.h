#pragma once
#include <cstdint>
#include <string>
#include <vector>

/**
 * One highlight in a BookFusion book. Offsets follow BookFusionHighlightAnchor:
 * cp* are codepoints (CrossPoint's page offsets, used to draw and jump), u16*
 * are BookFusion's character offsets (sent to the API).
 */
struct BookFusionHighlight {
  uint32_t localId = 0;
  std::string remoteId;  // BookFusion id once pushed; empty before
  bool deleted = false;  // deleted here, remote delete still to send
  uint16_t chapterIndex = 0;
  uint32_t cpStart = 0;
  uint32_t cpEnd = 0;
  uint32_t u16Start = 0;
  uint32_t u16End = 0;
  std::string quote;
  std::string prefix;
  std::string suffix;
  std::string chapterTitle;
  std::string addedAt;  // ISO 8601 UTC, empty when the clock was not set

  bool needsPush() const { return deleted ? !remoteId.empty() : remoteId.empty(); }
};

/**
 * Highlights of one book, kept in /.crosspoint/bookfusion_hl_<md5>.json next
 * to the book-id sidecar (not in the book cache, which "Delete cache" clears).
 */
class BookFusionHighlightStore {
 public:
  bool load(const std::string& epubPath);  // false only on a corrupt file
  bool save() const;

  const std::string& path() const { return epubPath; }
  // Visible highlights (not deleted), in reading order.
  std::vector<const BookFusionHighlight*> visible() const;
  std::vector<const BookFusionHighlight*> visibleInChapter(uint16_t chapterIndex) const;
  bool hasPending() const;

  BookFusionHighlight& add(BookFusionHighlight highlight);  // assigns localId
  // Removes it, or keeps a tombstone until the remote delete is sent.
  void remove(uint32_t localId);

  // Sync bookkeeping.
  std::vector<BookFusionHighlight>& all() { return highlights; }
  void dropSyncedTombstones();

 private:
  std::string epubPath;
  std::string filePath;
  uint32_t nextId = 1;
  std::vector<BookFusionHighlight> highlights;
};
