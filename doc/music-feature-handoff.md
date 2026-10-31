# QQ Music Feature Handoff

## 1. Purpose and Scope

This document is the handoff reference for the QQ Music feature in MicroTech. It
covers the complete design, current implementation status, verification evidence,
known boundaries, and the next work plan.

The feature is for personal learning use. It does not use a relay service and
does not use the QQ Music OpenAPI, because individual developers are not eligible
for that API. The device connects directly to QQ Music Web endpoints over HTTPS.

The implementation deliberately separates responsibilities:

- `layers/middleware/components/music_service/` owns QQ Music HTTP, JSON,
  WeChat QR login, credential persistence, image decode, lyric parsing, and the
  audio stream.
- `layers/apps/music_app/` is an LVGL application that consumes immutable
  `music_service` snapshots. It performs no network, JSON, image decode, or audio
  I/O.
- `main/app_runtime.c` owns service lifecycle, network/time propagation, factory
  reset cleanup, and system PM integration.
- `main/app_runtime_pm.c` treats active music playback as a standby blocker.
- `sim/` and the host test trees provide fakes, UI review scenarios, and source
  compilation coverage; they do not replace on-device validation.

## 2. Architecture

### 2.1 Layers and Ownership

| Layer | Owner | Responsibility |
| --- | --- | --- |
| App | `music_app` | Five pages: root, playlist, player, lyrics, login; renders snapshots and sends commands through the public API |
| Middleware | `music_service` | HTTPS/cookie handling, WeChat QR flow, catalog/lyric/cover acquisition, playback queue, audio decoder stream |
| Audio | `audio_service` | Exclusive PCM lease and BSP audio device ownership |
| Runtime | `main/app_runtime.c` | Init/deinit order, network-ready and time-ready propagation, factory-reset cleanup |
| PM | `main/app_runtime_pm.c` | Blocks standby admission and commit while music playback is active |
| Storage | `nv_storage` | `music_cred` credential blob; `music_service_reset_persisted_state()` erases it for factory reset |
| Events | `event_bus` | `MUSIC_SERVICE_MSG` notifications with `STATUS`, `HOME`, `PLAYLIST`, `LYRIC`, `IMAGE`, `PLAYER` subtypes |

The middleware public contract is
`layers/middleware/components/music_service/include/music_service.h`. The
middleware implementation is split into:

- `music_service.c`: singleton state, API worker, command queue, login flow,
  snapshot replacement, credential lifecycle.
- `music_net.c`: HTTP/HTTPS, cookie jar, JSON/JSONP, WeChat QR, catalog, song URL,
  lyric, JPEG cover decode.
- `music_stream.c`: CDN stream read, `esp_audio_simple_dec`, PCM writes through
  `audio_service`, stream worker lifecycle.
- `music_service_internal.h`: private command enum, credential blob, runtime
  state, and internal function declarations.

### 2.2 Service State Model

The public service states are:

```text
UNINITIALIZED
WAITING_NETWORK -> WAITING_TIME -> LOGGED_OUT
                                      |
                                      v
                                  QR_READY -> QR_SCANNED -> AUTHENTICATING
                                      |                         |
                                      +---- AUTH_REQUIRED <-----+
                                      |
                                      +---- READY
ERROR
```

- `music_service_set_network_ready(false)` enters `WAITING_NETWORK`.
- If the network is up but `time_service_get_quality()` is invalid, the service
  enters `WAITING_TIME`. The API worker wakes at least once per second and
  re-checks time quality, so SNTP completion does not require a new connectivity
  event.
- `music_service_login_begin()` is accepted only when network and time are ready.
- Both `MUSIC_LOGIN_TYPE_QQ` and `MUSIC_LOGIN_TYPE_WECHAT` are accepted; the
  login page exposes QQ and WeChat QR entries.
- A terminal QR failure enters `AUTH_REQUIRED`; a verified QR flow enters
  `READY` only after the credential is saved successfully.

