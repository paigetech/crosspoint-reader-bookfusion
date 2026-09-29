#include "BookFusionSyncClient.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <SecureHttpClient.h>

#include <cstdio>
#include <cstring>

#include "BookFusionTokenStore.h"

namespace {
void addAuthHeaders(freeink::SecureHttpClient& http) {
  http.addHeader("Authorization", "Bearer " + BF_TOKEN_STORE.getToken());
  http.addHeader("Accept", BookFusionSyncClient::API_ACCEPT);
}

void initHttp(freeink::SecureHttpClient& http) {
  http.setInsecure();
  http.setTimeout(30000);
}

// Persistent client kept alive across browse requests.
// beginSession() allocates it with setReuse(true); endSession() frees it.
// When no session is active, methods fall back to a local client.
freeink::SecureHttpClient* s_sessionClient = nullptr;

// Returns the session client if one exists, otherwise initialises and returns tmp.
// begin() clears headers and body on each call, so it is safe to call on the
// session client for each new request.
freeink::SecureHttpClient& resolveClient(freeink::SecureHttpClient& tmp) {
  if (s_sessionClient) return *s_sessionClient;
  initHttp(tmp);
  return tmp;
}
}  // namespace

void BookFusionSyncClient::beginSession() {
  if (s_sessionClient) {
    // Already have a live session — reuse it entirely.
    // Each TLS handshake leaves a permanent fragmentation scar on the heap
    // (MaxAlloc drops ~40KB per session). By the third re-entry the response
    // string's std::string::append() can't grow and calls abort(). Keeping the
    // same SecureHttpClient instance also preserves _body's allocated capacity
    // so the new response fits without a fresh large allocation.
    LOG_DBG("BFS", "Session reused (no handshake)");
    return;
  }
  s_sessionClient = new (std::nothrow) freeink::SecureHttpClient();
  if (!s_sessionClient) {
    LOG_ERR("BFS", "OOM: beginSession");
    return;
  }
  initHttp(*s_sessionClient);
  s_sessionClient->setReuse(true);
  LOG_DBG("BFS", "Session started");
}

void BookFusionSyncClient::endSession() {
  if (!s_sessionClient) return;
  delete s_sessionClient;
  s_sessionClient = nullptr;
  LOG_DBG("BFS", "Session ended");
}

// --- Device Code Auth ---

BookFusionSyncClient::Error BookFusionSyncClient::requestDeviceCode(BookFusionDeviceCodeResponse& out) {
  char url[128];
  snprintf(url, sizeof(url), "%s/api/user/auth/device", BASE_URL);
  LOG_DBG("BFS", "Requesting device code: %s", url);

  freeink::SecureHttpClient http;
  initHttp(http);
  if (!http.begin(url)) {
    LOG_ERR("BFS", "requestDeviceCode: begin failed");
    return NETWORK_ERROR;
  }
  http.addHeader("Accept", API_ACCEPT);
  http.addHeader("Content-Type", "application/json");

  JsonDocument body;
  body["client_id"] = CLIENT_ID;
  std::string bodyStr;
  serializeJson(body, bodyStr);

  const int httpCode = http.POST(bodyStr);
  LOG_DBG("BFS", "requestDeviceCode response: %d", httpCode);

  if (httpCode < 0) return NETWORK_ERROR;
  if (httpCode != 200) return SERVER_ERROR;

  JsonDocument doc;
  if (deserializeJson(doc, http.getString()) != DeserializationError::Ok) {
    LOG_ERR("BFS", "requestDeviceCode JSON parse error");
    return JSON_ERROR;
  }

  strlcpy(out.deviceCode, doc["device_code"] | "", sizeof(out.deviceCode));
  strlcpy(out.userCode, doc["user_code"] | "", sizeof(out.userCode));
  strlcpy(out.verificationUri, doc["verification_uri"] | "", sizeof(out.verificationUri));
  out.interval = doc["interval"] | 5;
  out.expiresIn = doc["expires_in"] | 600;

  LOG_DBG("BFS", "Device code received: user_code=%s, interval=%ds, expires_in=%ds", out.userCode, out.interval,
          out.expiresIn);
  return OK;
}

