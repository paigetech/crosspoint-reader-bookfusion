#pragma once

#include <string>

#include "activities/UiListActivity.h"

/**
 * Settings → Instapaper: account, the Unread/Starred/Archive lists, sign out.
 */
class InstapaperSettingsActivity final : public UiListActivity {
 public:
  explicit InstapaperSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;

  static constexpr int MENU_ITEMS = 5;

 private:
  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override;

  std::string rowValues_[MENU_ITEMS];
  freeink::ui::ListItem rowItems_[MENU_ITEMS]{};
};
