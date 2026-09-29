#include "EpubPackager.h"

namespace EpubPackager {
namespace {

// DOS date for 1980-01-01 00:00; a fixed timestamp keeps output reproducible.
constexpr uint16_t DOS_TIME = 0;
constexpr uint16_t DOS_DATE = (0 << 9) | (1 << 5) | 1;
constexpr uint16_t ZIP_VERSION = 20;

void put16(uint8_t* p, const uint16_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
}

void put32(uint8_t* p, const uint32_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
  p[2] = static_cast<uint8_t>(v >> 16);
  p[3] = static_cast<uint8_t>(v >> 24);
}

constexpr char CONTAINER_XML[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">\n"
    "  <rootfiles>\n"
    "    <rootfile full-path=\"OEBPS/content.opf\" media-type=\"application/oebps-package+xml\"/>\n"
    "  </rootfiles>\n"
    "</container>\n";

std::string contentOpf(const ArticleMeta& meta) {
  std::string s =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"2.0\" unique-identifier=\"BookId\">\n"
      "  <metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\" xmlns:opf=\"http://www.idpf.org/2007/opf\">\n"
      "    <dc:title>";
  s += xmlEscape(meta.title);
  s += "</dc:title>\n";
  if (!meta.author.empty()) {
    s += "    <dc:creator opf:role=\"aut\">";
    s += xmlEscape(meta.author);
    s += "</dc:creator>\n";
  }
  s += "    <dc:language>";
  s += xmlEscape(meta.language);
  s += "</dc:language>\n    <dc:identifier id=\"BookId\">";
  s += xmlEscape(meta.identifier);
  s += "</dc:identifier>\n"
       "  </metadata>\n"
       "  <manifest>\n"
       "    <item id=\"ncx\" href=\"toc.ncx\" media-type=\"application/x-dtbncx+xml\"/>\n"
       "    <item id=\"article\" href=\"article.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
       "  </manifest>\n"
       "  <spine toc=\"ncx\">\n"
       "    <itemref idref=\"article\"/>\n"
       "  </spine>\n"
       "</package>\n";
  return s;
}

std::string tocNcx(const ArticleMeta& meta) {
  std::string s =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<ncx xmlns=\"http://www.daisy.org/z3986/2005/ncx/\" version=\"2005-1\">\n"
      "  <head>\n"
      "    <meta name=\"dtb:uid\" content=\"";
  s += xmlEscape(meta.identifier);
  s += "\"/>\n"
       "    <meta name=\"dtb:depth\" content=\"1\"/>\n"
       "  </head>\n"
       "  <docTitle><text>";
  s += xmlEscape(meta.title);
  s += "</text></docTitle>\n"
       "  <navMap>\n"
       "    <navPoint id=\"article\" playOrder=\"1\">\n"
       "      <navLabel><text>";
  s += xmlEscape(meta.title);
  s += "</text></navLabel>\n"
       "      <content src=\"article.xhtml\"/>\n"
       "    </navPoint>\n"
       "  </navMap>\n"
       "</ncx>\n";
  return s;
}

}  // namespace

uint32_t crc32Update(uint32_t crc, const uint8_t* data, size_t len) {
  // Nibble-table CRC-32: 64 bytes of table, ~2 lookups per byte.
  static constexpr uint32_t TABLE[16] = {0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4,
                                         0x4DB26158, 0x5005713C, 0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C,
                                         0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C};
  crc = ~crc;
  while (len--) {
    crc ^= *data++;
    crc = (crc >> 4) ^ TABLE[crc & 0x0F];
    crc = (crc >> 4) ^ TABLE[crc & 0x0F];
  }
  return ~crc;
}

std::string xmlEscape(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (const char c : s) {
    switch (c) {
      case '&':
        out += "&amp;";
        break;
      case '<':
        out += "&lt;";
        break;
      case '>':
        out += "&gt;";
        break;
      case '"':
        out += "&quot;";
        break;
      default:
        // Drop C0 controls, which are not legal in XML 1.0.
        if (static_cast<unsigned char>(c) >= 0x20 || c == '\t' || c == '\n') out.push_back(c);
        break;
    }
  }
  return out;
}

std::string chapterHeader(const ArticleMeta& meta) {
  std::string s =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<html xmlns=\"http://www.w3.org/1999/xhtml\" xml:lang=\"";
  s += xmlEscape(meta.language);
  s += "\">\n<head>\n<title>";
  s += xmlEscape(meta.title);
  s += "</title>\n</head>\n<body>\n";
  return s;
}

const char* chapterFooter() { return "\n</body>\n</html>\n"; }

bool ZipWriter::write(const uint8_t* data, const size_t len) {
  if (!ok) return false;
  ok = sink(data, len);
  if (ok) offset += static_cast<uint32_t>(len);
  return ok;
}

bool ZipWriter::writeLocalHeader(const Entry& e) {
  uint8_t h[30];
  put32(h, 0x04034b50);
  put16(h + 4, ZIP_VERSION);
  put16(h + 6, 0);  // flags
  put16(h + 8, 0);  // method: stored
  put16(h + 10, DOS_TIME);
  put16(h + 12, DOS_DATE);
  put32(h + 14, e.crc);
  put32(h + 18, e.size);  // compressed size
  put32(h + 22, e.size);  // uncompressed size
  put16(h + 26, static_cast<uint16_t>(e.name.size()));
  put16(h + 28, 0);  // extra length
  return write(h, sizeof(h)) && write(reinterpret_cast<const uint8_t*>(e.name.data()), e.name.size());
}

bool ZipWriter::addFile(const char* name, const uint8_t* data, const size_t len) {
  Entry e{name, crc32Update(0, data, len), static_cast<uint32_t>(len), offset};
  if (!writeLocalHeader(e) || !write(data, len)) return false;
  entries.push_back(std::move(e));
  return true;
}

bool ZipWriter::addFileFromSource(const char* name, const uint32_t crc, const uint32_t size, const Source& source) {
  Entry e{name, crc, size, offset};
  if (!writeLocalHeader(e)) return false;
  uint8_t buf[256];
  uint32_t remaining = size;
  while (remaining > 0) {
    const size_t want = remaining < sizeof(buf) ? remaining : sizeof(buf);
    const int got = source(buf, want);
    if (got <= 0) {
      ok = false;  // source ended early: the declared size would be a lie
      return false;
    }
    if (!write(buf, static_cast<size_t>(got))) return false;
    remaining -= static_cast<uint32_t>(got);
  }
  entries.push_back(std::move(e));
  return true;
}

bool ZipWriter::finish() {
  const uint32_t cdOffset = offset;
  for (const auto& e : entries) {
    uint8_t h[46];
    put32(h, 0x02014b50);
    put16(h + 4, ZIP_VERSION);  // version made by
    put16(h + 6, ZIP_VERSION);  // version needed
    put16(h + 8, 0);            // flags
    put16(h + 10, 0);           // method: stored
    put16(h + 12, DOS_TIME);
    put16(h + 14, DOS_DATE);
    put32(h + 16, e.crc);
    put32(h + 20, e.size);
    put32(h + 24, e.size);
    put16(h + 28, static_cast<uint16_t>(e.name.size()));
    put16(h + 30, 0);  // extra
    put16(h + 32, 0);  // comment
    put16(h + 34, 0);  // disk
    put16(h + 36, 0);  // internal attributes
    put32(h + 38, 0);  // external attributes
    put32(h + 42, e.offset);
    if (!write(h, sizeof(h)) || !write(reinterpret_cast<const uint8_t*>(e.name.data()), e.name.size())) return false;
  }
  const uint32_t cdSize = offset - cdOffset;
  uint8_t end[22];
  put32(end, 0x06054b50);
  put16(end + 4, 0);
  put16(end + 6, 0);
  put16(end + 8, static_cast<uint16_t>(entries.size()));
  put16(end + 10, static_cast<uint16_t>(entries.size()));
  put32(end + 12, cdSize);
  put32(end + 16, cdOffset);
  put16(end + 20, 0);
  return write(end, sizeof(end));
}

bool writeEpub(const Sink& sink, const ArticleMeta& meta, const uint32_t chapterCrc, const uint32_t chapterSize,
               const Source& chapterSource) {
  ZipWriter zip(sink);
  static constexpr char MIMETYPE[] = "application/epub+zip";
  return zip.addFile("mimetype", reinterpret_cast<const uint8_t*>(MIMETYPE), sizeof(MIMETYPE) - 1) &&
         zip.addFile("META-INF/container.xml", reinterpret_cast<const uint8_t*>(CONTAINER_XML),
                     sizeof(CONTAINER_XML) - 1) &&
         zip.addFile("OEBPS/content.opf", contentOpf(meta)) && zip.addFile("OEBPS/toc.ncx", tocNcx(meta)) &&
         zip.addFileFromSource("OEBPS/article.xhtml", chapterCrc, chapterSize, chapterSource) && zip.finish();
}

}  // namespace EpubPackager