BookFusionSyncClient::Error BookFusionSyncClient::pollForToken(const char* deviceCode, char* outToken,
                                                               size_t tokenMaxLen) {
  char url[128];
  snprintf(url, sizeof(url), "%s/api/user/auth/token", BASE_URL);

  freeink::SecureHttpClient http;
  initHttp(http);
  if (!http.begin(url)) {
    LOG_ERR("BFS", "pollForToken: begin failed");
    return NETWORK_ERROR;
  }
  http.addHeader("Accept", API_ACCEPT);
  http.addHeader("Content-Type", "application/json");

  JsonDocument body;
  body["grant_type"] = DEVICE_CODE_GRANT_TYPE;
  body["client_id"] = CLIENT_ID;
  body["device_code"] = deviceCode;
  std::string bodyStr;
  serializeJson(body, bodyStr);

  const int httpCode = http.POST(bodyStr);
  LOG_DBG("BFS", "pollForToken response: %d", httpCode);

  if (httpCode < 0) return NETWORK_ERROR;

  JsonDocument doc;
  if (deserializeJson(doc, http.getString()) != DeserializationError::Ok) {
    LOG_ERR("BFS", "pollForToken JSON parse error");
    return JSON_ERROR;
  }

  if (httpCode == 200) {
    const char* token = doc["access_token"] | "";
    if (token[0] == '\0') return JSON_ERROR;
    strlcpy(outToken, token, tokenMaxLen);
    LOG_DBG("BFS", "Token received");
    return OK;
  }

  const char* errCode = doc["error"] | "";
  LOG_DBG("BFS", "pollForToken error: %s", errCode);

  if (strcmp(errCode, "authorization_pending") == 0) return PENDING;
  if (strcmp(errCode, "slow_down") == 0) return SLOW_DOWN;
  if (strcmp(errCode, "expired_token") == 0) return EXPIRED;
  if (strcmp(errCode, "access_denied") == 0) return DENIED;
  // BookFusion returns "invalid_grant" (HTTP 400) while authorization is still
  // pending — non-standard, but the official Lua plugin keeps polling on any
  // unrecognised error, so we do the same.
  if (strcmp(errCode, "invalid_grant") == 0) return PENDING;

  return SERVER_ERROR;
}

// --- Progress ---

BookFusionSyncClient::Error BookFusionSyncClient::getProgress(uint32_t bookId, BookFusionPosition& out) {
  if (!BF_TOKEN_STORE.hasToken()) return NO_TOKEN;

  char url[128];
  snprintf(url, sizeof(url), "%s/api/user/books/%lu/reading_position", BASE_URL, (unsigned long)bookId);
  LOG_DBG("BFS", "getProgress: %s", url);

  freeink::SecureHttpClient http;
  initHttp(http);
  if (!http.begin(url)) {
    LOG_ERR("BFS", "getProgress: begin failed");
    return NETWORK_ERROR;
  }
  addAuthHeaders(http);

  const int httpCode = http.GET();
  LOG_DBG("BFS", "getProgress response: %d", httpCode);

  if (httpCode < 0) return NETWORK_ERROR;
  if (httpCode == 401) return AUTH_FAILED;
  if (httpCode == 404) return NOT_FOUND;
  if (httpCode != 200) return SERVER_ERROR;

  JsonDocument doc;
  if (deserializeJson(doc, http.getString()) != DeserializationError::Ok) {
    LOG_ERR("BFS", "getProgress JSON parse error");
    return JSON_ERROR;
  }

  out.percentage = doc["percentage"] | 0.0f;
  out.chapterIndex = doc["chapter_index"] | 0;
  out.pagePositionInBook = doc["page_position_in_book"] | 0.0f;

  LOG_DBG("BFS", "Remote progress: %.2f%%, chapter %d", out.percentage, out.chapterIndex);
  return OK;
}

