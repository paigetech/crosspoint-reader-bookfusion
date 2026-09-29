#pragma once

#include <I18n.h>

#include "activities/UiListActivity.h"

/**
 * BookFusion library: category menu (Currently Reading, Favorites, Plan to
 * Read, Completed, All Books). Choosing one opens BookFusionBookListActivity.
 */
class BookFusionLibraryActivity final : public UiListActivity {
 public:
  explicit BookFusionLibraryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  static constexpr int CATEGORY_COUNT = 5;

  struct Category {
    StrId nameId;
    const char* list;  // BookFusion "list" filter, nullptr = all books
    const char* sort;  // BookFusion sort key, nullptr = default
  };
  static const Category& category(int index);

 private:
  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override;

  freeink::ui::ListItem rowItems_[CATEGORY_COUNT]{};
};
