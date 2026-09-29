#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

/**
 * OAuth 1.0a (RFC 5849) request signing with HMAC-SHA1, as used by the
 * Instapaper Full API. Pure string code: nonce and timestamp are passed in so
 * signatures are deterministic under test.
 */
namespace OAuth1 {

struct Param {
  std::string key;
  std::string value;
};

struct Credentials {
  std::string consumerKey;
  std::string consumerSecret;
  std::string token;        // empty before xAuth has issued one
  std::string tokenSecret;  // empty before xAuth has issued one
};

// RFC 3986 percent-encoding: everything but ALPHA / DIGIT / "-" / "." / "_" / "~".
std::string percentEncode(const std::string& in);

std::string base64(const uint8_t* data, size_t len);

// application/x-www-form-urlencoded body for params (same encoding as signing).
std::string formEncode(const std::vector<Param>& params);

// METHOD&url&sorted-params, each part percent-encoded (RFC 5849 §3.4.1).
// params must include the oauth_* protocol params and all body params.
std::string signatureBaseString(const char* method, const std::string& url, std::vector<Param> params);

// base64(HMAC-SHA1(consumerSecret&tokenSecret, baseString)).
std::string sign(const std::string& baseString, const std::string& consumerSecret, const std::string& tokenSecret);

// Complete "OAuth ..." Authorization header value for a request whose
// form-encoded body carries requestParams.
std::string authorizationHeader(const char* method, const std::string& url, const std::vector<Param>& requestParams,
                                const Credentials& creds, const std::string& nonce, const std::string& timestamp);

// Parses "a=1&b=2" (e.g. the xAuth access_token response). Values are percent-decoded.
std::vector<Param> parseQueryString(const std::string& query);

}  // namespace OAuth1
