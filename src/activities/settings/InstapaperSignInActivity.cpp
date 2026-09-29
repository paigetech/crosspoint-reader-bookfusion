#include "InstapaperSignInActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <WiFi.h>

#include <cstdio>

#include "InstapaperClient.h"
#include "InstapaperStore.h"
#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

void InstapaperSignInActivity::onEnter() {
  Activity::onEnter();
  startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_IP_EMAIL),
                                                                 INSTAPAPER_STORE.getUsername(), 128, InputType::Text),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled) {
                             finish();
                             return;
                           }
                           email = std::get<KeyboardResult>(result.data).text;
                           askPassword();
                         });
}

void InstapaperSignInActivity::askPassword() {
  // Instapaper accounts may have no password; an empty entry is allowed.
  startActivityForResult(
      std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_IP_PASSWORD), "", 128, InputType::Password),
      [this](const ActivityResult& result) {
        if (result.isCancelled) {
          finish();
          return;
        }
        password = std::get<KeyboardResult>(result.data).text;
        connectAndSignIn();
      });
}

void InstapaperSignInActivity::connectAndSignIn() {
  if (WiFi.status() == WL_CONNECTED) {
    signIn();
    return;
  }
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled) {
                             finish();
                             return;
                           }
                           signIn();
                         });
}

void InstapaperSignInActivity::signIn() {
  {
    RenderLock lock(*this);
    state = SIGNING_IN;
  }
  requestUpdateAndWait();

  WiFi.setSleep(false);
  const auto err = InstapaperClient::signIn(email, password);
  password.assign(password.size(), '\0');  // don't keep the password around
  password.clear();

  RenderLock lock(*this);
  if (err == InstapaperClient::OK) {
    state = SUCCESS;
  } else {
    state = FAILED;
    const char* detail = InstapaperClient::lastErrorMessage();
    snprintf(errorMsg, sizeof(errorMsg), "%s%s%s", InstapaperClient::errorString(err), detail[0] ? " - " : "", detail);
  }
  requestUpdate(true);
}

void InstapaperSignInActivity::loop() {
  if (state != SUCCESS && state != FAILED) return;
  int x = 0;
  int y = 0;
  if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
      mappedInput.wasPressed(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(x, y)) {
    finish();
  }
}

void InstapaperSignInActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_IP_INSTAPAPER));

  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
  const int top = (pageHeight - lineH) / 2;
  if (state == SIGNING_IN || state == ENTERING) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_IP_SIGNING_IN), true, EpdFontFamily::BOLD);
  } else if (state == SUCCESS) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_IP_SIGNED_IN), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, top + lineH + 10, INSTAPAPER_STORE.getUsername().c_str());
  } else {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_IP_SIGN_IN_FAILED), true, EpdFontFamily::BOLD);
    const auto detail = renderer.truncatedText(UI_10_FONT_ID, errorMsg, pageWidth - 40);
    renderer.drawCenteredText(UI_10_FONT_ID, top + lineH + 10, detail.c_str());
  }

  const auto labels = mappedInput.mapLabels(state == SIGNING_IN ? "" : tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
