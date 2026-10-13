// Host tests for the Instapaper download pipeline: OAuth 1.0a signing,
// HTML → XHTML sanitizing, and the single-article EPUB writer.

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "ArticleSanitizer.h"
#include "EpubPackager.h"
#include "HmacSha1.h"
#include "OAuth1.h"

namespace {

std::string hex(const uint8_t* d, size_t n) {
  std::string s;
  char b[3];
  for (size_t i = 0; i < n; i++) {
    snprintf(b, sizeof(b), "%02x", d[i]);
    s += b;
  }
  return s;
}

// --- HMAC-SHA1 ---------------------------------------------------------------

TEST(HmacSha1, Sha1KnownVectors) {
  uint8_t d[20];
  HmacSha1::sha1(reinterpret_cast<const uint8_t*>("abc"), 3, d);
  EXPECT_EQ(hex(d, 20), "a9993e364706816aba3e25717850c26c9cd0d89d");
  HmacSha1::sha1(reinterpret_cast<const uint8_t*>(""), 0, d);
  EXPECT_EQ(hex(d, 20), "da39a3ee5e6b4b0d3255bfef95601890afd80709");
  const std::string two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  HmacSha1::sha1(reinterpret_cast<const uint8_t*>(two.data()), two.size(), d);
  EXPECT_EQ(hex(d, 20), "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
}

TEST(HmacSha1, Rfc2202Vectors) {
  uint8_t d[20];
  const std::string msg2 = "what do ya want for nothing?";
  HmacSha1::hmac(reinterpret_cast<const uint8_t*>("Jefe"), 4, reinterpret_cast<const uint8_t*>(msg2.data()),
                 msg2.size(), d);
  EXPECT_EQ(hex(d, 20), "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79");

  // Test case 6: key longer than the block size is hashed first.
  std::vector<uint8_t> key(80, 0xaa);
  const std::string msg6 = "Test Using Larger Than Block-Size Key - Hash Key First";
  HmacSha1::hmac(key.data(), key.size(), reinterpret_cast<const uint8_t*>(msg6.data()), msg6.size(), d);
  EXPECT_EQ(hex(d, 20), "aa4ae5e15272d00e95705637ce8a3b55ed402112");
}

// --- OAuth 1.0a --------------------------------------------------------------

TEST(OAuth1, PercentEncodeFollowsRfc3986) {
  EXPECT_EQ(OAuth1::percentEncode("Ladies + Gentlemen"), "Ladies%20%2B%20Gentlemen");
  EXPECT_EQ(OAuth1::percentEncode("An encoded string!"), "An%20encoded%20string%21");
  EXPECT_EQ(OAuth1::percentEncode("Dogs, Cats & Mice"), "Dogs%2C%20Cats%20%26%20Mice");
  EXPECT_EQ(OAuth1::percentEncode("-._~"), "-._~");
  EXPECT_EQ(OAuth1::percentEncode("\xE2\x98\x83"), "%E2%98%83");
}

TEST(OAuth1, Base64) {
  const auto b64 = [](const std::string& s) {
    return OAuth1::base64(reinterpret_cast<const uint8_t*>(s.data()), s.size());
  };
  EXPECT_EQ(b64(""), "");
  EXPECT_EQ(b64("f"), "Zg==");
  EXPECT_EQ(b64("fo"), "Zm8=");
  EXPECT_EQ(b64("foo"), "Zm9v");
  EXPECT_EQ(b64("foobar"), "Zm9vYmFy");
}

// The worked example from Twitter's "Creating a signature" documentation, the
// usual reference vector for OAuth 1.0a HMAC-SHA1.
TEST(OAuth1, SignsReferenceRequest) {
  OAuth1::Credentials creds{"xvz1evFS4wEEPTGEFPHBog", "kAcSOqF21Fu85e7zjz7ZN2U4ZRhfV3WpwPAoE3Z7kBw",
                            "370773112-GmHxMAgYyLbNEtIKZeRNFsMKPR9EyMZeS9weJAEb",
                            "LswwdoUaIvS8ltyTt5jkRh4J50vUPVVHtR2YPi5kE"};
  const std::vector<OAuth1::Param> body = {{"status", "Hello Ladies + Gentlemen, a signed OAuth request!"},
                                           {"include_entities", "true"}};
  const std::string header =
      OAuth1::authorizationHeader("POST", "https://api.twitter.com/1.1/statuses/update.json", body, creds,
                                  "kYjzVBB8Y0ZFabxSWbWovY3uYSQ2pTgmZeNu2VS4cg", "1318622958");
  EXPECT_NE(header.find("oauth_signature=\"hCtSmYh%2BiHYCEqBWrE7C7hYmtUk%3D\""), std::string::npos) << header;
  EXPECT_EQ(header.rfind("OAuth ", 0), 0u);
  EXPECT_NE(header.find("oauth_token=\"370773112-GmHxMAgYyLbNEtIKZeRNFsMKPR9EyMZeS9weJAEb\""), std::string::npos);
}

TEST(OAuth1, OmitsTokenBeforeXAuth) {
  OAuth1::Credentials creds{"key", "secret", "", ""};
  const std::string header = OAuth1::authorizationHeader(
      "POST", "https://www.instapaper.com/api/1/oauth/access_token",
      {{"x_auth_username", "a@b.c"}, {"x_auth_password", "pw"}, {"x_auth_mode", "client_auth"}}, creds, "n", "1");
  EXPECT_EQ(header.find("oauth_token="), std::string::npos);
}

TEST(OAuth1, ParsesAccessTokenResponse) {
  const auto params = OAuth1::parseQueryString("oauth_token_secret=s%2Fec&oauth_token=tok");
  ASSERT_EQ(params.size(), 2u);
  EXPECT_EQ(params[0].key, "oauth_token_secret");
  EXPECT_EQ(params[0].value, "s/ec");
  EXPECT_EQ(params[1].value, "tok");
}

// --- ArticleSanitizer --------------------------------------------------------

// Checks the output is well-formed for our purposes: balanced tags from the
// allowed set, only &amp;/&lt;/&gt; entities, no raw '<' in text, valid UTF-8.
::testing::AssertionResult isWellFormed(const std::string& s) {
  std::vector<std::string> stack;
  for (size_t i = 0; i < s.size(); i++) {
    const char c = s[i];
    if (c == '<') {
      const size_t end = s.find('>', i);
      if (end == std::string::npos) return ::testing::AssertionFailure() << "unterminated tag at " << i;
      std::string tag = s.substr(i + 1, end - i - 1);
      if (tag == "br/" || tag == "hr/") {
      } else if (!tag.empty() && tag[0] == '/') {
        if (stack.empty() || stack.back() != tag.substr(1)) {
          return ::testing::AssertionFailure() << "mismatched </" << tag.substr(1) << "> at " << i << " in: " << s;
        }
        stack.pop_back();
      } else {
        for (const char t : tag) {
          if (!isalnum(static_cast<unsigned char>(t))) return ::testing::AssertionFailure() << "attr in tag " << tag;
        }
        stack.push_back(tag);
      }
      i = end;
    } else if (c == '&') {
      if (s.compare(i, 5, "&amp;") != 0 && s.compare(i, 4, "&lt;") != 0 && s.compare(i, 4, "&gt;") != 0) {
        return ::testing::AssertionFailure() << "bad entity at " << i << ": " << s.substr(i, 10);
      }
    } else if (c == '>') {
      return ::testing::AssertionFailure() << "raw > at " << i;
    }
  }
  if (!stack.empty()) return ::testing::AssertionFailure() << "unclosed <" << stack.back() << ">";

  // UTF-8 validity and XML-legal characters.
  for (size_t i = 0; i < s.size();) {
    const auto b = static_cast<unsigned char>(s[i]);
    size_t n = 0;
    uint32_t cp = 0;
    if (b < 0x80) {
      n = 1;
      cp = b;
    } else if ((b & 0xE0) == 0xC0) {
      n = 2;
      cp = b & 0x1F;
    } else if ((b & 0xF0) == 0xE0) {
      n = 3;
      cp = b & 0x0F;
    } else if ((b & 0xF8) == 0xF0) {
      n = 4;
      cp = b & 0x07;
    } else {
      return ::testing::AssertionFailure() << "bad UTF-8 lead at " << i;
    }
    if (i + n > s.size()) return ::testing::AssertionFailure() << "truncated UTF-8 at " << i;
    for (size_t k = 1; k < n; k++) {
      const auto cb = static_cast<unsigned char>(s[i + k]);
      if ((cb & 0xC0) != 0x80) return ::testing::AssertionFailure() << "bad UTF-8 continuation at " << i;
      cp = (cp << 6) | (cb & 0x3F);
    }
    const bool legal = cp == 0x9 || cp == 0xA || cp == 0xD || (cp >= 0x20 && cp <= 0xD7FF) ||
                       (cp >= 0xE000 && cp <= 0xFFFD) || (cp >= 0x10000 && cp <= 0x10FFFF);
    if (!legal) return ::testing::AssertionFailure() << "illegal XML char U+" << std::hex << cp << " at " << i;
    i += n;
  }
  return ::testing::AssertionSuccess();
}

std::string sanitizeChunked(const std::string& html, size_t chunk) {
  std::string out;
  ArticleSanitizer s([&](const char* d, size_t n) {
    out.append(d, n);
    return true;
  });
  for (size_t i = 0; i < html.size(); i += chunk) {
    s.feed(html.data() + i, std::min(chunk, html.size() - i));
  }
  s.finish();
  return out;
}

// Sanitizes whole, then byte-by-byte and in odd chunks, and requires identical,
// well-formed output: state must survive every chunk boundary.
std::string sanitize(const std::string& html) {
  const std::string whole = sanitizeChunked(html, html.size() + 1);
  EXPECT_TRUE(isWellFormed(whole)) << "input: " << html;
  EXPECT_EQ(sanitizeChunked(html, 1), whole) << "input: " << html;
  EXPECT_EQ(sanitizeChunked(html, 7), whole) << "input: " << html;
  return whole;
}

TEST(ArticleSanitizer, KeepsBasicStructure) {
  EXPECT_EQ(sanitize("<p>Hello <b>world</b></p><p>Second</p>"), "<p>Hello <strong>world</strong></p><p>Second</p>");
  EXPECT_EQ(sanitize("<h1>Title</h1><p>Body</p>"), "<h1>Title</h1><p>Body</p>");
  EXPECT_EQ(sanitize("<ul><li>one</li><li>two</li></ul>"), "<ul><li>one</li><li>two</li></ul>");
  EXPECT_EQ(sanitize("<blockquote><p>q</p></blockquote>"), "<blockquote><p>q</p></blockquote>");
}

TEST(ArticleSanitizer, WrapsLooseTextAndCollapsesWhitespace) {
  EXPECT_EQ(sanitize("  loose   text\n here  "), "<p>loose text here</p>");
  EXPECT_EQ(sanitize("<div>a</div><div>b</div>"), "<p>a</p><p>b</p>");
  EXPECT_EQ(sanitize("<p>  lead and trail  </p>"), "<p>lead and trail</p>");
}

TEST(ArticleSanitizer, DropsAttributesLinksAndImages) {
  EXPECT_EQ(sanitize("<p class=\"x\" style='a>b'>Go <a href=\"http://x?a=1&b=2\">here</a><img src=x.png/></p>"),
            "<p>Go here</p>");
}

TEST(ArticleSanitizer, SkipsScriptsStylesAndHead) {
  EXPECT_EQ(sanitize("<html><head><title>T</title><style>p{color:red}</style></head>"
                     "<body><script>if (a<b && c>d) { x = '</p>'; }</script><p>ok</p></body></html>"),
            "<p>ok</p>");
  EXPECT_EQ(sanitize("<p>a<svg><text>no</text><svg><g/></svg></svg>b</p>"), "<p>ab</p>");
  EXPECT_EQ(sanitize("<p>a<!-- <p>hidden</p> -- -->b</p>"), "<p>ab</p>");
}

TEST(ArticleSanitizer, DecodesEntities) {
  EXPECT_EQ(sanitize("<p>caf&eacute; &amp; cr&#232;me &#x2014; 5 &lt; 6 &nbsp;x</p>"),
            "<p>caf\xC3\xA9 &amp; cr\xC3\xA8me \xE2\x80\x94 5 &lt; 6 \xC2\xA0x</p>");
  EXPECT_EQ(sanitize("<p>AT&T &bogus; &amp</p>"), "<p>AT&amp;T &amp;bogus; &amp;</p>");
  EXPECT_EQ(sanitize("<p>&#0; &#xD800; &#1114112;</p>"), "<p>\xEF\xBF\xBD \xEF\xBF\xBD \xEF\xBF\xBD</p>");
}

TEST(ArticleSanitizer, RepairsBrokenNesting) {
  EXPECT_EQ(sanitize("<p>one<p>two"), "<p>one</p><p>two</p>");
  EXPECT_EQ(sanitize("<p><b>bold <i>both</b> after</i></p>"), "<p><strong>bold <em>both</em></strong> after</p>");
  EXPECT_EQ(sanitize("</p></div>text</li>"), "<p>text</p>");
  EXPECT_EQ(sanitize("<ul><li>a<li>b</ul>"), "<ul><li>a</li><li>b</li></ul>");
  EXPECT_EQ(sanitize("<li><div>item</div></li>"), "<p>item</p>");
  EXPECT_EQ(sanitize("<ul><li><div>item</div></li></ul>"), "<ul><li>item</li></ul>");
  EXPECT_EQ(sanitize("<ul><li><p>item</p></li></ul>"), "<ul><li>item</li></ul>");
  EXPECT_EQ(sanitize("<ul><li>a<ul><li>b</li></ul></li></ul>"), "<ul><li>a<ul><li>b</li></ul></li></ul>");
}

TEST(ArticleSanitizer, OmitsEmptyElements) {
  EXPECT_EQ(sanitize("<p><img src=x></p><ul><li><a href=y></a></li></ul><blockquote> </blockquote><p>kept</p>"),
            "<p>kept</p>");
  EXPECT_EQ(sanitize("<div><p> </p></div>"), "");
}

TEST(ArticleSanitizer, PreservesPreformattedText) {
  EXPECT_EQ(sanitize("<pre>  a\n    b &lt;c&gt;</pre>"), "<pre>  a\n    b &lt;c&gt;</pre>");
}

TEST(ArticleSanitizer, BreaksAndRules) {
  EXPECT_EQ(sanitize("<p>a<br>b<br/>c</p><hr><p>d</p>"), "<p>a<br/>b<br/>c</p><hr/><p>d</p>");
  EXPECT_EQ(sanitize("<table><tr><td>a</td><td>b</td></tr></table>"), "<p>a b</p>");
}

TEST(ArticleSanitizer, RepairsInvalidUtf8AndControls) {
  EXPECT_EQ(sanitize(std::string("<p>a\xFF"
                                 "b\xC3"
                                 "c\x01"
                                 "d\xE2\x82\xAC</p>")),
            "<p>a\xEF\xBF\xBD"
            "b\xEF\xBF\xBD"
            "cd\xE2\x82\xAC</p>");
  EXPECT_EQ(sanitize(std::string("<p>\xED\xA0\x80</p>")), "<p>\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD</p>");
}

TEST(ArticleSanitizer, LiteralLessThan) { EXPECT_EQ(sanitize("<p>a < b > c</p>"), "<p>a &lt; b &gt; c</p>"); }

TEST(ArticleSanitizer, DepthIsBounded) {
  std::string html;
  for (int i = 0; i < 200; i++) html += "<blockquote>";
  html += "deep";
  const std::string out = sanitize(html);
  EXPECT_NE(out.find("deep"), std::string::npos);
}

TEST(ArticleSanitizer, RandomInputIsAlwaysWellFormed) {
  // Tag soup from a fixed seed: every output must pass isWellFormed().
  static const char* PIECES[] = {
      "<p>",          "</p>",           "<div>",  "</div>",      "<b>",  "</b>",     "<i>",     "</i>",
      "<ul>",         "</ul>",          "<li>",   "</li>",       "<h2>", "</h2>",    "<pre>",   "</pre>",
      "<blockquote>", "</blockquote>",  "<br>",   "<hr/>",       "text", " ",        "\n",      "&amp;",
      "&nbsp;",       "&#169;",         "&",      "<",           ">",    "<!--",     "-->",     "<script>",
      "</script>",    "<a href='x>y'>", "</a>",   "\xC3\xA9",    "\xFF", "\xE2\x82", "<table>", "<td>",
      "</td>",        "<svg>",          "</svg>", "<img src=x>", "<ol>", "</ol>",    "<code>",  "</code>"};
  std::mt19937 rng(12345);
  for (int iter = 0; iter < 400; iter++) {
    std::string html;
    const int n = static_cast<int>(rng() % 60);
    for (int k = 0; k < n; k++) html += PIECES[rng() % (sizeof(PIECES) / sizeof(PIECES[0]))];
    sanitize(html);
  }
}

// --- EpubPackager ------------------------------------------------------------

TEST(EpubPackager, Crc32KnownValue) {
  EXPECT_EQ(EpubPackager::crc32Update(0, reinterpret_cast<const uint8_t*>("123456789"), 9), 0xCBF43926u);
  // Incremental updates match a single pass.
  uint32_t c = EpubPackager::crc32Update(0, reinterpret_cast<const uint8_t*>("1234"), 4);
  c = EpubPackager::crc32Update(c, reinterpret_cast<const uint8_t*>("56789"), 5);
  EXPECT_EQ(c, 0xCBF43926u);
}

TEST(EpubPackager, XmlEscape) {
  EXPECT_EQ(EpubPackager::xmlEscape("A & B <C> \"D\"\x01"), "A &amp; B &lt;C&gt; &quot;D&quot;");
}

uint32_t rd32(const std::string& s, size_t o) {
  return static_cast<uint8_t>(s[o]) | (static_cast<uint8_t>(s[o + 1]) << 8) | (static_cast<uint8_t>(s[o + 2]) << 16) |
         (static_cast<uint32_t>(static_cast<uint8_t>(s[o + 3])) << 24);
}
uint16_t rd16(const std::string& s, size_t o) {
  return static_cast<uint16_t>(static_cast<uint8_t>(s[o]) | (static_cast<uint8_t>(s[o + 1]) << 8));
}

TEST(EpubPackager, WritesValidStoredZip) {
  EpubPackager::ArticleMeta meta{"Tom & Jerry's <Guide>", "example.com", "urn:instapaper:bookmark:42", "en"};
  const std::string chapter = EpubPackager::chapterHeader(meta) + "<p>Hi</p>" + EpubPackager::chapterFooter();
  const uint32_t crc = EpubPackager::crc32Update(0, reinterpret_cast<const uint8_t*>(chapter.data()), chapter.size());

  std::string zip;
  size_t pos = 0;
  ASSERT_TRUE(EpubPackager::writeEpub(
      [&](const uint8_t* d, size_t n) {
        zip.append(reinterpret_cast<const char*>(d), n);
        return true;
      },
      meta, crc, static_cast<uint32_t>(chapter.size()),
      [&](uint8_t* buf, size_t len) {
        const size_t n = std::min(len, chapter.size() - pos);
        memcpy(buf, chapter.data() + pos, n);
        pos += n;
        return static_cast<int>(n);
      }));

  // mimetype first, stored, content right after a 30+8 byte header.
  ASSERT_EQ(rd32(zip, 0), 0x04034b50u);
  EXPECT_EQ(rd16(zip, 8), 0);  // stored
  EXPECT_EQ(zip.substr(30, 8), "mimetype");
  EXPECT_EQ(zip.substr(38, 20), "application/epub+zip");

  // End of central directory → central directory → every local header.
  const size_t eocd = zip.size() - 22;
  ASSERT_EQ(rd32(zip, eocd), 0x06054b50u);
  const uint16_t count = rd16(zip, eocd + 10);
  EXPECT_EQ(count, 5);
  size_t cd = rd32(zip, eocd + 16);
  std::vector<std::string> names;
  for (int i = 0; i < count; i++) {
    ASSERT_EQ(rd32(zip, cd), 0x02014b50u);
    const uint32_t entryCrc = rd32(zip, cd + 16);
    const uint32_t size = rd32(zip, cd + 24);
    const uint16_t nameLen = rd16(zip, cd + 28);
    const uint32_t local = rd32(zip, cd + 42);
    const std::string name = zip.substr(cd + 46, nameLen);
    names.push_back(name);
    ASSERT_EQ(rd32(zip, local), 0x04034b50u);
    EXPECT_EQ(zip.substr(local + 30, rd16(zip, local + 26)), name);
    const std::string data = zip.substr(local + 30 + nameLen, size);
    EXPECT_EQ(EpubPackager::crc32Update(0, reinterpret_cast<const uint8_t*>(data.data()), data.size()), entryCrc)
        << name;
    cd += 46 + nameLen;
  }
  EXPECT_EQ(names[0], "mimetype");
  EXPECT_EQ(names[4], "OEBPS/article.xhtml");
  EXPECT_NE(zip.find("<dc:title>Tom &amp; Jerry's &lt;Guide&gt;</dc:title>"), std::string::npos);

  // Leave a copy for manual inspection (unzip -t, epubcheck) when asked to.
  if (const char* path = getenv("INSTAPAPER_TEST_EPUB")) {
    if (FILE* f = fopen(path, "wb")) {
      fwrite(zip.data(), 1, zip.size(), f);
      fclose(f);
    }
  }
}

TEST(EpubPackager, FailsWhenSourceEndsEarly) {
  EpubPackager::ArticleMeta meta{"t", "", "id", "en"};
  EXPECT_FALSE(EpubPackager::writeEpub([](const uint8_t*, size_t) { return true; }, meta, 0, 100,
                                       [](uint8_t*, size_t) { return 0; }));
}

}  // namespace
