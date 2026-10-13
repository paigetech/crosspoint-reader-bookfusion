#include "BookFusionAutoSync.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_mac.h>

#include <algorithm>
#include <cstdio>
#include <ctime>

#include "BookFusionAutoSyncPolicy.h"
#include "BookFusionTokenStore.h"
#include "WifiCredentialStore.h"
#include "activities/RenderLock.h"
#include "components/UITheme.h"

namespace BookFusionAutoSync {
namespace {

constexpr char STATE_FILE[] = "/.crosspoint/bf_autosync.json";
// Whole budget for joining a saved network; past it sync is skipped.
constexpr unsigned long WIFI_BUDGET_MS = 12000;
constexpr unsigned long WIFI_TRY_MS = 7000;

// Position reported by the reader this boot. Not persisted: a reboot always
// goes back through the reader, which reports again on its first page.
struct Noted {
  bool valid = false;
  std::string path;
  uint32_t bookId = 0;
  BookFusionPosition position;
};
Noted noted;

struct State {
  std::string path;           // book that was open at the last sleep
  uint32_t bookId = 0;        // its BookFusion id
  float localPct = 0.0f;      // its position at the last sleep
  int64_t sleptAt = 0;        // time(nullptr) at the last sleep
  uint32_t pushedBookId = 0;  // book of the last successful push
  float pushedPct = 0.0f;     // position of the last successful push
  bool hasPending = false;    // newer position found on wake, not yet offered
  BookFusionPosition pending;
};

State loadState() {
  State st;
  if (!Storage.exists(STATE_FILE)) return st;
  const String json = Storage.readFile(STATE_FILE);
  JsonDocument doc;
  if (json.isEmpty() || deserializeJson(doc, json) != DeserializationError::Ok) {
    LOG_ERR("BFAuto", "Unreadable state file, starting fresh");
    return st;
  }
  st.path = doc["path"] | "";
  st.bookId = doc["book_id"] | static_cast<uint32_t>(0);
  st.localPct = doc["local_pct"] | 0.0f;
  st.sleptAt = doc["slept_at"] | static_cast<int64_t>(0);
  st.pushedBookId = doc["pushed_book_id"] | static_cast<uint32_t>(0);
  st.pushedPct = doc["pushed_pct"] | 0.0f;
  const JsonVariantConst pending = doc["pending"];
  if (pending.is<JsonObjectConst>()) {
    st.hasPending = true;
    st.pending.percentage = pending["pct"] | 0.0f;
    st.pending.chapterIndex = pending["chapter"] | 0;
    st.pending.pagePositionInBook = pending["pos"] | 0.0f;
  }
  return st;
}

void saveState(const State& st) {
  JsonDocument doc;
  doc["path"] = st.path;
  doc["book_id"] = st.bookId;
  doc["local_pct"] = st.localPct;
  doc["slept_at"] = st.sleptAt;
  doc["pushed_book_id"] = st.pushedBookId;
  doc["pushed_pct"] = st.pushedPct;
  if (st.hasPending) {
    JsonObject pending = doc["pending"].to<JsonObject>();
    pending["pct"] = st.pending.percentage;
    pending["chapter"] = st.pending.chapterIndex;
    pending["pos"] = st.pending.pagePositionInBook;
  }
  String json;
  serializeJson(doc, json);
  Storage.mkdir("/.crosspoint");
  if (!Storage.writeFile(STATE_FILE, json)) {
    LOG_ERR("BFAuto", "Failed to write state file");
  }
}

bool enabled() { return BF_TOKEN_STORE.hasToken() && BF_TOKEN_STORE.autoSyncEnabled(); }

// WIFI_STORE is otherwise loaded only when a WiFi screen opens.
bool haveSavedWifi() {
  if (WIFI_STORE.getCredentialCount() == 0) WIFI_STORE.loadFromFile();
  return WIFI_STORE.getCredentialCount() > 0;
}

bool waitForConnection(const unsigned long timeoutMs) {
  const unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    const wl_status_t status = WiFi.status();
    if (status == WL_CONNECTED) return true;
    if (status == WL_CONNECT_FAILED) return false;  // wrong password
    delay(100);
  }
  return false;
}

bool tryJoin(const WifiCredential& cred, const unsigned long timeoutMs) {
  LOG_DBG("BFAuto", "Joining %s", cred.ssid.c_str());
  if (cred.password.empty()) {
    WiFi.begin(cred.ssid.c_str());
  } else {
    WiFi.begin(cred.ssid.c_str(), cred.password.c_str());
  }
  if (waitForConnection(timeoutMs)) return true;
  WiFi.disconnect();
  return false;
}

// Joins a saved network without UI: the last-used one first, then any other
// saved network that a scan finds, all within WIFI_BUDGET_MS.
bool connectSavedWifi() {
  if (WiFi.status() == WL_CONNECTED) return true;
  if (!haveSavedWifi()) return false;

  const unsigned long start = millis();
  const auto remaining = [&]() -> unsigned long {
    const unsigned long used = millis() - start;
    return used >= WIFI_BUDGET_MS ? 0 : WIFI_BUDGET_MS - used;
  };

  // Same setup as WifiSelectionActivity: credentials come from WIFI_STORE, not NVS.
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);
  delay(100);
  uint8_t mac[6] = {};
  if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
    char hostname[sizeof("CrossPoint-Reader-") + 12];
    snprintf(hostname, sizeof(hostname), "CrossPoint-Reader-%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3],
             mac[4], mac[5]);
    WiFi.setHostname(hostname);
  }

  const std::string lastSsid = WIFI_STORE.getLastConnectedSsid();
  if (!lastSsid.empty()) {
    if (const auto cred = WIFI_STORE.findCredential(lastSsid)) {
      if (tryJoin(*cred, std::min(WIFI_TRY_MS, remaining()))) return true;
    }
  }

  if (remaining() < 3000) return false;
  const int16_t found = WiFi.scanNetworks();
  for (size_t i = 0; i < WIFI_STORE.getCredentialCount() && remaining() > 1000; i++) {
    const auto cred = WIFI_STORE.getCredentialAt(i);
    if (!cred || cred->ssid == lastSsid) continue;
    bool visible = false;
    for (int16_t n = 0; n < found && !visible; n++) {
      visible = WiFi.SSID(n) == cred->ssid.c_str();
    }
    if (visible && tryJoin(*cred, std::min(WIFI_TRY_MS, remaining()))) {
      WiFi.scanDelete();
      return true;
    }
  }
  WiFi.scanDelete();
  return false;
}

}  // namespace