### 2.3 Queue and Snapshot Ownership

Large objects are allocated in PSRAM:

- `music_home_snapshot_t`: up to 12 playlists per category, three categories.
- `music_playlist_snapshot_t`: up to `MUSIC_SERVICE_MAX_SONGS` songs (100).
- `music_lyric_snapshot_t`: up to `MUSIC_SERVICE_MAX_LYRIC_LINES` parsed lines
  (256).
- `music_image_t`: decoded RGB565 pixels plus source URL and token.
- Stream buffers: 4 KiB input and 16 KiB initial PCM, grown on demand up to
  256 KiB for decoder output frames.

The service runtime state (`s_music`), the 24-entry cookie jar (`s_cookies`),
and both command queues also live in PSRAM. The small `s_stream` runtime block
(72 B) stays in internal RAM. The API worker stack remains in internal RAM because
credential save/erase goes through NVS, which disables the flash cache; a PSRAM
stack would fault during that window. The audio worker runs from a PSRAM stack
and uses the `audio_service` exclusive playback lease.

`music_service_*_acquire()` transfers ownership of the snapshot to the caller;
`music_service_*_release()` frees it. A page must release every acquired
snapshot in `UNMOUNT`. The service never holds a snapshot that has been acquired
by a page.

The service publishes event-bus messages after replacing snapshots. The App pages
subscribe with `EVENT_BUS_DISPATCH_UI` and apply changes only in the UI worker.
The player page additionally polls player status while playing; other pages are
event-driven with a low-frequency status refresh.

### 2.4 Login and Credential Flow

WeChat QR flow:

1. The App calls `music_service_login_begin(MUSIC_LOGIN_TYPE_WECHAT)`.
2. The API worker requests
   `https://open.weixin.qq.com/connect/qrconnect` with the QQ Music WeChat app id
   and the QQ Music redirect URI.
3. The returned `uuid` is used to download the QR image from
   `https://open.weixin.qq.com/connect/qrcode/<uuid>`.
4. The API worker polls
   `https://lp.open.weixin.qq.com/connect/l/qrconnect?uuid=<uuid>` every 1.5 s.
5. `wx_errcode=404` means scanned; `405` carries `wx_code`; other non-408 codes
   are terminal failures.
6. A successful code is exchanged through
   `https://u.y.qq.com/cgi-bin/musicu.fcg` using module
   `music.login.LoginServer`, method `Login`.
7. The credential is written to NVS key `music_cred` with magic `MUS1` and version
   1. Only a successful save sets `credential_valid` and `READY`.

QQ QR flow:

1. The App calls `music_service_login_begin(MUSIC_LOGIN_TYPE_QQ)`.
2. The API worker obtains `pt_login_sig` from
   `https://xui.ptlogin2.qq.com/cgi-bin/xlogin`, then downloads the PNG QR from
   `https://ssl.ptlogin2.qq.com/ptqrshow`. `ptqrtoken` is `hash33(qrsig)`.
3. The API worker polls
   `https://ssl.ptlogin2.qq.com/ptqrlogin` every 1.5 s. `66` means waiting,
   `67` means scanned, `65` means expired, and `0` carries the `uin` and OAuth
   authorization URL.
4. The authorization URL is fetched with cookies; `p_skey` is read from the
   cookie jar. A form POST to `https://graph.qq.com/oauth2.0/authorize` uses
   `g_tk = hash33(p_skey, 5381)` and returns an OAuth `code`.
5. The code is exchanged through `https://u.y.qq.com/cgi-bin/musicu.fcg` using
   module `QQConnectLogin.LoginServer`, method `QQLogin`, and
   `comm.tmeLoginType=2`.
6. The credential is saved with `login_type=2`; the public status reports
   `MUSIC_LOGIN_TYPE_QQ`.

