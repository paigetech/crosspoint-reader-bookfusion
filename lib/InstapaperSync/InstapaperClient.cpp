#include "InstapaperClient.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <SecureHttpClient.h>
#include <WiFi.h>
#include <esp_random.h>
#include <esp_sntp.h>
#include <time.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "InstapaperStore.h"
#include "OAuth1.h"

namespace {

constexpr char BASE_URL[] = "https://www.instapaper.com/api/1/";
constexpr char LIST_TMP_PATH[] = "/.crosspoint/ip_list.json";
constexpr uint32_t HTTP_TIMEOUT_MS = 30000;
// Anything before 2023 means the clock was never set.
constexpr time_t MIN_VALID_EPOCH = 1672531200;

char s_lastError[128] = "";

void setLastError(const char* msg) { strlcpy(s_lastError, msg ? msg : "", sizeof(s_lastError)); }

std::string makeNonce() {
  char buf[17];
  snprintf(buf, sizeof(buf), "%08lx%08lx", static_cast<unsigned long>(esp_random()),
           static_cast<unsigned long>(esp_random()));
  return buf;
}

OAuth1::Credentials credentials(const bool withToken) {
  OAuth1::Credentials c;
  c.consumerKey = INSTAPAPER_STORE.getConsumerKey();
  c.consumerSecret = INSTAPAPER_STORE.getConsumerSecret();
  if (withToken) {
    c.token = INSTAPAPER_STORE.getToken();
    c.tokenSecret = INSTAPAPER_STORE.getTokenSecret();
  }
  return c;
}

// Records the API's error message from a JSON error body:
// [{"type":"error","error_code":1240,"message":"..."}].
void captureApiError(const std::string& body) {
  JsonDocument doc;
  if (deserializeJson(doc, body) != DeserializationError::Ok) {
    setLastError(body.substr(0, 100).c_str());
    return;
  }
  JsonVariantConst err = doc.as<JsonVariantConst>();
  if (doc.is<JsonArray>()) err = err[0];
  char msg[128];
  snprintf(msg, sizeof(msg), "%d: %s", err["error_code"] | 0, err["message"] | "");
  setLastError(msg);
}

InstapaperClient::Error statusToError(const int status) {
  if (status < 0) return InstapaperClient::NETWORK_ERROR;
  if (status == 401 || status == 403) return InstapaperClient::AUTH_FAILED;
  return InstapaperClient::SERVER_ERROR;
}

// Signed POST. With onData the body streams there; otherwise it is buffered
// and returned in bodyOut.
int signedPost(const char* endpoint, const std::vector<OAuth1::Param>& params, const bool withToken,
               std::string* bodyOut, const freeink::SecureHttpClient::DataCallback& onData = nullptr) {
  const std::string url = std::string(BASE_URL) + endpoint;
  const std::string auth = OAuth1::authorizationHeader("POST", url, params, credentials(withToken), makeNonce(),
                                                       std::to_string(static_cast<long long>(time(nullptr))));
  const std::string body = OAuth1::formEncode(params);

  freeink::SecureHttpClient http;
  http.setInsecure();  // matches the rest of CrossPoint's network code
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(url)) {
    LOG_ERR("IPC", "begin failed: %s", url.c_str());
    return -1;
  }
  http.addHeader("Authorization", auth);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");

  int status;
  if (onData) {
    status = http.sendRequest("POST", reinterpret_cast<const uint8_t*>(body.data()), body.size(), onData);
  } else {
    status = http.POST(body);
    if (bodyOut) *bodyOut = http.getString();
  }
  LOG_DBG("IPC", "POST %s -> %d", endpoint, status);
  return status;
}

void hostOf(const char* url, char* out, const size_t outLen) {
  const char* p = strstr(url, "://");
  p = p ? p + 3 : url;
  if (strncmp(p, "www.", 4) == 0) p += 4;
  size_t n = 0;
  while (p[n] && p[n] != '/' && p[n] != ':' && p[n] != '?' && n + 1 < outLen) n++;
  memcpy(out, p, n);
  out[n] = '\0';
}

}  // namespace

