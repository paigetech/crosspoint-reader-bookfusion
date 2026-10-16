#include "BookFusionHighlightResolver.h"

#include <Arduino.h>
#include <Logging.h>
#include <Print.h>

#include <algorithm>

namespace BookFusionHighlightResolver {
namespace {

// Room before the expected start (offset drift from text CrossPoint does not
// count), and after the selection for the quote suffix.
constexpr uint32_t WINDOW_BEFORE = 512;
constexpr uint32_t WINDOW_AFTER = 1024;

class CollectorSink final : public Print {
 public:
  explicit CollectorSink(BookFusionHighlightAnchor::BodyTextCollector& collector) : collector(collector) {}
  size_t write(uint8_t c) override { return write(&c, 1); }
  // A short write tells the zip reader to stop: the window is complete.
  size_t write(const uint8_t* buffer, size_t size) override {
    return collector.feed(reinterpret_cast<const char*>(buffer), size) ? size : 0;
  }

 private:
  BookFusionHighlightAnchor::BodyTextCollector& collector;
};

}  // namespace

bool resolve(const Epub& epub, const int spineIndex, const uint32_t approxStartCp,
             const std::vector<std::string>& words, BookFusionHighlightAnchor::Anchor& out,
             const BookFusionHighlightAnchor::PageContext* page) {
  if (spineIndex < 0 || spineIndex >= epub.getSpineItemsCount() || words.empty()) return false;
  const std::string href = epub.getSpineItem(spineIndex).href;
  if (href.empty()) return false;

  size_t selectionBytes = 0;
  for (const auto& w : words) selectionBytes += w.size() + 1;
  // The window starts at the page (repeated text is counted from there).
  uint32_t anchorCp = page ? std::min(page->pageStartCp, approxStartCp) : approxStartCp;
  const uint32_t from = anchorCp > WINDOW_BEFORE ? anchorCp - WINDOW_BEFORE : 0;
  // Source whitespace (indentation, line breaks) can double the selection's length.
  const uint32_t to = approxStartCp + static_cast<uint32_t>(selectionBytes) * 2 + WINDOW_AFTER;

  const auto dot = href.find_last_of('.');
  std::string ext = dot == std::string::npos ? "" : href.substr(dot + 1);
  for (auto& c : ext) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
  BookFusionHighlightAnchor::BodyTextCollector collector(from, to, ext == "html" || ext == "htm");
  CollectorSink sink(collector);
  const unsigned long started = millis();
  if (!epub.readItemContentsToStream(href, sink, 2048, true)) {
    LOG_ERR("BFHL", "Failed to read spine %d", spineIndex);
    return false;
  }
  collector.finish();
  if (!collector.ok()) {
    LOG_ERR("BFHL", "Chapter %d did not parse", spineIndex);
    return false;
  }
  const bool found = BookFusionHighlightAnchor::locate(collector.windowText(), collector.windowStartCp(),
                                                       collector.windowStartUtf16(), words, approxStartCp, out, page);
  LOG_DBG("BFHL", "Resolve spine %d near %lu: %s in %lums (%u bytes window)", spineIndex,
          static_cast<unsigned long>(approxStartCp), found ? "found" : "not found", millis() - started,
          static_cast<unsigned>(collector.windowText().size()));
  return found;
}

}  // namespace BookFusionHighlightResolver
