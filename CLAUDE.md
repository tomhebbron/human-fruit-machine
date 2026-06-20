# Human Fruit Machine — project notes for Claude

A standalone HTML/CSS/JS fruit-machine game for Menston Primary School's summer
fete, designed to run on an iPhone/iPad mounted in a wooden cabinet. A child
inserts a £1 coin (or a games-master credits them on a wristband), pulls a
physical lever, and three reels spin to reveal a result. The cabinet is driven
by an ESP32 over BLE HID keyboard input (lever potentiometer + coin sensor +
easter-egg buttons + WS2812B LED strips); the same page also runs standalone
in any browser with keyboard controls for testing.

This file exists so future Claude sessions don't re-litigate the long, hard
debugging history of this project — especially the iOS Safari audio saga.
**Read this BEFORE touching audio.**

---

## Tech stack

* **One file** for the entire web app: `index.html`. Inline CSS + inline JS.
  No build step, no bundler, no npm. Open in a browser and it runs.
* **Deployment**: GitHub Pages, from branch `claude/wonderful-dirac-chbll2`,
  configured via `.github/workflows/deploy-pages.yml`. Every push to that
  branch triggers a deploy. There is **no preview environment** — push to
  the branch IS publish to production.
* **Persistence**: `localStorage` for settings + play history,
  `IndexedDB` (`hfm_sfx` database) for custom-uploaded SFX bytes.
* **Hardware**: ESP32 firmware in `lever/` (BLE keyboard) and
  `lever_wifi/` (WiFi WebSocket). Both talk to the same web page; the
  page auto-detects which transport (WebSocket only fires when the page
  is loaded over `http:`, i.e. served from the ESP32 itself).
* **Audio**: HTML5 `<audio>` elements throughout. See the audio section
  below for the long, painful history of why Web Audio was abandoned.

## Repo layout

```
index.html              # the entire web app
sounds/                 # bundled MP3 SFX (see "Audio mapping" below)
sounds/README.md        # file-by-file mapping
lever/lever.ino         # ESP32 BLE-keyboard firmware
lever_wifi/             # ESP32 WiFi+WebSocket firmware variant
.github/workflows/      # GitHub Pages deploy
```

## Working in this repo

* **Branch**: always commit to `claude/wonderful-dirac-chbll2` —
  it's the one wired to GitHub Pages.
* **Version string**: bump `const VERSION` in `index.html` on every
  user-visible change. It's displayed at the bottom of the admin panel
  so the user can confirm "did my push actually deploy?" without
  guessing about cache. Format: `YYYY-MM-DD.N`.
* **No comments unless WHY is non-obvious.** The code is dense; only
  comment workarounds (especially iOS audio quirks) and surprising
  invariants.
* **One file means search is cheap.** Grep `index.html` rather than
  refactoring into modules. Splitting it would require a build step,
  which would break the "drop into GitHub Pages and it works" property.
* **No emoji in commit messages or files unless the user asked.**
  (Plenty in the UI though.)

---

## Audio: the long, painful story — READ BEFORE TOUCHING

### What works today

All gameplay sound goes through HTML5 `<audio>` elements. Some sounds are
bundled MP3s (in `sounds/`), some are synth-rendered via
`OfflineAudioContext` → WAV → `Blob` → `<audio>`. **Nothing in the gameplay
path plays through the realtime Web Audio graph** — that's been proven
unreliable on iOS. The realtime `AudioContext` is kept around solely for
the "Web Audio tone" diagnostic button in the admin panel.

### Why Web Audio was abandoned

Over multiple sessions we tried every documented iOS audio-unlock pattern:

* Silent-buffer prime in a user gesture (the classic Mozilla trick).
* Dummy `AudioContext` created before any `await` to preserve the
  trusted-gesture flag.
* `visibilitychange` → `resume()` for FaceTime / lock-screen interrupts.
* Synchronous `close()` + recreate (`recoverAudio()`) to escape the
  `'interrupted'` state.
* Explicit `<audio>` element kept playing silent WAV in the background
  to hold the iOS audio session open.

**Final diagnosis**: even with `AudioContext.state === 'running'` and the
clock advancing (`ac.currentTime > 5`), iOS Safari plays Web Audio
output through a path that the ring/silent switch silences. After a
FaceTime call (or with the silent switch on) the entire Web Audio graph
is muted at the OS level. HTML5 `<audio>` uses the **media path**,
which:

* overrides the silent switch;
* survives call interruptions automatically;
* respects element-level `.volume` (and the system volume buttons).

Console logs proved beyond doubt: `[FM Audio] t=5.493 state=running`
firing a `BufferSourceNode`, **zero sound**. The same gesture playing an
HTML5 `<audio>` produced sound immediately.

If you find yourself thinking "but if I just unlock the context one more
way…" — you won't. That door is closed. Use HTML5 `<audio>`.

### The audio architecture (in `index.html`)