The credential blob contains `musicid`, `musickey`, `refresh_key`,
`refresh_token`, and `expires_at`. Startup validates the blob and rejects an
already-expired credential. Automatic token refresh is not implemented; expired
credentials require a new QR login.

`music_service_logout()` stops playback, erases the NVS credential, clears the
cookie jar, clears home/playlist/lyric snapshots, clears player metadata, and
publishes `STATUS`, `HOME`, and `PLAYER`.

### 2.5 Catalog and Playback Flow

Catalog endpoints currently used:

| Purpose | Endpoint |
| --- | --- |
| Anonymous recommendations | `music.playlist.PlaylistSquare/GetRecommendFeed` |
| User playlists | `music.musicasset.PlaylistBaseRead/GetPlaylistByUin` |
| Playlist detail | `music.srfDissInfo.DissInfo/CgiGetDiss` |
| Song URL | `vkey.GetVkeyServer/CgiGetVkey` |
| Lyric | `music.musichallSong.PlayLyricInfo/GetPlayLyricInfo` |
| Cover | `https://y.gtimg.cn/music/photo_new/T002R300x300M000<album_mid>.jpg` |

The current catalog responses use the newer song schema: `songlist[].mid`,
`file.media_mid`, `file.size_128mp3`, `file.size_320mp3`, and
`file.size_flac`; playlist metadata is under `dirinfo`. The parser accepts the
older `songmid`/`strMediaMid`/`size*` and `dissname` names as fallbacks.
`CgiGetVkey` requires `songmid` and `songtype` arrays; the filename is built
from `media_mid`. Lyrics are requested with `crypt: 0`, which returns
Base64-encoded plain LRC text.

`dirid == 201` identifies the QQ Music favorite bucket; other `GetPlaylistByUin`
entries are created playlists. The home snapshot therefore has separate
`my_created` and `my_favorite` arrays, and the root page renders both sections when
present.

Playback path:

1. `music_service_play_playlist(ref, start_index)` queues a PLAY command and marks
   the player as buffering.
2. The API worker fetches the playlist page containing `start_index`
   (`MUSIC_SERVICE_PLAYLIST_PAGE_SIZE = 30`), obtains the song URL, stops the old
   stream, and queues the URL to the audio worker.
3. `music_stream` opens the CDN connection, decodes with
   `esp_audio_simple_dec`, configures the BSP format, acquires
   `AUDIO_SERVICE_SESSION_PLAYBACK`, and writes PCM through
   `audio_service_write_leased()`.
4. A natural end-of-stream calls `music_service_stream_finished()`; the service
   advances according to the selected mode (sequential, repeat one, repeat all,
   shuffle) and stops at the end of a sequential queue.
5. `music_service_stop()` sets a generation-based stop, aborts the HTTP request,
   releases the audio lease, and publishes an idle player state.
6. `music_service_seek()` is a reserved API and currently returns
   `ESP_ERR_NOT_SUPPORTED`.

Audio fault policy:

- `music_stream_init()` registers the MP3 and FLAC decoder implementations before
  the audio worker opens `esp_audio_simple_dec`; the simple decoder wrapper does
  not register the underlying codec by itself.
- The stream task is stopped cooperatively with `shutdown/exited` atomics and an
  HTTP request cancellation. It is not deleted while it may own HTTP, decoder, or
  audio resources.
- A failed `audio_service_release()` keeps the lease handle for retry; the service
  does not overwrite it.
- Pause calls `audio_service_suspend()`; resume calls `audio_service_resume()`.
- `music_service_is_playing()` reads an atomic public state for the PM guard.

### 2.6 App Pages

Routes are defined in `layers/apps/music_app/src/music_app.c`:

| Page id | Page | Responsibility |
| --- | --- | --- |
| `root` | Music home | Personal/recommended playlists, login/logout, refresh |
| `playlist` | Playlist detail | Up to 100 songs from one playlist snapshot |
| `player` | Now playing | Cover, title, artist, progress, play/pause, previous/next, lyrics |
| `lyrics` | Lyrics | Up to 256 LRC lines, refresh on song change |
| `login` | Login | WeChat QR image and status |

