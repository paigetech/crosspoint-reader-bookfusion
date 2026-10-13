#include "BookFusionJsonIO.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <ObfuscationUtils.h>

#include "BookFusionTokenStore.h"

namespace BookFusionJsonIO {

bool save(const BookFusionTokenStore& store, const char* path) {
  JsonDocument doc;
  doc["token_obf"] = obfuscation::obfuscateToBase64(store.accessToken);
  doc["auto_sync"] = store.autoSync;

  String json;
  serializeJson(doc, json);
  return Storage.writeFile(path, json);
}

bool load(BookFusionTokenStore& store, const char* json) {
  JsonDocument doc;
  auto error = deserializeJson(doc, json);
  if (error) {
    LOG_ERR("BFS", "JSON parse error loading BookFusion token: %s", error.c_str());
    return false;
  }

  store.autoSync = doc["auto_sync"] | true;

  bool ok = false;
  store.accessToken = obfuscation::deobfuscateFromBase64(doc["token_obf"] | "", &ok);
  if (!ok) {
    store.accessToken.clear();
    return false;
  }

  LOG_DBG("BFS", "Loaded BookFusion token");
  return true;
}

}  // namespace BookFusionJsonIO
