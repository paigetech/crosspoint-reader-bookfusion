#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

struct InstapaperBookmark {
  uint32_t id = 0;
  char title[96] = {};
  char site[48] = {};  // host part of the article URL
  float progress = 0.0f;
};

struct InstapaperListResult {
  static constexpr int MAX = 40;
  InstapaperBookmark items[MAX];
  int count = 0;
};

/**
 * Instapaper Full API client (https://www.instapaper.com/api/1/...).
 *
 * Every call is a form-encoded POST signed with OAuth 1.0a HMAC-SHA1 using the
 * consumer key from INSTAPAPER_STORE; after signIn() the access token too.
 * Large responses (the bookmark list, article HTML) stream to the SD card.
 * Requires WiFi; signing needs a correct clock, which ensureClock() provides.
 */
class InstapaperClient {
 public:
  enum Error {
    OK = 0,
    NO_KEY,            // no consumer key imported
    NOT_SIGNED_IN,     // no access token
    CLOCK_ERROR,       // could not get the time for oauth_timestamp
    NETWORK_ERROR,     // TLS/HTTP failure
    AUTH_FAILED,       // 401/403, bad credentials or revoked token
    SERVER_ERROR,      // other non-2xx response
    JSON_ERROR,        // unexpected response shape
    TEXT_UNAVAILABLE,  // Instapaper could not produce a text version
    FILE_ERROR,        // SD write failed
    CANCELLED,
  };

  static Error signIn(const std::string& username, const std::string& password);
  // folderId: "unread", "starred", "archive" or a numeric folder id.
  static Error listBookmarks(const char* folderId, InstapaperListResult& out);
  // Streams the article's processed HTML to destPath. progress(bytes) may
  // return false to cancel.
  static Error downloadText(uint32_t bookmarkId, const char* destPath,
                            const std::function<bool(size_t bytes)>& progress = nullptr);

  // Makes sure the system clock is set (NTP if needed). OAuth timestamps
  // outside Instapaper's window are rejected as unauthorized.
  static bool ensureClock();

  static const char* errorString(Error error);
  // The API's own message for the last failed call, or "" if none.
  static const char* lastErrorMessage();
};