bool InstapaperClient::ensureClock() {
  if (time(nullptr) >= MIN_VALID_EPOCH) return true;
  if (WiFi.status() != WL_CONNECTED) return false;

  // Same approach as HalClock::syncFromNTP, which only runs on boards with an
  // RTC: configTzTime switches TZ to UTC for the exchange, so restore it after.
  const char* tzBefore = getenv("TZ");
  char savedTz[64] = {};
  if (tzBefore) strlcpy(savedTz, tzBefore, sizeof(savedTz));
  configTzTime("UTC0", "pool.ntp.org", "time.nist.gov");
  bool synced = false;
  for (int i = 0; i < 100 && !synced; i++) {
    synced = sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED && time(nullptr) >= MIN_VALID_EPOCH;
    if (!synced) delay(100);
  }
  if (savedTz[0]) {
    setenv("TZ", savedTz, 1);
    tzset();
  }
  LOG_INF("IPC", "NTP sync %s", synced ? "ok" : "failed");
  return synced;
}

InstapaperClient::Error InstapaperClient::signIn(const std::string& username, const std::string& password) {
  setLastError("");
  if (!INSTAPAPER_STORE.hasConsumerKey()) return NO_KEY;
  if (!ensureClock()) return CLOCK_ERROR;

  std::string body;
  int status = signedPost(
      "oauth/access_token",
      {{"x_auth_username", username}, {"x_auth_password", password}, {"x_auth_mode", "client_auth"}}, false, &body);
  if (status != 200) {
    setLastError(body.substr(0, 100).c_str());
    return statusToError(status);
  }

  std::string token;
  std::string tokenSecret;
  for (const auto& p : OAuth1::parseQueryString(body)) {
    if (p.key == "oauth_token") token = p.value;
    if (p.key == "oauth_token_secret") tokenSecret = p.value;
  }
  if (token.empty() || tokenSecret.empty()) {
    LOG_ERR("IPC", "access_token response missing token");
    return JSON_ERROR;
  }

  // Store the session first so verify_credentials is signed with the token.
  INSTAPAPER_STORE.setSession(token, tokenSecret, username);
  status = signedPost("account/verify_credentials", {}, true, &body);
  if (status == 200) {
    JsonDocument doc;
    if (deserializeJson(doc, body) == DeserializationError::Ok) {
      for (JsonVariantConst item : doc.as<JsonArrayConst>()) {
        const char* name = item["username"] | "";
        if (strcmp(item["type"] | "", "user") == 0 && name[0]) {
          INSTAPAPER_STORE.setSession(token, tokenSecret, name);
          break;
        }
      }
    }
  } else {
    // The token works even if this lookup fails; keep the typed username.
    LOG_ERR("IPC", "verify_credentials failed: %d", status);
  }
  return OK;
}

