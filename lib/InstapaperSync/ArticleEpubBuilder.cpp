#include "ArticleEpubBuilder.h"

#include <HalStorage.h>
#include <Logging.h>

#include <cstring>
#include <memory>
#include <new>

#include "ArticleSanitizer.h"

namespace ArticleEpubBuilder {
namespace {

constexpr char CHAPTER_TMP_PATH[] = "/.crosspoint/ip_chapter.xhtml";
constexpr size_t CHUNK = 1024;

}  // namespace

Result build(const char* htmlPath, const char* epubPath, const EpubPackager::ArticleMeta& meta) {
  // Pass 1: HTML → XHTML chapter, tracking CRC-32 and size.
  uint32_t crc = 0;
  uint32_t size = 0;
  size_t textChars = 0;
  {
    HalFile in;
    if (!Storage.openFileForRead("IPB", htmlPath, in)) return Result::READ_ERROR;
    HalFile chapter;
    if (!Storage.openFileForWrite("IPB", CHAPTER_TMP_PATH, chapter)) return Result::WRITE_ERROR;

    bool writeOk = true;
    const auto emit = [&](const char* data, const size_t len) {
      if (!writeOk) return false;
      writeOk = chapter.write(reinterpret_cast<const uint8_t*>(data), len) == len;
      crc = EpubPackager::crc32Update(crc, reinterpret_cast<const uint8_t*>(data), len);
      size += static_cast<uint32_t>(len);
      return writeOk;
    };

    const std::string header = EpubPackager::chapterHeader(meta);
    emit(header.data(), header.size());

    // Both objects are heap-allocated: ~0.5 KB sanitizer + 1 KB read buffer
    // would crowd the loop task's stack.
    auto sanitizer = std::unique_ptr<ArticleSanitizer>(new (std::nothrow) ArticleSanitizer(emit));
    auto buf = std::unique_ptr<char[]>(new (std::nothrow) char[CHUNK]);
    if (!sanitizer || !buf) return Result::NO_MEMORY;

    while (writeOk) {
      const int n = in.read(buf.get(), CHUNK);
      if (n < 0) return Result::READ_ERROR;
      if (n == 0) break;
      sanitizer->feed(buf.get(), static_cast<size_t>(n));
    }
    sanitizer->finish();
    textChars = sanitizer->textLength();
    const char* footer = EpubPackager::chapterFooter();
    emit(footer, strlen(footer));
    if (!writeOk) {
      chapter.close();
      Storage.remove(CHAPTER_TMP_PATH);
      return Result::WRITE_ERROR;
    }
  }

  if (textChars == 0) {
    Storage.remove(CHAPTER_TMP_PATH);
    return Result::EMPTY_ARTICLE;
  }

  // Pass 2: package.
  bool ok;
  {
    HalFile chapter;
    if (!Storage.openFileForRead("IPB", CHAPTER_TMP_PATH, chapter)) return Result::READ_ERROR;
    HalFile epub;
    if (!Storage.openFileForWrite("IPB", epubPath, epub)) return Result::WRITE_ERROR;
    ok =
        EpubPackager::writeEpub([&epub](const uint8_t* data, size_t len) { return epub.write(data, len) == len; }, meta,
                                crc, size, [&chapter](uint8_t* buf, size_t len) { return chapter.read(buf, len); });
  }
  Storage.remove(CHAPTER_TMP_PATH);
  if (!ok) {
    Storage.remove(epubPath);
    return Result::WRITE_ERROR;
  }
  LOG_DBG("IPB", "Built %s (%lu byte chapter, %u chars)", epubPath, static_cast<unsigned long>(size),
          static_cast<unsigned>(textChars));
  return Result::OK;
}

const char* resultString(const Result result) {
  switch (result) {
    case Result::OK:
      return "OK";
    case Result::READ_ERROR:
      return "Could not read the article";
    case Result::WRITE_ERROR:
      return "SD card write failed";
    case Result::NO_MEMORY:
      return "Out of memory";
    case Result::EMPTY_ARTICLE:
      return "Article has no text";
  }
  return "Unknown error";
}

}  // namespace ArticleEpubBuilder
