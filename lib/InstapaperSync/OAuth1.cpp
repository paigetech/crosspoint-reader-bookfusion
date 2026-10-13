#include "OAuth1.h"

#include <algorithm>

#include "HmacSha1.h"

namespace OAuth1 {
namespace {

bool isUnreserved(const unsigned char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
         c == '_' || c == '~';
}

int hexValue(const char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

std::string percentDecode(const std::string& in) {
  std::string out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size(); i++) {
    const char c = in[i];
    if (c == '%' && i + 2 < in.size()) {
      const int hi = hexValue(in[i + 1]);
      const int lo = hexValue(in[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out.push_back(static_cast<char>(hi * 16 + lo));
        i += 2;
        continue;
      }
    }
    out.push_back(c == '+' ? ' ' : c);
  }
  return out;
}

}  // namespace

std::string percentEncode(const std::string& in) {
  static constexpr char HEX[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(in.size() * 3);
  for (const char ch : in) {
    const auto c = static_cast<unsigned char>(ch);
    if (isUnreserved(c)) {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(HEX[c >> 4]);
      out.push_back(HEX[c & 0x0F]);
    }
  }
  return out;
}

std::string base64(const uint8_t* data, const size_t len) {
  static constexpr char TABLE[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((len + 2) / 3) * 4);
  size_t i = 0;
  for (; i + 2 < len; i += 3) {
    const uint32_t n = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
    out.push_back(TABLE[(n >> 18) & 63]);
    out.push_back(TABLE[(n >> 12) & 63]);
    out.push_back(TABLE[(n >> 6) & 63]);
    out.push_back(TABLE[n & 63]);
  }
  if (i + 1 == len) {
    const uint32_t n = data[i] << 16;
    out.push_back(TABLE[(n >> 18) & 63]);
    out.push_back(TABLE[(n >> 12) & 63]);
    out.append("==");
  } else if (i + 2 == len) {
    const uint32_t n = (data[i] << 16) | (data[i + 1] << 8);
    out.push_back(TABLE[(n >> 18) & 63]);
    out.push_back(TABLE[(n >> 12) & 63]);
    out.push_back(TABLE[(n >> 6) & 63]);
    out.push_back('=');
  }
  return out;
}

std::string formEncode(const std::vector<Param>& params) {
  std::string out;
  for (const auto& p : params) {
    if (!out.empty()) out.push_back('&');
    out += percentEncode(p.key);
    out.push_back('=');
    out += percentEncode(p.value);
  }
  return out;
}

std::string signatureBaseString(const char* method, const std::string& url, std::vector<Param> params) {
  // Sort by encoded key, then encoded value (RFC 5849 §3.4.1.3.2).
  for (auto& p : params) {
    p.key = percentEncode(p.key);
    p.value = percentEncode(p.value);
  }
  std::sort(params.begin(), params.end(),
            [](const Param& a, const Param& b) { return a.key != b.key ? a.key < b.key : a.value < b.value; });
  std::string normalized;
  for (const auto& p : params) {
    if (!normalized.empty()) normalized.push_back('&');
    normalized += p.key;
    normalized.push_back('=');
    normalized += p.value;
  }
  std::string base = method;
  base.push_back('&');
  base += percentEncode(url);
  base.push_back('&');
  base += percentEncode(normalized);
  return base;
}

std::string sign(const std::string& baseString, const std::string& consumerSecret, const std::string& tokenSecret) {
  const std::string key = percentEncode(consumerSecret) + "&" + percentEncode(tokenSecret);
  uint8_t digest[HmacSha1::DIGEST_SIZE];
  HmacSha1::hmac(reinterpret_cast<const uint8_t*>(key.data()), key.size(),
                 reinterpret_cast<const uint8_t*>(baseString.data()), baseString.size(), digest);
  return base64(digest, sizeof(digest));
}

std::string authorizationHeader(const char* method, const std::string& url, const std::vector<Param>& requestParams,
                                const Credentials& creds, const std::string& nonce, const std::string& timestamp) {
  std::vector<Param> oauthParams = {
      {"oauth_consumer_key", creds.consumerKey},
      {"oauth_nonce", nonce},
      {"oauth_signature_method", "HMAC-SHA1"},
      {"oauth_timestamp", timestamp},
      {"oauth_version", "1.0"},
  };
  if (!creds.token.empty()) oauthParams.push_back({"oauth_token", creds.token});

  std::vector<Param> all = oauthParams;
  all.insert(all.end(), requestParams.begin(), requestParams.end());
  const std::string signature = sign(signatureBaseString(method, url, all), creds.consumerSecret, creds.tokenSecret);
  oauthParams.push_back({"oauth_signature", signature});

  std::string header = "OAuth ";
  for (size_t i = 0; i < oauthParams.size(); i++) {
    if (i > 0) header += ", ";
    header += percentEncode(oauthParams[i].key);
    header += "=\"";
    header += percentEncode(oauthParams[i].value);
    header += "\"";
  }
  return header;
}

std::vector<Param> parseQueryString(const std::string& query) {
  std::vector<Param> out;
  size_t start = 0;
  while (start <= query.size()) {
    size_t end = query.find('&', start);
    if (end == std::string::npos) end = query.size();
    const std::string pair = query.substr(start, end - start);
    if (!pair.empty()) {
      const size_t eq = pair.find('=');
      if (eq == std::string::npos) {
        out.push_back({percentDecode(pair), ""});
      } else {
        out.push_back({percentDecode(pair.substr(0, eq)), percentDecode(pair.substr(eq + 1))});
      }
    }
    start = end + 1;
  }
  return out;
}

}  // namespace OAuth1