InstapaperClient::Error InstapaperClient::listBookmarks(const char* folderId, InstapaperListResult& out) {
  setLastError("");
  out.count = 0;
  if (!INSTAPAPER_STORE.hasConsumerKey()) return NO_KEY;
  if (!INSTAPAPER_STORE.isSignedIn()) return NOT_SIGNED_IN;
  if (!ensureClock()) return CLOCK_ERROR;

  // Stream to SD: the list carries descriptions and can run to tens of KB.
  int status;
  bool writeOk = true;
  {
    HalFile file;
    if (!Storage.openFileForWrite("IPC", LIST_TMP_PATH, file)) return FILE_ERROR;
    char limit[8];
    snprintf(limit, sizeof(limit), "%d", InstapaperListResult::MAX);
    status = signedPost("bookmarks/list", {{"folder_id", folderId}, {"limit", limit}}, true, nullptr,
                        [&file, &writeOk](const uint8_t* data, size_t len) {
                          writeOk = writeOk && file.write(data, len) == len;
                          return writeOk;
                        });
  }
  if (!writeOk) {
    Storage.remove(LIST_TMP_PATH);
    return FILE_ERROR;
  }
  if (status != 200) {
    captureApiError(std::string(Storage.readFile(LIST_TMP_PATH).c_str()));
    Storage.remove(LIST_TMP_PATH);
    return statusToError(status);
  }

  JsonDocument filter;
  filter[0]["type"] = true;
  filter[0]["bookmark_id"] = true;
  filter[0]["title"] = true;
  filter[0]["url"] = true;
  filter[0]["progress"] = true;

  JsonDocument doc;
  {
    HalFile file;
    if (!Storage.openFileForRead("IPC", LIST_TMP_PATH, file)) return FILE_ERROR;
    struct Reader {
      HalFile& f;
      int read() { return f.read(); }
      size_t readBytes(char* buf, size_t n) {
        const int r = f.read(buf, n);
        return r < 0 ? 0 : static_cast<size_t>(r);
      }
    } reader{file};
    const auto err = deserializeJson(doc, reader, DeserializationOption::Filter(filter));
    if (err) {
      LOG_ERR("IPC", "bookmarks/list JSON: %s", err.c_str());
      Storage.remove(LIST_TMP_PATH);
      return JSON_ERROR;
    }
  }
  Storage.remove(LIST_TMP_PATH);

  for (JsonVariantConst item : doc.as<JsonArrayConst>()) {
    if (out.count >= InstapaperListResult::MAX) break;
    if (strcmp(item["type"] | "", "bookmark") != 0) continue;
    InstapaperBookmark& b = out.items[out.count];
    b.id = item["bookmark_id"] | static_cast<uint32_t>(0);
    if (b.id == 0) continue;
    strlcpy(b.title, item["title"] | "Untitled", sizeof(b.title));
    hostOf(item["url"] | "", b.site, sizeof(b.site));
    b.progress = item["progress"] | 0.0f;
    out.count++;
  }
  LOG_DBG("IPC", "bookmarks/list %s: %d", folderId, out.count);
  return OK;
}

InstapaperClient::Error InstapaperClient::downloadText(const uint32_t bookmarkId, const char* destPath,
                                                       const std::function<bool(size_t)>& progress) {
  setLastError("");
  if (!INSTAPAPER_STORE.isSignedIn()) return NOT_SIGNED_IN;
  if (!ensureClock()) return CLOCK_ERROR;

  char id[12];
  snprintf(id, sizeof(id), "%lu", static_cast<unsigned long>(bookmarkId));
  int status;
  bool writeOk = true;
  bool cancelled = false;
  size_t total = 0;
  {
    HalFile file;
    if (!Storage.openFileForWrite("IPC", destPath, file)) return FILE_ERROR;
    status =
        signedPost("bookmarks/get_text", {{"bookmark_id", id}}, true, nullptr, [&](const uint8_t* data, size_t len) {
          if (file.write(data, len) != len) {
            writeOk = false;
            return false;
          }
          total += len;
          if (progress && !progress(total)) {
            cancelled = true;
            return false;
          }
          return true;
        });
  }
  if (cancelled || !writeOk || status != 200) {
    if (status >= 400) captureApiError(std::string(Storage.readFile(destPath).c_str()));
    Storage.remove(destPath);
    if (cancelled) return CANCELLED;
    if (!writeOk) return FILE_ERROR;
    // 1550: "Error generating text version of this URL".
    if (status == 400 && strstr(s_lastError, "1550")) return TEXT_UNAVAILABLE;
    return statusToError(status);
  }
  LOG_DBG("IPC", "get_text %lu: %u bytes", static_cast<unsigned long>(bookmarkId), static_cast<unsigned>(total));
  return OK;
}

const char* InstapaperClient::errorString(const Error error) {
  switch (error) {
    case OK:
      return "Success";
    case NO_KEY:
      return "No Instapaper API key";
    case NOT_SIGNED_IN:
      return "Not signed in to Instapaper";
    case CLOCK_ERROR:
      return "Could not set the clock";
    case NETWORK_ERROR:
      return "Network error";
    case AUTH_FAILED:
      return "Authentication failed";
    case SERVER_ERROR:
      return "Server error";
    case JSON_ERROR:
      return "Unexpected response";
    case TEXT_UNAVAILABLE:
      return "No text version of this article";
    case FILE_ERROR:
      return "SD card error";
    case CANCELLED:
      return "Cancelled";
  }
  return "Unknown error";
}

const char* InstapaperClient::lastErrorMessage() { return s_lastError; }
