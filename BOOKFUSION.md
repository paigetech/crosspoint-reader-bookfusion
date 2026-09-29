# BookFusion Sync (fork notes)

This branch is CrossPoint Reader with BookFusion account linking, reading-progress
sync, and library browsing/download added. The feature is a port of
[vbbot's BookFusion fork](https://github.com/vbbot/crosspoint-reader) (`v1.5.0-bookfusion`)
onto upstream CrossPoint 1.6.5, which is the first line with Xteink X4 Pro support.

## Builds

| Device | PlatformIO env | Output |
|---|---|---|
| Xteink X4 Pro (ESP32-S3) | `x4pro` | `.pio/build/x4pro/firmware.bin` |
| Xteink X3 / X4 (ESP32-C3) | `default` | `.pio/build/default/firmware.bin` |

Never flash the C3 image on an X4 Pro, or the reverse.

The X4 Pro has no USB-C, so flash through the pogo-pin adapter:
<https://crosspointreader.com/#flash-tools> → X4 Pro → **Custom .bin**. To recover,
flash the official x4pro firmware from the same page.

## Where the code lives

Almost everything is in fork-only files, to keep upstream rebases cheap:

- `lib/BookFusionSync/`: API client, token store, per-book id sidecars
- `src/activities/settings/BookFusion*`: Settings → BookFusion Sync (link/unlink), library browser and download
- `src/activities/reader/BookFusionSyncActivity.*`: in-reader progress sync

Hooks into upstream files are kept to a few lines each:

- `src/activities/settings/SettingsActivity.{h,cpp}`: menu entry
- `src/main.cpp`: load the token at boot
- `src/activities/reader/EpubReaderActivity.cpp`: route *Sync progress* to BookFusion for BookFusion books
- `lib/I18n/translations/english.yaml`: `STR_BF_*` strings, appended at the end
- `src/network/HttpDownloader.cpp`: report progress without a Content-Length (byte counter, cancel)
- `src/network/OtaUpdater.cpp`: update check points at this fork

## Things to know

- **OAuth client id.** Login uses BookFusion's OAuth device-code flow with the
  client id `"koreader"` (`BookFusionSyncClient::CLIENT_ID`), the same public id
  BookFusion's official KOReader plugin uses. There is no secret involved. It is
  a single constant, so it is easy to swap out if BookFusion ever restricts it or issues this
  fork its own id.
- **Token storage.** The access token is stored on the SD card at
  `/.crosspoint/bookfusion.json`. It is XOR-obfuscated with the device MAC (not
  encrypted), the same way the KOReader credentials are stored.
- **TLS.** Like the rest of CrossPoint's network code, the client does not verify
  server certificates (`setInsecure()`).
- **Which books sync.** Only books downloaded through the BookFusion library browser
  can sync, because the download writes a `/.crosspoint/bookfusion_<md5>.json`
  sidecar holding the BookFusion book id. For any other book, *Sync progress* falls
  through to KOReader sync as before.
- **Updates.** *Check for updates* looks at this fork's GitHub releases
  (`paigetech/crosspoint-reader-bookfusion`), not upstream's, so it can never replace
  this firmware with one that lacks BookFusion. Fork versions carry a `-bf<N>` suffix
  (e.g. `1.6.5-bf2`); the updater compares that build number after the upstream
  version, so `1.6.5-bf2` is offered over `1.6.5-bf1`, and any `-bf` release over a
  plain `1.6.5` or local dev build.

## Releasing an update

CI (`.github/workflows/ci.yml`) builds every device on each pull request and on
pushes to `develop`; firmware for a PR is attached to its CI run as artifacts.

To publish an over-the-air update:

1. Bump `version` in `platformio.ini` (e.g. `1.6.5-bf1` to `1.6.5-bf2`) and merge it into `develop`.
2. On GitHub: **Releases → Draft a new release**. Create tag `1.6.5-bf2` (exactly the
   `platformio.ini` version, no `v`) targeting `develop`, leave *pre-release* unticked,
   and **Publish release**.
3. Upstream's `.github/workflows/release.yml` builds every device and attaches
   `crosspoint-1.6.5-bf2-<device>.bin` (`x4pro`, `x3-x4`, …) to the release. It fails
   if the tag and `platformio.ini` disagree. Wait for the "Compile Release" run to finish.
4. On the reader: Settings → Check for updates.

When rebasing onto a new upstream release (say 1.7.0), restart the counter at `1.7.0-bf1`.
