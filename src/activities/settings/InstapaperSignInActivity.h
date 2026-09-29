#pragma once

#include <string>

#include "activities/Activity.h"

/**
 * Instapaper sign-in: email and password on the on-screen keyboard, then an
 * xAuth token exchange. Only the resulting token is stored.
 */
class InstapaperSignInActivity final : public Activity {
 public:
  explicit InstapaperSignInActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("InstapaperSignIn", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == SIGNING_IN; }

 private:
  enum State { ENTERING, SIGNING_IN, SUCCESS, FAILED };

  State state = ENTERING;
  std::string email;
  std::string password;
  char errorMsg[160] = {};

  void askPassword();
  void connectAndSignIn();
  void signIn();
};