void noteReading(const std::string& epubPath, const uint32_t bookId, const BookFusionPosition& position) {
  noted.valid = true;
  noted.path = epubPath;
  noted.bookId = bookId;
  noted.position = position;
}

void recordPushed(const uint32_t bookId, const float percentage) {
  State st = loadState();
  st.pushedBookId = bookId;
  st.pushedPct = percentage;
  saveState(st);
}

void syncBeforeSleep() {
  if (!noted.valid || !enabled()) return;

  State st = loadState();
  st.path = noted.path;
  st.bookId = noted.bookId;
  st.localPct = noted.position.percentage;
  st.sleptAt = static_cast<int64_t>(time(nullptr));

  const bool pushedThisBook = st.pushedBookId == noted.bookId;
  if (!BookFusionAutoSyncPolicy::needsPush(pushedThisBook, st.pushedPct, st.localPct)) {
    LOG_DBG("BFAuto", "Position unchanged since last push");
    saveState(st);
    return;
  }

  const unsigned long start = millis();
  if (!connectSavedWifi()) {
    LOG_DBG("BFAuto", "No saved network reachable; will retry next sleep");
    saveState(st);
    return;
  }
  WiFi.setSleep(false);  // modem sleep stalls short TLS exchanges

  BookFusionPosition remote;
  const auto fetched = BookFusionSyncClient::getProgress(noted.bookId, remote);
  if (fetched != BookFusionSyncClient::OK && fetched != BookFusionSyncClient::NOT_FOUND) {
    LOG_ERR("BFAuto", "Fetch before push failed: %s", BookFusionSyncClient::errorString(fetched));
    saveState(st);
    return;
  }

  const bool remoteKnown = fetched == BookFusionSyncClient::OK;
  if (!BookFusionAutoSyncPolicy::shouldPush(remoteKnown, remote.percentage, st.localPct, pushedThisBook,
                                            st.pushedPct)) {
    LOG_DBG("BFAuto", "Another device is ahead (%.1f%% vs %.1f%%), not pushing", remote.percentage, st.localPct);
    saveState(st);
    return;
  }

  const auto pushed = BookFusionSyncClient::setProgress(noted.bookId, noted.position);
  if (pushed == BookFusionSyncClient::OK) {
    st.pushedBookId = noted.bookId;
    st.pushedPct = st.localPct;
    LOG_DBG("BFAuto", "Pushed %.1f%% in %lums", st.localPct, millis() - start);
  } else {
    LOG_ERR("BFAuto", "Push failed: %s", BookFusionSyncClient::errorString(pushed));
  }
  saveState(st);
}

bool checkOnWake(const std::string& epubPath, GfxRenderer& renderer) {
  if (!enabled() || !haveSavedWifi()) return false;

  State st = loadState();
  if (st.bookId == 0 || st.path != epubPath) return false;
  if (BookFusionAutoSyncPolicy::isShortNap(st.sleptAt, static_cast<int64_t>(time(nullptr)))) {
    LOG_DBG("BFAuto", "Short nap, skipping wake check");
    return false;
  }

  {
    RenderLock lock;  // the boot screen may still be painting
    GUI.drawPopup(renderer, tr(STR_BF_CHECKING));
  }

  const bool connected = connectSavedWifi();
  bool usedTls = false;
  if (connected) {
    WiFi.setSleep(false);
    usedTls = true;
    BookFusionPosition remote;
    const auto fetched = BookFusionSyncClient::getProgress(st.bookId, remote);
    if (fetched == BookFusionSyncClient::OK && BookFusionAutoSyncPolicy::remoteAhead(remote.percentage, st.localPct)) {
      LOG_DBG("BFAuto", "BookFusion is ahead: %.1f%% vs %.1f%%", remote.percentage, st.localPct);
      st.hasPending = true;
      st.pending = remote;
      saveState(st);
    } else if (fetched != BookFusionSyncClient::OK && fetched != BookFusionSyncClient::NOT_FOUND) {
      LOG_ERR("BFAuto", "Wake check failed: %s", BookFusionSyncClient::errorString(fetched));
    }
  } else {
    LOG_DBG("BFAuto", "No saved network reachable on wake");
  }

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  return usedTls;
}

bool takePendingRemote(const std::string& epubPath, BookFusionPosition& out) {
  State st = loadState();
  if (!st.hasPending) return false;
  // Consumed whether or not it matches: a pending position for another book is stale.
  st.hasPending = false;
  saveState(st);
  if (st.path != epubPath) return false;
  out = st.pending;
  return true;
}

}  // namespace BookFusionAutoSync