BookFusionSyncClient::Error BookFusionSyncClient::setProgress(uint32_t bookId, const BookFusionPosition& pos) {
  if (!BF_TOKEN_STORE.hasToken()) return NO_TOKEN;

  char url[128];
  snprintf(url, sizeof(url), "%s/api/user/books/%lu/reading_position", BASE_URL, (unsigned long)bookId);
  LOG_DBG("BFS", "setProgress: %s (%.2f%%)", url, pos.percentage);

  freeink::SecureHttpClient http;
  initHttp(http);
  if (!http.begin(url)) {
    LOG_ERR("BFS", "setProgress: begin failed");
    return NETWORK_ERROR;
  }
  addAuthHeaders(http);
  http.addHeader("Content-Type", "application/json");

  JsonDocument body;
  body["percentage"] = pos.percentage;
  body["chapter_index"] = pos.chapterIndex;
  body["page_position_in_book"] = pos.pagePositionInBook;
  std::string bodyStr;
  serializeJson(body, bodyStr);

  const int httpCode = http.POST(bodyStr);
  LOG_DBG("BFS", "setProgress response: %d", httpCode);

  if (httpCode == 200 || httpCode == 201) return OK;
  if (httpCode == 401) return AUTH_FAILED;
  if (httpCode < 0) return NETWORK_ERROR;
  return SERVER_ERROR;
}

// --- Library Browse & Download ---

BookFusionSyncClient::Error BookFusionSyncClient::searchBooks(int page, BookFusionSearchResult& out, const char* list,
                                                              const char* sort) {
  if (!BF_TOKEN_STORE.hasToken()) return NO_TOKEN;

  char url[128];
  snprintf(url, sizeof(url), "%s/api/user/books/search", BASE_URL);

  freeink::SecureHttpClient tmp;
  freeink::SecureHttpClient& http = resolveClient(tmp);
  if (!http.begin(url)) {
    LOG_ERR("BFS", "searchBooks: begin failed");
    return NETWORK_ERROR;
  }
  addAuthHeaders(http);
  http.addHeader("Content-Type", "application/json");

  static constexpr int BOOKS_PER_PAGE = 8;

  JsonDocument reqBody;
  reqBody["page"] = page;
  reqBody["per_page"] = BOOKS_PER_PAGE + 1;
  reqBody["sort"] = (sort != nullptr) ? sort : "added_at-desc";
  if (list != nullptr) reqBody["list"] = list;
  std::string bodyStr;
  serializeJson(reqBody, bodyStr);

  // Stream the response body directly to SD instead of buffering it in _body.
  // "All Books" raw JSON exceeds 60 KB; std::string::append() growing beyond
  // the post-handshake MaxAlloc calls abort() via -fno-exceptions.
  static constexpr char TMP_PATH[] = "/.bfs_tmp.json";
  int httpCode = -1;
  bool writeOk = true;
  {
    HalFile tmpFile;
    if (!Storage.openFileForWrite("BFS", TMP_PATH, tmpFile)) {
      LOG_ERR("BFS", "searchBooks: failed to open temp file");
      return SERVER_ERROR;
    }
    httpCode = http.sendRequest("POST", reinterpret_cast<const uint8_t*>(bodyStr.data()), bodyStr.size(),
                                [&tmpFile, &writeOk](const uint8_t* data, size_t len) -> bool {
                                  if (!writeOk) return false;
                                  if (tmpFile.write(data, len) != len) {
                                    writeOk = false;
                                    return false;
                                  }
                                  return true;
                                });
    LOG_DBG("BFS", "searchBooks page=%d response: %d", page, httpCode);
    // tmpFile closed at scope exit before Storage.remove() below
  }
  if (!writeOk || httpCode < 0) {
    Storage.remove(TMP_PATH);
    return NETWORK_ERROR;
  }
  if (httpCode == 401) {
    Storage.remove(TMP_PATH);
    return AUTH_FAILED;
  }
  if (httpCode != 200) {
    Storage.remove(TMP_PATH);
    return SERVER_ERROR;
  }

  // Parse from SD temp file — zero DRAM for the response body.
  JsonDocument filter;
  filter[0]["id"] = true;
  filter[0]["title"] = true;
  filter[0]["format"] = true;
  filter[0]["authors"][0]["name"] = true;

  JsonDocument doc;
  {
    HalFile readFile;
    if (!Storage.openFileForRead("BFS", TMP_PATH, readFile)) {
      LOG_ERR("BFS", "searchBooks: failed to open temp file for read");
      Storage.remove(TMP_PATH);
      return SERVER_ERROR;
    }
    struct HalFileReader {
      HalFile& f;
      int read() { return f.read(); }
      size_t readBytes(char* buf, size_t n) {
        const int r = f.read(buf, n);
        return r < 0 ? 0 : static_cast<size_t>(r);
      }
    } reader{readFile};

    const auto parseErr = deserializeJson(doc, reader, DeserializationOption::Filter(filter));
    // readFile closed at scope exit before Storage.remove() below
    if (parseErr != DeserializationError::Ok) {
      LOG_ERR("BFS", "searchBooks JSON parse error: %s", parseErr.c_str());
      Storage.remove(TMP_PATH);
      return JSON_ERROR;
    }
  }
  Storage.remove(TMP_PATH);

  if (!doc.is<JsonArray>()) {
    LOG_ERR("BFS", "searchBooks: expected JSON array");
    return JSON_ERROR;
  }

  JsonArray arr = doc.as<JsonArray>();
  out.count = 0;
  out.currentPage = page;
  out.hasMore = false;

  for (JsonObject book : arr) {
    if (out.count >= BOOKS_PER_PAGE) {
      out.hasMore = true;
      break;
    }

    BookFusionBook& b = out.books[out.count];
    b.id = book["id"] | static_cast<uint32_t>(0);
    if (b.id == 0) continue;

    strlcpy(b.title, book["title"] | "Untitled", sizeof(b.title));
    strlcpy(b.format, book["format"] | "epub", sizeof(b.format));

    b.authors[0] = '\0';
    JsonArray authors = book["authors"].as<JsonArray>();
    bool first = true;
    for (JsonObject author : authors) {
      const char* name = author["name"] | "";
      if (name[0] != '\0') {
        if (!first) strlcat(b.authors, ", ", sizeof(b.authors));
        strlcat(b.authors, name, sizeof(b.authors));
        first = false;
      }
    }

    out.count++;
  }

  LOG_DBG("BFS", "searchBooks: %d books on page %d, hasMore=%d", out.count, page, out.hasMore);
  return OK;
}

