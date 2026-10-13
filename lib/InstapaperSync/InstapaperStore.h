#pragma once
#include <string>

/**
 * Instapaper credentials on the SD card.
 *
 * The Full API needs an OAuth consumer key/secret issued to the user by
 * Instapaper (instapaper.com/developers). They are not in the firmware: the
 * user copies an `instapaper.json` with
 *   {"consumer_key": "...", "consumer_secret": "..."}
 * to the SD root; importKeyFile() moves it into /.crosspoint/instapaper.json
 * (secrets obfuscated, like the other credential stores) and deletes the
 * plain copy. Signing in stores the xAuth access token and secret; the
 * account password is never stored.
 */
class InstapaperStore {
 public:
  static InstapaperStore& getInstance() { return instance; }

  InstapaperStore(const InstapaperStore&) = delete;
  InstapaperStore& operator=(const InstapaperStore&) = delete;

  bool loadFromFile();
  bool saveToFile() const;
  // Imports /instapaper.json from the SD root if present. Returns true if a key was imported.
  bool importKeyFile();

  bool hasConsumerKey() const { return !consumerKey.empty() && !consumerSecret.empty(); }
  bool isSignedIn() const { return !token.empty() && !tokenSecret.empty(); }

  const std::string& getConsumerKey() const { return consumerKey; }
  const std::string& getConsumerSecret() const { return consumerSecret; }
  const std::string& getToken() const { return token; }
  const std::string& getTokenSecret() const { return tokenSecret; }
  const std::string& getUsername() const { return username; }

  void setSession(const std::string& newToken, const std::string& newTokenSecret, const std::string& newUsername);
  void signOut();

  static constexpr char KEY_IMPORT_PATH[] = "/instapaper.json";

 private:
  static InstapaperStore instance;
  InstapaperStore() = default;

  std::string consumerKey;
  std::string consumerSecret;
  std::string token;
  std::string tokenSecret;
  std::string username;
};

#define INSTAPAPER_STORE InstapaperStore::getInstance()
