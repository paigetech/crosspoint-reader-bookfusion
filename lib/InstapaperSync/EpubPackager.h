#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

/**
 * Minimal EPUB 2 writer for single-article books.
 *
 * The firmware's miniz is built without archive-writing support, so this
 * writes the ZIP container itself. Every entry is STORED (no compression),
 * which CrossPoint's ZipFile reads directly, and `mimetype` is the first,
 * uncompressed entry as the OCF spec requires.
 *
 * The article body is produced separately (ArticleSanitizer → SD file) with
 * its CRC-32 and size computed while it is written, so the packager can stream
 * it into the archive without holding it in RAM.
 */
namespace EpubPackager {

using Sink = std::function<bool(const uint8_t* data, size_t len)>;
// Reads the next bytes into buf. Returns the count, 0 at end, or <0 on error.
using Source = std::function<int(uint8_t* buf, size_t len)>;

// Incremental CRC-32 (IEEE, as used by ZIP). Start from 0.
uint32_t crc32Update(uint32_t crc, const uint8_t* data, size_t len);

std::string xmlEscape(const std::string& s);

struct ArticleMeta {
  std::string title;
  std::string author;      // shown as dc:creator (e.g. the source site)
  std::string identifier;  // stable unique id, e.g. "urn:instapaper:bookmark:123"
  std::string language = "en";
};

// The chapter document is chapterHeader(meta) + sanitized body + chapterFooter().
std::string chapterHeader(const ArticleMeta& meta);
const char* chapterFooter();

class ZipWriter {
 public:
  explicit ZipWriter(Sink sink) : sink(std::move(sink)) {}

  bool addFile(const char* name, const uint8_t* data, size_t len);
  bool addFile(const char* name, const std::string& data) {
    return addFile(name, reinterpret_cast<const uint8_t*>(data.data()), data.size());
  }
  // Streams size bytes from source; crc must be the CRC-32 of exactly those bytes.
  bool addFileFromSource(const char* name, uint32_t crc, uint32_t size, const Source& source);
  bool finish();

 private:
  struct Entry {
    std::string name;
    uint32_t crc;
    uint32_t size;
    uint32_t offset;
  };

  Sink sink;
  std::vector<Entry> entries;
  uint32_t offset = 0;
  bool ok = true;

  bool write(const uint8_t* data, size_t len);
  bool writeLocalHeader(const Entry& e);
};

// Writes a complete EPUB whose single chapter is read from chapterSource
// (chapterSize bytes with CRC-32 chapterCrc, including header and footer).
bool writeEpub(const Sink& sink, const ArticleMeta& meta, uint32_t chapterCrc, uint32_t chapterSize,
               const Source& chapterSource);

}  // namespace EpubPackager