BookFusionSyncClient::Error BookFusionSyncClient::getDownloadUrl(uint32_t bookId, char* outUrl, size_t maxLen) {
  if (!BF_TOKEN_STORE.hasToken()) return NO_TOKEN;

  char url[128];
  snprintf(url, sizeof(url), "%s/api/user/books/%lu/download", BASE_URL, static_cast<unsigned long>(bookId));

  freeink::SecureHttpClient tmp;
  freeink::SecureHttpClient& http = resolveClient(tmp);
  if (!http.begin(url)) {
    LOG_ERR("BFS", "getDownloadUrl: begin failed");
    return NETWORK_ERROR;
  }
  addAuthHeaders(http);
  http.addHeader("Content-Type", "application/json");

  const int httpCode = http.POST("{}");
  LOG_DBG("BFS", "getDownloadUrl book=%lu response: %d", static_cast<unsigned long>(bookId), httpCode);

  if (httpCode < 0) return NETWORK_ERROR;
  if (httpCode == 401) return AUTH_FAILED;
  if (httpCode == 403 || httpCode == 404) return NOT_FOUND;
  if (httpCode != 200) return SERVER_ERROR;

  JsonDocument doc;
  if (deserializeJson(doc, http.getString()) != DeserializationError::Ok) {
    LOG_ERR("BFS", "getDownloadUrl JSON parse error");
    return JSON_ERROR;
  }

  const char* dlUrl = doc["url"] | "";
  if (dlUrl[0] == '\0') {
    LOG_ERR("BFS", "getDownloadUrl: missing url field");
    return JSON_ERROR;
  }

  strlcpy(outUrl, dlUrl, maxLen);
  LOG_DBG("BFS", "getDownloadUrl: ok");
  return OK;
}

const char* BookFusionSyncClient::errorString(Error error) {
  switch (error) {
    case OK:
      return "Success";
    case NO_TOKEN:
      return "Not logged in to BookFusion";
    case NETWORK_ERROR:
      return "Network error";
    case AUTH_FAILED:
      return "Authentication failed";
    case SERVER_ERROR:
      return "Server error (try again later)";
    case JSON_ERROR:
      return "JSON parse error";
    case NOT_FOUND:
      return "No progress found";
    case PENDING:
      return "Authorization pending";
    case SLOW_DOWN:
      return "Slow down polling";
    case EXPIRED:
      return "Device code expired";
    case DENIED:
      return "Authorization denied";
    default:
      return "Unknown error";
  }
}