All pages reset render/request caches in `MOUNT`, destroy LVGL objects in
`UNMOUNT`, and release every acquired snapshot or image. Image descriptors are
stored per page; there is no shared static `lv_image_dsc_t`.

## 3. Configuration and Public Contract

### 3.1 Product Configuration

`music_service_config_t` supplies:

- `api_task_priority`
- `audio_task_priority`
- `default_quality` (`MUSIC_QUALITY_STANDARD`, `HIGH`, `LOSSLESS`)

The product values live in `main/app_product_config.c`.

Kconfig supplies:

- `CONFIG_MUSIC_SERVICE_API_TASK_STACK_SIZE`
- `CONFIG_MUSIC_SERVICE_AUDIO_TASK_STACK_SIZE`
- `CONFIG_MUSIC_SERVICE_API_QUEUE_DEPTH`
- `CONFIG_MUSIC_SERVICE_MAX_RESPONSE_BYTES`

The API worker uses internal RAM for its stack. The audio worker uses a PSRAM
stack through `xTaskCreatePinnedToCoreWithCaps()`. Both workers are pinned to core
0; LVGL remains on the UI worker. The API stack stays internal because credential
save/erase calls `nv_storage_set_blob()` / `nv_storage_erase_key()`, and an NVS
commit disables the flash cache; a PSRAM stack would fault in that window.

### 3.2 Public API Groups

- Lifecycle: `music_service_init()`, `deinit()`, `set_network_ready()`.
- Login: `login_begin()`, `login_qr_acquire()`, `login_qr_release()`,
  `login_cancel()`, `logout()`, `reset_persisted_state()`.
- Catalog: `refresh_home()`, `home_acquire()/release()`,
  `playlist_acquire()/release()`, `lyric_acquire()/release()`,
  `image_acquire()/release()`.
- Playback: `play_playlist()`, `play_pause()`, `next()`, `prev()`, `set_mode()`,
  `stop()`, `get_player_status()`, `is_playing()`.
- Reserved: `seek()` returns `ESP_ERR_NOT_SUPPORTED`.

### 3.3 Runtime Integration

`main/app_runtime.c` initializes the service after `audio_service` and
`recorder_service`. Initialization failure is non-fatal: the rest of the device
continues without music. Connectivity status snapshots propagate IPv4 readiness;
the API worker handles time readiness internally and also refreshes home when it
becomes ready.

Factory reset calls `music_service_reset_persisted_state()` after NVS storage is
available. The service also survives page pause/screen off while playback owns the
exclusive audio lease; `app_runtime_pm.c` blocks standby while
`music_service_is_playing()` is true.

## 4. Current Status

### 4.1 Implemented

- Five-page `music_app` with launcher icon and App Manager registration.
- WeChat and QQ account QR login, QR display, scan/confirmation status,
  credential save, expiry check, logout cleanup, and factory-reset erasure.
- Player-page cover loading, replacement on track change, bounded retry, and
  PSRAM ownership release.
- Recommended playlists and logged-in user's created/favorite playlists.
- Playlist detail and a paged playback queue that can play beyond the first 30
  songs; the playlist page currently loads up to 100 songs.
- Song URL acquisition with quality fallback.
- MP3/FLAC stream decode, cover decode, and LRC parsing.
- Play/pause, previous/next, repeat modes, auto-advance, stop, and exclusive
  audio lease handling.
- Event-bus status/snapshot notifications and PM playback guard.
- Simulator UI review scenarios and golden baselines for all five music pages.
- Host fakes for main runtime, integration tests, and simulator.
- Service state, cookie jar, and command queues in PSRAM; the API stack remains
  internal because credential writes go through NVS.

### 4.2 Verification Evidence

The current memory change has been built and measured:

