# Instapaper (fork notes, phase 1)

Settings → Instapaper downloads articles from your Instapaper account and
converts each one into a small EPUB in `/Instapaper/` on the SD card, where it
opens like any other book.

## Setup

1. **Get an API key.** Instapaper's Full API needs an OAuth consumer key and
   secret issued to you: request one at <https://www.instapaper.com/developers>.
   Instapaper has at times limited bookmark access through the Full API to paid
   (Premium) accounts; if signing in works but the lists fail, that is the
   likely reason.
2. **Put the key on the SD card.** Create a file named `instapaper.json` in the
   SD card root:
   ```json
   {"consumer_key": "YOUR_KEY", "consumer_secret": "YOUR_SECRET"}
   ```
   Opening Settings → Instapaper imports it into `/.crosspoint/instapaper.json`
   (the secret obfuscated, like the other stored credentials) and deletes the
   plain copy.
3. **Sign in.** Settings → Instapaper → Account, then your Instapaper email and
   password. Only the OAuth token Instapaper returns is stored, never the
   password. Sign out removes the token.

## Using it

Unread, Starred and Archive show your 40 most recent articles in each (site and
reading progress underneath). Choosing one downloads Instapaper's text version,
converts it, and offers **Open** (Confirm, or tap) to start reading. Files are
named `Title [bookmark id].epub`.

## What phase 1 does not do yet

- Images are dropped; articles are text only.
- No paging beyond 40 articles per list, and no custom folders.
- Nothing is sent back to Instapaper: reading progress, archiving and starring
  stay on the device.

## How it works

- `lib/InstapaperSync/OAuth1.*`, `HmacSha1.*`: OAuth 1.0a request signing (xAuth
  sign-in, every API call). Signatures need the right time, so the clock is set
  over NTP if it is not already.
- `InstapaperClient.*`: `oauth/access_token`, `account/verify_credentials`,
  `bookmarks/list`, `bookmarks/get_text`. Large responses stream to the SD card.
- `ArticleSanitizer.*`: streaming HTML → well-formed XHTML (CrossPoint's chapter
  parser is strict XML). Keeps paragraphs, headings, lists, quotes, pre/code and
  emphasis; decodes entities; repairs bad nesting and invalid UTF-8.
- `EpubPackager.*`, `ArticleEpubBuilder.*`: writes an EPUB 2 (stored ZIP,
  OPF + NCX) without holding the article in RAM.
- `test/instapaper_sync/`: host tests (HMAC and OAuth reference vectors,
  sanitizer cases plus 400 random tag-soup inputs checked for well-formed
  output at any chunking, ZIP structure and CRCs). Generated EPUBs from real
  pages (Wikipedia, Project Gutenberg, blogs) pass epubcheck with no errors.
- Hooks into upstream files: a Settings menu entry, `INSTAPAPER_STORE.loadFromFile()`
  in `main.cpp`, and `STR_IP_*` strings at the end of `english.yaml`.
