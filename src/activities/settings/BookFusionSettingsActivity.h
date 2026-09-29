#pragma once

#include <string>

#include "activities/UiListActivity.h"

/**
 * Settings submenu for BookFusion Sync.
 * Link / unlink the BookFusion account (OAuth device-code flow).
 */
class BookFusionSettingsActivity final : public UiListActivity {
 public:
  explicit BookFusionSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onExit() override;

  static constexpr int MENU_ITEMS = 2;

 private:
  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override;

  std::string rowValues_[MENU_ITEMS];
  freeink::ui::ListItem rowItems_[MENU_ITEMS]{};
};