| Check | Result |
| --- | --- |
| `idf.py build` | PASS (0 warnings) |
| `microtech.bin` | `0x2bdc20`; `0x423e0` bytes free in the 3 MiB app partition |
| DIRAM | `219,074 / 341,760` bytes; `122,686` bytes free |
| External RAM `.bss` | `51,096` bytes; `s_music`, `s_cookies`, and queues are in PSRAM |
| `s_cookies` | `13,056 B` (24 entries) in PSRAM |
| Queue item sizes | `592 B` (API), `520 B` (stream); both queues are PSRAM-backed |
| Music internal footprint (calculated) | `≈10.3 KB` (was `≈28.5 KB`) |
| `music_api` stack | `10,240 B`, still internal by design |
| `music-service` host suite | 1 / 1 passed |

The existing functional suites remain green after the QQ/login and cover
changes:

| Check | Result |
| --- | --- |
| `main/tests/host` | 21 / 21 passed |
| `tests/integration` | 2 / 2 passed |
| `layers/apps/tests/host` | 15 / 15 passed |
| `layers/app_manager/app_core/tests/host` | 19 / 19 passed |
| `sim/ci/run_ci.sh build/sim` | SIM CI green |
| Music page review | 5 states, 0 failures |
| `tests/run_python_tests.py configuration resources` | Passed |

The stage commits are:

- Parent: `244cc1e feat(music): integrate QQ Music service and app`
- `layers/middleware`: `452b4d1 feat(music): add QQ Music service`
- `layers/apps`: `707c938 feat(apps): add QQ Music application`
- `layers/app_manager`: `79a28d6 feat(app_core): add music application identifier`

The memory change and the QQ login/cover/stream playback changes are committed
separately from those stage commits (middleware `43b65b1`, apps `aaa8561`,
bsp `1ea201e`).

### 4.3 Known Boundaries

1. QQ Music App MQTT/QPlay login is not implemented; QQ account QR login is
   implemented through the web ptlogin flow.
2. `music_service_seek()` is reserved and returns `ESP_ERR_NOT_SUPPORTED`.
3. Automatic credential refresh is not implemented; expired credentials force a
   new QR login.
4. The playlist page is bounded to 100 songs; the playback queue can page beyond
   that, but the page does not provide an explicit "load more" control.
5. Credentials are stored in a plain NVS blob. Flash encryption / NVS encryption
   is not enabled in the current `sdkconfig`.
6. A standalone `music-service` host suite covers the pure QQ login protocol
   parsers; network, TLS, NVS, and audio paths remain covered by firmware builds,
   simulator fakes, and on-device checks.
7. QQ Music Web endpoints are unofficial and may change without notice; their
   response fields and error codes must be re-verified when behavior changes.
8. CDN playback and ES8311 output have been verified on hardware; QQ account
   scan/login, cover rendering, and battery behavior have not.
9. The API worker stack remains in internal RAM because credential save/erase
   goes through NVS, which disables the flash cache. The feature's remaining
   internal footprint is therefore dominated by that 10,240 B stack; moving it
   requires isolating persistence work first.

## 5. Operational Notes

### 5.1 Build

Use the project component cache to avoid writing outside the workspace:

```sh
XDG_CACHE_HOME="$PWD/build/component-cache" idf.py build
```

The app partition is 3 MiB. The current binary leaves approximately 388 KiB free.

### 5.2 Relevant Tests

```sh
cmake -S main/tests/host -B /tmp/mt-main -G Ninja
cmake --build /tmp/mt-main
ctest --test-dir /tmp/mt-main --output-on-failure

cmake -S tests/integration -B /tmp/mt-integration -G Ninja
cmake --build /tmp/mt-integration
ctest --test-dir /tmp/mt-integration --output-on-failure

cmake -S layers/apps/tests/host -B /tmp/mt-apps -G Ninja
cmake --build /tmp/mt-apps
ctest --test-dir /tmp/mt-apps --output-on-failure

sim/ci/run_ci.sh build/sim
python3 sim/tools/review_pages.py \
  --pages music/root,music/login,music/player,music/lyrics,music/playlist \
  --build build/sim --check
```

