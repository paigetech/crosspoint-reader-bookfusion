#include "BookFusionSettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <WiFi.h>

#include <memory>

#include "BookFusionAuthActivity.h"
#include "BookFusionLibraryActivity.h"
#include "BookFusionSyncClient.h"
#include "BookFusionTokenStore.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

namespace {
constexpr int LINK_INDEX = 0;
constexpr int UNLINK_INDEX = 1;
constexpr int BROWSE_INDEX = 2;

const StrId menuNames[BookFusionSettingsActivity::MENU_ITEMS] = {StrId::STR_BF_LINK_ACCOUNT, StrId::STR_BF_UNLINK,
                                                                 StrId::STR_BF_BROWSE_LIBRARY};
}  // namespace

BookFusionSettingsActivity::BookFusionSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("BookFusionSettings", renderer, mappedInput) {
  for (int i = 0; i < MENU_ITEMS; i++) {
    rowItems_[i].label = I18N.get(menuNames[i]);
    rowItems_[i].actionValue = static_cast<int16_t>(i);
  }
}

void BookFusionSettingsActivity::onExit() {
  UiListActivity::onExit();
  BookFusionSyncClient::endSession();

  // Same as the other network activities: a TLS session leaves the heap
  // fragmented, so reboot back to Settings once WiFi has been used.
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestartToSettings();
  }
}

int BookFusionSettingsActivity::listCount() const { return MENU_ITEMS; }

const char* BookFusionSettingsActivity::headerTitle() const { return tr(STR_BF_SYNC); }

void BookFusionSettingsActivity::activateIndex(const int index) {
  app.clearTapFlash();
  if (index == LINK_INDEX) {
    startActivityForResult(std::make_unique<BookFusionAuthActivity>(renderer, mappedInput),
                           [this](const ActivityResult&) { requestUpdate(); });
  } else if (index == UNLINK_INDEX) {
    if (BF_TOKEN_STORE.hasToken()) {
      BF_TOKEN_STORE.clearToken();
    }
    requestUpdate();
  } else if (index == BROWSE_INDEX) {
    if (BF_TOKEN_STORE.hasToken()) {
      startActivityForResult(std::make_unique<BookFusionLibraryActivity>(renderer, mappedInput),
                             [this](const ActivityResult&) { requestUpdate(); });
    }
  }
}

void BookFusionSettingsActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  const bool linked = BF_TOKEN_STORE.hasToken();
  rowValues_[LINK_INDEX] = linked ? tr(STR_BF_LINKED) : tr(STR_BF_NOT_LINKED);
  rowValues_[UNLINK_INDEX] = "";
  rowValues_[BROWSE_INDEX] = linked ? "" : tr(STR_BF_NOT_LINKED);
  for (int i = 0; i < MENU_ITEMS; i++) {
    rowItems_[i].value = rowValues_[i].empty() ? nullptr : rowValues_[i].c_str();
  }

  fui::ListProps props;
  props.items = rowItems_;
  props.count = static_cast<uint16_t>(MENU_ITEMS);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.valueInset = 8;
  props.labelText = screen.theme().smallText;
  props.labelText.maxLines = 2;
  syncListViewport(screen, props);
  screen.list(props);
}