* `makeWavUrl(freq, seconds, volume)` — build a tiny WAV blob URL from
  PCM. Used for the unlock helper and the diagnostic beep.
* `audioBufferToWavUrl(buf)` — turn an `AudioBuffer` into a 16-bit PCM
  WAV blob URL. The bridge from offline-rendered synth to HTML5
  playback.
* `bakeSound(key, durationSec, renderFn)` — render a synth closure
  through `OfflineAudioContext` once, cache the WAV URL in
  `_wavCache[key]`. Pure computation, so unaffected by mute switch.
* `_pool` — 8 pre-unlocked `<audio>` elements for overlapping playback.
  Unlocked on first user gesture by playing then pausing a silent WAV.
* `playUrl(url, vol)` — grab a free pool element, set `src`, play.
* `playBaked(key, dur, vol, renderFn)` — bake-if-needed then `playUrl`.
* `playBundled(key, vol)` — play a static file from `_BUNDLED_FILES`
  via the pool. Used for sounds we know exist (deployed in `sounds/`).
* `bundledEl(key)` / `playBundledEl(key, vol)` — dedicated preloaded
  element for files that might 404 (e.g. user-supplied uploads). The
  element's `error` event flips `_bFailed[key]` so callers fall back.
* `_loseAudio` / `getLoseAudio()` / `playLoseSound(vol)` — dedicated
  HTMLAudioElement for `sounds/lose.mp3` (the sad-trombone loser
  sound).
* `_spinEl` / `getSpinEl()` — dedicated `<audio>` for the spin clip
  (Jägerhaus). Played **once** (not looped) because the clip contains
  its own built-in reel-stop sounds; looping would re-trigger them at
  the wrong moment.
* `playCustom(key, vol)` — user-uploaded SFX (admin → SFX tab). Stored
  as raw bytes in IndexedDB so they survive reloads. Played via blob
  URL through the pool. **Do not** route these through
  `decodeAudioData()` — that was the Web Audio path and is dead.

### Priority order for every sound

1. **Custom upload** (`rawSfx[key]` set by the admin SFX tab) — operator
   override always wins.
2. **Bundled file** (`_BUNDLED_FILES[key]` in `sounds/`) — the curated
   default.
3. **Baked synth** (`playBaked`) — fallback that never fails.

### iOS unlock invariants — DO NOT BREAK

* `_startAudio()` runs on the FIRST `touchstart`, `click`, or `keydown`
  on the document, then unregisters itself.
* iOS 17 only treats `touchstart` / `click` / `keydown` as **trusted**
  events for audio unlock. `pointerdown` is NOT trusted on iOS 17 —
  that's why we listen on `touchstart`, not `pointerdown`.
* `unlockPool()` plays a silent WAV (muted) on every pool element.
  After that, timer-driven plays work because each element has been
  "touched" inside a real gesture.
* Dedicated elements (`_loseAudio`, `_spinEl`, bundled-egg elements)
  must also be primed in `_startAudio()` for the same reason.
* The mobile coin/lever handlers also call `unlockPool()` defensively —
  it's idempotent and cheap. **Keep these calls.**

### Sound files (`sounds/`)

See `sounds/README.md` for the file-by-file mapping. Summary:

| Game event | Default source | Synth fallback? |
|------------|----------------|-----------------|
| Coin insert | `freesound_community-cash-register-purchase-87313.mp3` | yes |
| Ahem (no coin) | synth only (no bundled file) | yes |
| Reel spin | `SlotMachine Jägerhuus.mp3` (plays once — has stop sounds) | yes (looping click-bed) |
| Lever clunk | **none** — included in spin clip | — |
| Per-reel ching | **none** — included in spin clip | — |
| Pair win | `floraphonic-you-win-sequence-1-…mp3` | yes |
| Triple win | `floraphonic-you-win-sequence-2-…mp3` | yes |
| Jackpot | `Jackpot-Millionaire.mp3` | yes |
| Loser spin | `lose.mp3` (sad trombone) | yes (wah-wah) |
| Secret button ⭐ | `mlg-airhorn.mp3` | yes (synth arpeggio) |
| Secret button 👥 | `muttley-wheeze-or-laugh.mp3` (upload to enable) | yes (crowd ooh) |

**Reel-stop timing**: `cfg.spinDuration` (default 1600ms) + `cfg.reelDelay`
(default 650ms) are tuned so the three reels lock within the Jägerhaus
clip's ~3.2s runtime, letting its built-in stop clunks line up with the
reels. If the user wants to swap in a different spin clip, retune these in
admin → Settings → Timing by ear.

---

## Game flow & state machine

```
(coin mode)    nocoin → ready → pulling → spinning → result → nocoin
(credit mode)           ready → pulling → spinning → result → ready
```

`cfg.requireCoin` (admin → Settings → Game Mode) toggles between the two.
Default is `true` (coin mode — classic flow). **Credit mode** is for the
school fete with wristbands: a games-master stamps a wristband for X
goes, and the player just pulls the lever each time — no coin step.

