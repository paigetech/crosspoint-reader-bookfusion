#include "InstapaperSettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <WiFi.h>

#include <memory>

#include "InstapaperArticleListActivity.h"
#include "InstapaperSignInActivity.h"
#include "InstapaperStore.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

namespace {
constexpr int ACCOUNT_INDEX = 0;
constexpr int UNREAD_INDEX = 1;
constexpr int STARRED_INDEX = 2;
constexpr int ARCHIVE_INDEX = 3;
constexpr int SIGN_OUT_INDEX = 4;

const StrId menuNames[InstapaperSettingsActivity::MENU_ITEMS] = {
    StrId::STR_IP_ACCOUNT, StrId::STR_IP_UNREAD, StrId::STR_IP_STARRED, StrId::STR_IP_ARCHIVE, StrId::STR_IP_SIGN_OUT};

const char* folderFor(const int index) {
  switch (index) {
    case STARRED_INDEX:
      return "starred";
    case ARCHIVE_INDEX:
      return "archive";
    default:
      return "unread";
  }
}
}  // namespace

InstapaperSettingsActivity::InstapaperSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("InstapaperSettings", renderer, mappedInput) {
  for (int i = 0; i < MENU_ITEMS; i++) {
    rowItems_[i].label = I18N.get(menuNames[i]);
    rowItems_[i].actionValue = static_cast<int16_t>(i);
  }
}

void InstapaperSettingsActivity::onEnter() {
  // Pick up an instapaper.json the user just copied to the SD card.
  INSTAPAPER_STORE.importKeyFile();
  UiListActivity::onEnter();
}

void InstapaperSettingsActivity::onExit() {
  UiListActivity::onExit();
  // Same as the other network activities: reboot back to Settings once WiFi
  // has been used, to clear the heap fragmentation TLS leaves behind.
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestartToSettings();
  }
}

int InstapaperSettingsActivity::listCount() const { return MENU_ITEMS; }

const char* InstapaperSettingsActivity::headerTitle() const { return tr(STR_IP_INSTAPAPER); }

void InstapaperSettingsActivity::activateIndex(const int index) {
  app.clearTapFlash();
  if (!INSTAPAPER_STORE.hasConsumerKey()) {
    INSTAPAPER_STORE.importKeyFile();
    requestUpdate();
    return;
  }
  if (index == ACCOUNT_INDEX) {
    startActivityForResult(std::make_unique<InstapaperSignInActivity>(renderer, mappedInput),
                           [this](const ActivityResult&) { requestUpdate(); });
  } else if (index == SIGN_OUT_INDEX) {
    INSTAPAPER_STORE.signOut();
    requestUpdate();
  } else if (INSTAPAPER_STORE.isSignedIn()) {
    startActivityForResult(
        std::make_unique<InstapaperArticleListActivity>(renderer, mappedInput, folderFor(index), menuNames[index]),
        [this](const ActivityResult&) { requestUpdate(); });
  }
}

void InstapaperSettingsActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  const bool hasKey = INSTAPAPER_STORE.hasConsumerKey();
  const bool signedIn = INSTAPAPER_STORE.isSignedIn();
  if (!hasKey) {
    rowValues_[ACCOUNT_INDEX] = tr(STR_IP_NO_KEY);
  } else if (signedIn) {
    rowValues_[ACCOUNT_INDEX] = INSTAPAPER_STORE.getUsername();
  } else {
    rowValues_[ACCOUNT_INDEX] = tr(STR_IP_NOT_SIGNED_IN);
  }
  for (int i = UNREAD_INDEX; i <= ARCHIVE_INDEX; i++) rowValues_[i] = signedIn ? "" : tr(STR_IP_NOT_SIGNED_IN);
  rowValues_[SIGN_OUT_INDEX] = "";

  for (int i = 0; i < MENU_ITEMS; i++) {
    rowItems_[i].value = rowValues_[i].empty() ? nullptr : rowValues_[i].c_str();
  }
  // Without a key nothing works: say where it comes from.
  rowItems_[ACCOUNT_INDEX].subtitle = hasKey ? nullptr : tr(STR_IP_NO_KEY_HINT);

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
