#pragma once
#include <Epub.h>

#include <memory>
#include <string>
#include <vector>

#include "BookFusionHighlightStore.h"
#include "activities/UiListActivity.h"
#include "components/OptionPopup.h"

/**
 * The open book's highlights (fork feature, BookFusion books). Selecting one
 * jumps there; hold (or long-press) for Open / Delete. Deletions reach
 * BookFusion with the next sync. Modeled on EpubReaderBookmarksActivity.
 */
class BookFusionHighlightsActivity final : public UiListActivity {
 public:
  BookFusionHighlightsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& epubPath,
                               uint32_t bookId);
  void onEnter() override;
  void render(RenderLock&&) override;

 private:
  int listCount() const override { return static_cast<int>(rows.size()); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onRowLongPress(int index) override;
  bool handleCustomInput() override;
  bool handleButtons() override;

  void rebuildRows();
  void openSelected();
  void showActions();
  void showDeleteConfirmation();
  void deleteSelected();
  void cancel();

  std::string epubPath;
  uint32_t bookId;
  BookFusionHighlightStore store;
  std::vector<const BookFusionHighlight*> rows;
  std::vector<std::string> labels;
  std::vector<freeink::ui::ListItem> rowItems;
  bool confirmingDelete = false;
  OptionPopup popup;
};