### 5.3 Debugging Checklist

If music does not appear:

1. Check `audio_service_init()` succeeded; `music_service_init()` is skipped when
   audio is unavailable.
2. Check `music_service_set_network_ready(true)` was called after an IPv4 address
   was assigned.
3. Check `time_service_get_quality()` is not `TIME_SERVICE_QUALITY_INVALID`; the
   service enters `WAITING_TIME` until SNTP/RTC provides a trusted time.
4. Check NVS key `music_cred` is either absent or a valid `MUS1` blob.
5. Check the common TLS certificate bundle includes the QQ Music/WeChat roots.

If login fails:

1. Verify `open.weixin.qq.com` and `lp.open.weixin.qq.com` are reachable.
2. Check that QR UUID parsing succeeded and the QR image body is JPEG.
3. Check `wx_errcode` handling: 404 means scanned, 405 carries the code, 402 means
   expired.
4. Check that `music.login.LoginServer/Login` returned `musicid` and `musickey`.

If QQ login fails:

1. Verify `xui.ptlogin2.qq.com`, `ssl.ptlogin2.qq.com`, and `graph.qq.com` are
   reachable.
2. Check that `pt_login_sig` and `qrsig` cookies were captured; the QR body
   must start with `89 50 4E 47` (PNG).
3. Check that the authorization redirect set `p_skey` and that
   `graph.qq.com/oauth2.0/authorize` returned a `Location` with `code=`.
4. Check that `QQConnectLogin.LoginServer/QQLogin` returned `musicid` and
   `musickey`, and that the credential reports `login_type=2`.

If playback fails:

1. Verify `audio_service_acquire(PLAYBACK)` succeeded; another consumer may own
   the lease.
2. Check the CDN URL is HTTPS and returns audio, not an HTML error page.
3. Check the URL path extension before `?`; the decoder selects MP3/FLAC from
   that path.
4. Check the stream task has exited cleanly before starting another track; a
   failed `audio_service_release()` retains the lease for retry.

## 6. Next Work Plan

### M0: Memory Baseline

- Save `build/microtech.map`, `build/microtech.elf`, and
  `idf.py size --format json2` output before changing allocation domains.
- Record `nm` placement of `s_music`, `s_cookies`, and `s_stream`.
- Use the current build as the baseline; do not reuse the older
  `228,642 / 113,118` numbers.

### M1: Internal RAM Reduction (Implemented)

- `s_music` and `s_cookies` use `EXT_RAM_BSS_ATTR`; both now report PSRAM
  addresses.
- The API and stream command queues use `xQueueCreateWithCaps(..., PSRAM)` and
  `vQueueDeleteWithCaps()`.
- `music_service_init()` logs item size, internal allocation cost, current free
  bytes, and largest free block.
- `music_api` logs stack high-water after login and after playback starts.
- The API stack remains internal by design; see 2.3 and 3.1.

### M2: Verification and On-Device Acceptance

- Run the firmware build and compare DIRAM/PSRAM against M0.
- Flash only after explicit user approval; verify boot without panic.
- Verify network-ready and SNTP transitions; confirm `WAITING_TIME` leaves
  automatically after time sync.
- Scan the WeChat QR code and confirm the full login state sequence.
- Verify real home recommendations, created playlists, and favorite playlists.
- Play at least one MP3 and one lossless/FLAC track through ES8311; verify volume,
  pause/resume, stop, next, and auto-advance.
- Verify cover and lyric rendering for real songs.
- Verify the worst memory case: open cover/lyrics while a track is playing, so
  the API and stream TLS sessions overlap; no `ESP_ERR_NO_MEM` or cache fault.