`idleState()` returns the resting state for the current mode. The save
handler calls `applyCoinMode()` (toggles `body.no-coin` class) and resets
state to `idleState()` if currently idle, so the toggle takes effect
mid-game without a reload.

## Controls

The web page can be driven by:

* **Keyboard** (laptop testing): `C` coin, `1-5` lever level, `Space` /
  `Enter` spin, `A` ahem, `Q`/`W`/`E` secret buttons.
* **BLE HID keyboard** (ESP32 in `lever/`): same keystrokes, sent by
  the firmware in response to physical inputs.
* **WiFi WebSocket** (ESP32 in `lever_wifi/`): structured JSON
  messages on `ws://<esp32-ip>/ws` — see `connectEsp32Ws()` in
  `index.html`. Auto-enabled only when the page is loaded over `http:`
  (i.e. from the ESP32's own webserver). On GitHub Pages this is a
  no-op.
* **Mobile touch UI**: bottom bar with coin button (hidden in credit
  mode), draggable lever, and three secret buttons.

In credit mode the ESP32 still sends its physical `coin` event, but the
page ignores it. The `release` event also bypasses the coin gate — every
lever release counts as a spin trigger.

## Settings (admin panel)

All settings live in `cfg` (loaded from `localStorage` key `hfm_cfg`,
saved by `saveCfg()`). The admin form mirrors these into form fields in
`syncSettingsUI()` and reads them back in the `save-settings` handler.
**Add a default in `DEFAULTS`, sync in `syncSettingsUI`, read in the
save handler, and use everywhere via `cfg.X`** — and add a corresponding
field to the form HTML.

## Easter eggs

Three secret buttons (`Q`/`W`/`E` on keyboard, `⭐`/`🎉`/`👥` in mobile
bar):

* `eggParty` — airhorn + party arpeggio + reels do a colour dance.
* `eggForceJackpot` — silent toggle; next spin is guaranteed a jackpot.
  The game title glows gold while armed.
* `eggCrowd` — Muttley laugh (if uploaded) or crowd "ooooh".

---

## History of major changes (most recent first)

* `v2026-06-20.14` — Credit mode (no-coin toggle for fete wristbands).
  Spin sound = Jägerhaus once-through (provides its own stops);
  removed per-reel ching + lever clunk. Split win sound into pair vs.
  triple vs. jackpot. Secret buttons mapped to airhorn / Muttley.
* `v2026-06-20.13` — Wired all 10 uploaded MP3s into the game
  (cash-register coin, airhorn ahem, win/jackpot fanfares, etc.).
* `v2026-06-20.12` — **Big one**: migrated entire game audio engine
  from realtime Web Audio to offline-rendered WAV played through
  HTML5 `<audio>` elements. Fixed iOS silent-switch + post-FaceTime
  silence permanently. Web Audio kept only for a diagnostic tone.
* `lose.mp3` (sad trombone) added as bundled loser sound.
* Pre-`.12`: many failed attempts at iOS Web Audio fixes — see
  commit log for the depressing tour. Don't repeat them.
* Admin button changed to long-press on title (1.5s) to avoid an
  always-visible cog cluttering kid-facing UI.
* Version string added to admin panel so we can verify a deploy.
* Tabs in admin made horizontally scrollable for iPhone width.

## Common pitfalls

* **"The sound stopped working" on iOS** — it is the ring/silent
  switch, or audio routed to AirPods, or the page lost its trusted
  gesture after a FaceTime interruption. The admin → Stats panel has
  an Audio Diagnostics block with the right test buttons (🎰 / 😬 /
  🔔 / 🔊 / ↺ Recover audio). The first thing to check is the
  physical switch on the side of the phone.
* **"My new file isn't loading"** — GitHub Pages has a CDN cache.
  After a push, wait ~1 min, then hard-reload (Cmd+Shift+R on
  desktop; on iOS, close the tab and reopen). The version string
  in admin confirms which build is live.
* **"Custom SFX disappeared"** — they're in IndexedDB per browser
  per device. Clearing site data or switching browser wipes them.
  Operators can re-upload from the admin SFX tab.
* **"WebSocket isn't connecting"** — `ESP32_WS_URL` is only set when
  the page is on `http:` (served from the ESP32). On GitHub Pages
  (`https:`) it stays `null` and the page just runs from keyboard
  input. To test with the ESP32, load the page from `http://<esp32-ip>/`.

## Hardware

See header comments in `lever/lever.ino` and `lever_wifi/lever_wifi.ino`
for pinouts. Both firmwares need batteries, a buck converter for the
LED strips (50 LEDs at full white ≈ 3 A), and the same potentiometer +
coin sensor wiring.

The cabinet uses an iPad mounted behind a cutout, displaying this page
in full-screen Safari. The "Add to Home Screen" PWA mode is what the
`apple-mobile-web-app-capable` meta tag is for.
