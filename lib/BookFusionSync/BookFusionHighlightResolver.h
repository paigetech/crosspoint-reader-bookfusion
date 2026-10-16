#pragma once
#include <Epub.h>

#include <cstdint>
#include <string>
#include <vector>

#include "BookFusionHighlightAnchor.h"

namespace BookFusionHighlightResolver {

// Locates selected words in the chapter's source text and returns BookFusion
// offsets for them. Streams the chapter from the EPUB, keeping only a window
// around approxStartCp in memory.
bool resolve(const Epub& epub, int spineIndex, uint32_t approxStartCp, const std::vector<std::string>& words,
             BookFusionHighlightAnchor::Anchor& out, const BookFusionHighlightAnchor::PageContext* page = nullptr);

}  // namespace BookFusionHighlightResolver
