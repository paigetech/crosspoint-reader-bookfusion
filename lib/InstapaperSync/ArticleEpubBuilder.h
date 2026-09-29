#pragma once
#include <cstddef>
#include <string>

#include "EpubPackager.h"

/**
 * Turns a downloaded article (raw HTML on the SD card) into an EPUB file.
 *
 * Two streaming passes, each with O(1) RAM:
 *   1. HTML → sanitized XHTML chapter in a temp file, computing its CRC-32 and
 *      size on the way out;
 *   2. ZIP the chapter plus the generated OPF/NCX into epubPath.
 */
namespace ArticleEpubBuilder {

enum class Result { OK, READ_ERROR, WRITE_ERROR, NO_MEMORY, EMPTY_ARTICLE };

Result build(const char* htmlPath, const char* epubPath, const EpubPackager::ArticleMeta& meta);

const char* resultString(Result result);

}  // namespace ArticleEpubBuilder
