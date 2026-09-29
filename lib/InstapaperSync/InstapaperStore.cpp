#include "InstapaperStore.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <ObfuscationUtils.h>
#include <PersistableStore.h>

InstapaperStore InstapaperStore::instance;

namespace {
constexpr char STORE_PATH[] = "/.crosspoint/instapaper.json";

std::string readObfuscated(const JsonDocument& doc, const char* key) {
  bool ok = false;
  std::string value = obfuscation::deobfuscateFromBase64(doc[key] | "", &ok);
  return ok ? value : std::string();
}
}  // namespace

bool InstapaperStore::saveToFile() const {
  JsonDocument doc;
  doc["consumer_key"] = consumerKey;
  doc["consumer_secret_obf"] = obfuscation::obfuscateToBase64(consumerSecret);
  if (!token.empty()) {
    doc["token_obf"] = obfuscation::obfuscateToBase64(token);
    doc["token_secret_obf"] = obfuscation::obfuscateToBase64(tokenSecret);
    doc["username"] = username;
  }
  return PersistableStoreBase::writeDocToFile(STORE_PATH, doc);
}

bool InstapaperStore::loadFromFile() {
  JsonDocument doc;
  if (!PersistableStoreBase::readDocFromFile(STORE_PATH, doc)) return false;
  consumerKey = doc["consumer_key"] | "";
  consumerSecret = readObfuscated(doc, "consumer_secret_obf");
  token = readObfuscated(doc, "token_obf");
  tokenSecret = readObfuscated(doc, "token_secret_obf");
  username = doc["username"] | "";
  return true;
}

bool InstapaperStore::importKeyFile() {
  if (!Storage.exists(KEY_IMPORT_PATH)) return false;
  JsonDocument doc;
  if (!PersistableStoreBase::readDocFromFile(KEY_IMPORT_PATH, doc)) {
    LOG_ERR("IPS", "Could not parse %s", KEY_IMPORT_PATH);
    return false;
  }
  const char* key = doc["consumer_key"] | "";
  const char* secret = doc["consumer_secret"] | "";
  if (key[0] == '\0' || secret[0] == '\0') {
    LOG_ERR("IPS", "%s needs consumer_key and consumer_secret", KEY_IMPORT_PATH);
    return false;
  }
  const bool keyChanged = consumerKey != key;
  consumerKey = key;
  consumerSecret = secret;
  if (keyChanged) {
    // A token is bound to the consumer key that issued it.
    token.clear();
    tokenSecret.clear();
    username.clear();
  }
  if (!saveToFile()) return false;
  Storage.remove(KEY_IMPORT_PATH);
  LOG_INF("IPS", "Imported Instapaper API key");
  return true;
}

void InstapaperStore::setSession(const std::string& newToken, const std::string& newTokenSecret,
                                 const std::string& newUsername) {
  token = newToken;
  tokenSecret = newTokenSecret;
  username = newUsername;
  saveToFile();
}

void InstapaperStore::signOut() {
  token.clear();
  tokenSecret.clear();
  username.clear();
  saveToFile();
}