- Verify screen-off playback and `system_pm` standby blocking/release.
- Verify logout and factory reset remove the credential and cached private data.
- Run repeated page mounts, login/logout cycles, and at least ten tracks while
  watching internal/PSRAM free bytes for leaks.
- Record `music_api` and `music_stream` stack high-water marks.

### M3: Follow-up Work

1. If the 10,240 B API stack has at least 4,096 B high-water after login and
   playback, reduce it to 8,192 B and require at least 2,048 B after the change.
   Otherwise keep 10,240 B and document why.
2. If internal RAM is still insufficient, isolate credential save/erase onto an
   internal-stack persistence worker, then move the API stack to PSRAM. This
   needs its own on-device cache-fault validation.
3. Add the standalone service host suite: queue create/delete caps and failure
   cleanup, cookie jar, LRC/URL/quality parsing, login transitions, and mocked
   HTTP 402/403/408, malformed JSON, empty playlists, and oversized covers.
4. Re-check task shutdown timing, `esp_http_client_cancel_request()`, pause/resume
   PCM boundaries, and the automatic credential-refresh decision.
5. Only after the memory budget is stable: load-more, seek, HTTP keep-alive,
   endpoint-specific response limits, image cache, and NVS/flash encryption.

### M4: QQ QR Login and Player Cover (Implemented)

- Added the QQ ptlogin2 flow: `xlogin` -> `ptqrshow` (PNG) -> `ptqrlogin` ->
  `p_skey` -> Graph OAuth `code` -> `QQConnectLogin.LoginServer/QQLogin`.
- The `xlogin` page is about 285 KiB and is fetched in header-only mode; only
  its cookies are used.
- Added the 24-entry cookie jar, form-encoded Graph authorization, and PNG
  RGB565 decoding through `espressif/libpng`.
- Added QQ/WeChat entries to the login page and mapped `login_type=2` to
  `MUSIC_LOGIN_TYPE_QQ`.
- Added the player cover retry state machine, IMAGE failure isolation, and
  aspect-fit LVGL scaling.
- Added `music-service` host coverage for hash33, ptuiCB parsing, cookie
  parsing, PNG signature, query extraction, and form encoding.

On-device playback has been verified. Remaining for this stage: QQ scan/login,
cover rendering, reboot, logout/factory-reset, and memory-leak acceptance.

## 7. File Map

```text
layers/middleware/components/music_service/
  include/music_service.h
  src/music_service.c
  src/music_net.c
  src/music_login_protocol.c
  src/music_login_protocol.h
  src/music_stream.c
  src/music_service_internal.h
  tests/host/CMakeLists.txt
  tests/host/music_login_protocol_test.c
  Kconfig
  CMakeLists.txt
  idf_component.yml
  README.md
  README_ZH.md

layers/apps/music_app/
  src/music_app.c
  src/music_app_internal.h
  src/music_ui.c
  src/music_root_page.c
  src/music_playlist_page.c
  src/music_player_page.c
  src/music_lyrics_page.c
  src/music_login_page.c
  assets/music_app.svg
  resource_manifest.cmake

main/
  app_runtime.c
  app_runtime_pm.c
  app_product_config.c
  app_product_config.h
  CMakeLists.txt

sim/
  ports/fakes/sim_fakes_services.c
  ci/review/spec.json
  ci/golden/ui/music__*.png

tests/
  main/tests/host/music_service_fake.c
  integration/host/music_service_fake.c
```

## 8. Change Discipline

- Keep `music_service` as the sole owner of QQ Music network and audio I/O.
- Do not add HTTP, JSON, image decode, or audio calls to `music_app`.
- Do not hold a service snapshot after releasing it; do not acquire a second
  snapshot of the same type without releasing the first.
- Never clear `lease_held` or `play_valid` without completing the corresponding
  cleanup/queue transition.
- Keep `music_service_seek()` unsupported until a range-seek implementation and
  on-device validation exist.
- Update `README.md` and `README_ZH.md` in the same change when behavior changes.
