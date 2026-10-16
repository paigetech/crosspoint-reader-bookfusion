#include "BookFusionHighlightStore.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>

#include "BookFusionBookIdStore.h"

namespace {
bool readingOrder(const BookFusionHighlight* a, const BookFusionHighlight* b) {
  return a->chapterIndex != b->chapterIndex ? a->chapterIndex < b->chapterIndex : a->cpStart < b->cpStart;
}
}  // namespace

bool BookFusionHighlightStore::load(const std::string& path) {
  epubPath = path;
  highlights.clear();
  nextId = 1;
  char file[72];
  BookFusionBookIdStore::buildHighlightsPath(path.c_str(), file, sizeof(file));
  filePath = file;
  if (!Storage.exists(file)) return true;

  const String json = Storage.readFile(file);
  JsonDocument doc;
  if (json.isEmpty() || deserializeJson(doc, json) != DeserializationError::Ok) {
    LOG_ERR("BFHL", "Unreadable highlights file %s", file);
    return false;
  }
  nextId = doc["next_id"] | static_cast<uint32_t>(1);
  for (JsonObjectConst h : doc["highlights"].as<JsonArrayConst>()) {
    BookFusionHighlight hl;
    hl.localId = h["id"] | static_cast<uint32_t>(0);
    hl.remoteId = h["remote_id"] | "";
    hl.deleted = h["deleted"] | false;
    hl.chapterIndex = h["chapter"] | static_cast<uint16_t>(0);
    hl.cpStart = h["cp_start"] | static_cast<uint32_t>(0);
    hl.cpEnd = h["cp_end"] | static_cast<uint32_t>(0);
    hl.u16Start = h["start"] | static_cast<uint32_t>(0);
    hl.u16End = h["end"] | static_cast<uint32_t>(0);
    hl.quote = h["quote"] | "";
    hl.prefix = h["prefix"] | "";
    hl.suffix = h["suffix"] | "";
    hl.chapterTitle = h["chapter_title"] | "";
    hl.addedAt = h["added_at"] | "";
    nextId = std::max(nextId, hl.localId + 1);
    highlights.push_back(std::move(hl));
  }
  return true;
}

bool BookFusionHighlightStore::save() const {
  if (filePath.empty()) return false;
  if (highlights.empty()) {
    if (Storage.exists(filePath.c_str())) Storage.remove(filePath.c_str());
    return true;
  }
  JsonDocument doc;
  doc["next_id"] = nextId;
  JsonArray arr = doc["highlights"].to<JsonArray>();
  for (const auto& hl : highlights) {
    JsonObject h = arr.add<JsonObject>();
    h["id"] = hl.localId;
    if (!hl.remoteId.empty()) h["remote_id"] = hl.remoteId;
    if (hl.deleted) h["deleted"] = true;
    h["chapter"] = hl.chapterIndex;
    h["cp_start"] = hl.cpStart;
    h["cp_end"] = hl.cpEnd;
    h["start"] = hl.u16Start;
    h["end"] = hl.u16End;
    h["quote"] = hl.quote;
    h["prefix"] = hl.prefix;
    h["suffix"] = hl.suffix;
    h["chapter_title"] = hl.chapterTitle;
    if (!hl.addedAt.empty()) h["added_at"] = hl.addedAt;
  }
  String json;
  serializeJson(doc, json);
  Storage.mkdir("/.crosspoint");
  if (!Storage.writeFile(filePath.c_str(), json)) {
    LOG_ERR("BFHL", "Failed to write %s", filePath.c_str());
    return false;
  }
  return true;
}

std::vector<const BookFusionHighlight*> BookFusionHighlightStore::visible() const {
  std::vector<const BookFusionHighlight*> out;
  for (const auto& hl : highlights) {
    if (!hl.deleted) out.push_back(&hl);
  }
  std::sort(out.begin(), out.end(), readingOrder);
  return out;
}

std::vector<const BookFusionHighlight*> BookFusionHighlightStore::visibleInChapter(const uint16_t chapterIndex) const {
  std::vector<const BookFusionHighlight*> out;
  for (const auto& hl : highlights) {
    if (!hl.deleted && hl.chapterIndex == chapterIndex) out.push_back(&hl);
  }
  return out;
}

bool BookFusionHighlightStore::hasPending() const {
  return std::any_of(highlights.begin(), highlights.end(),
                     [](const BookFusionHighlight& hl) { return hl.needsPush(); });
}

BookFusionHighlight& BookFusionHighlightStore::add(BookFusionHighlight highlight) {
  highlight.localId = nextId++;
  highlights.push_back(std::move(highlight));
  return highlights.back();
}

void BookFusionHighlightStore::remove(const uint32_t localId) {
  for (auto it = highlights.begin(); it != highlights.end(); ++it) {
    if (it->localId != localId) continue;
    if (it->remoteId.empty()) {
      highlights.erase(it);  // never reached BookFusion
    } else {
      it->deleted = true;
    }
    return;
  }
}

void BookFusionHighlightStore::dropSyncedTombstones() {
  highlights.erase(std::remove_if(highlights.begin(), highlights.end(),
                                  [](const BookFusionHighlight& hl) { return hl.deleted && hl.remoteId.empty(); }),
                   highlights.end());
}
