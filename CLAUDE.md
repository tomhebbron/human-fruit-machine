# Human Fruit Machine — project notes for Claude

A standalone HTML/CSS/JS fruit-machine game for Menston Primary School's summer
fete, running on an iPad mounted in a wooden booth. A child presses a big
physical BUTTON (the lever was retired in v.21) and three reels spin; the iPad
is the VOLUNTEER display — three helpers in the booth hold up the matching
fruit props. The booth is run by an ESP32 (`button_wifi/` firmware): it is the
WiFi access point, serves this page + sounds from LittleFS, reads the button /
coin sensor / easter-egg buttons, and drives WS2812B LED strips from real game
events over a WebSocket. The same page also runs standalone in any browser
with keyboard controls for testing.

**Deploy flow (self-sync)**: develop and test against GitHub Pages as always;
the ESP32 downloads `index.html` + the sounds listed in `sync-manifest.txt`
from Pages at boot whenever it can reach the home WiFi configured in
`button_wifi.ino`. So: git push → power-cycle the ESP32 at home → the booth
carries the new build. At the fete (no home WiFi) it boots straight into AP
mode after ~10 s.

**Origin split gotcha**: localStorage/IndexedDB are per-origin. Settings tuned
on the Pages URL do NOT exist at `http://192.168.4.1` — use Admin → Settings →
Transfer Settings (copy/paste JSON) to move them, or bake final values into
`DEFAULTS` before fete day.

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
* **Hardware**: current ESP32 firmware is `button_wifi/` (WiFi AP +
  WebSocket + LittleFS hosting + self-sync). `lever/` (BLE) and
  `lever_wifi/` are superseded lever-era variants kept for reference.
  The WebSocket transport only activates when the page is loaded over
  `http:` (i.e. served from the ESP32 itself); on GitHub Pages the page
  runs from keyboard input only.
* **Audio**: HTML5 `<audio>` elements throughout. See the audio section
  below for the long, painful history of why Web Audio was abandoned.

## Repo layout

```
index.html              # the entire web app
sounds/                 # bundled MP3 SFX (see "Audio mapping" below)
sounds/README.md        # file-by-file mapping
sync-manifest.txt       # files the ESP32 self-sync downloads from Pages
button_wifi/            # CURRENT ESP32 firmware (button + AP + sync + LEDs)
lever/lever.ino         # superseded BLE-keyboard firmware (lever era)
lever_wifi/             # superseded WiFi firmware (lever era) — do not flash
.github/workflows/      # GitHub Pages deploy
REVIEW.md               # 2026-07 project review: findings + priorities
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
* `bundledEl(key)` / `playBundledEl(key, vol)` — dedicated preloaded
  element per bundled file. The element's `error` event flips
  `_bFailed[key]` so callers fall back to the synth. `playBundled` is
  now just a delegate to `playBundledEl` — every bundled sound goes
  through the 404-aware path because an ESP32-hosted page with a failed
  sound sync would otherwise play silence.
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
| Reel spin | `slot-machine-spin.mp3` (plays once — has stop sounds) | yes (looping click-bed) |
| Lever clunk | **none** — included in spin clip | — |
| Per-reel ching | **none** — included in spin clip | — |
| Pair win | `floraphonic-…-bonus-1-183918.mp3` | yes |
| Triple win | `floraphonic-…-bonus-2-183919.mp3` | yes |
| Jackpot | `floraphonic-…-jackpot-3-183921.mp3` | yes |
| Loser spin | `lose.mp3` (sad trombone) | yes (wah-wah) |
| Secret button ⭐ | `mlg-airhorn.mp3` | yes (synth arpeggio) |
| Secret button 👥 | `muttley-wheeze-or-laugh.mp3` | yes (crowd ooh) |

**Reel-stop timing**: `cfg.reelStops` (defaults 1890/2890/3890 ms, absolute
from spin start) and `cfg.spinEnd` (5500 ms, when the fanfare fires) are
tuned so the reels lock on the Jägerhaus clip's built-in stop clunks. If the
user swaps in a different spin clip, retune in admin → Settings → Timing by
ear. Any newly wired bundled file must ALSO be added to `sync-manifest.txt`
or the ESP32-hosted page won't have it.

---

## Game flow & state machine

```
(coin mode)    nocoin → ready → spinning → result → nocoin
(credit mode)           ready → spinning → result → ready
```

(`pulling` still exists in code for the legacy BLE lever path but nothing
current enters it.)

`cfg.requireCoin` (admin → Settings → Game Mode) toggles between the two.
Default is `false` (credit mode — fete flow). **Credit mode** is for the
school fete with wristbands: a games-master stamps a wristband for X
goes, and the player just presses the button each time — no coin step.

`idleState()` returns the resting state for the current mode. The save
handler calls `applyCoinMode()` (toggles `body.no-coin` class) and resets
state to `idleState()` if currently idle, so the toggle takes effect
mid-game without a reload.

## Controls

The web page can be driven by:

* **Keyboard** (laptop testing): `C` coin, `Space` / `Enter` spin (or
  reset after a result), `A` ahem, `Q`/`W`/`E` secret buttons. (`1-5`
  legacy lever levels still parse but nothing sends them.)
* **WiFi WebSocket** (ESP32 in `button_wifi/`): JSON on
  `ws://<esp32-ip>/ws` — `{"t":"button"}` for the spin button (the page
  applies the coin gate: `nocoin` state → ahem, otherwise spin/reset),
  `{"t":"coin"}`, `{"t":"egg","n":0-2}`. Auto-enabled only when the page
  is loaded over `http:` (served from the ESP32). On GitHub Pages this
  is a no-op. The page sends back `spin_start` / `reel` / `result`
  (with `m` = matched reel indices, so a pair lights exactly those two
  reel-window borders) / `state` (drives the big red button's lamp).
* **Mobile touch UI**: on-screen spin button (single `click` handler —
  do NOT add a `pointerdown` handler, one tap would fire both) and the
  three secret buttons in the bottom bar.

In credit mode the ESP32 still sends its physical `coin` event, but the
page ignores it.

**Start overlay**: `#start-overlay` covers the page until the first
trusted gesture (`_startAudio`) removes it. It exists because WebSocket
events are not trusted gestures — without one deliberate tap after every
page load, iOS keeps all audio locked and the booth runs silently.

## Settings (admin panel)

All settings live in `cfg` (loaded from `localStorage` key `hfm_cfg`,
saved by `saveCfg()`). The admin form mirrors these into form fields in
`syncSettingsUI()` and reads them back in the `save-settings` handler.
**Add a default in `DEFAULTS`, sync in `syncSettingsUI`, read in the
save handler, and use everywhere via `cfg.X`** — and add a corresponding
field to the form HTML.

### Fruit & jackpot (configurable per event)

`FRUITS` is the fixed *catalog* of all symbols (8: cherry, lemon, orange,
apple, banana, strawberry, grapes, star). What's actually **in play** is
`cfg.fruits` (array of ids) — the operator ticks what they've got as prizes
that day (admin → Settings → Fruit in play; need ≥3). `activeFruits()`
returns the enabled subset and `weightedRandom(pool)` draws from it.

The **jackpot is decoupled** from the normal three-of-a-kind:

* `cfg.jackpotFruit` — three of THIS symbol = jackpot (default `star`, but
  can be any fruit, e.g. three lemons).
* `cfg.jackpotChance` — its own independent probability (UI: "1 in N spins",
  default 1/500). So the jackpot rate is exactly `jackpotChance` **regardless**
  of how common the jackpot fruit is elsewhere.
* `spinReels()` rolls jackpot first; the natural three-of-a-kind branch then
  *excludes* the jackpot fruit, so three-jackpot-fruit **never** lands by
  chance — only on a real jackpot roll. `evaluate()` keys the jackpot off
  `cfg.jackpotFruit`, not a hardcoded star.
* The jackpot fruit still whizzes past during the spin (`reelVisualPool()`
  adds it to the blur even if it isn't a normal in-play fruit).

If you add a hardcoded `'star'` check anywhere you've reintroduced a bug —
always go through `cfg.jackpotFruit` / `jackpotFruit()`.

## Easter eggs

Three secret buttons (`Q`/`W`/`E` on keyboard, `⭐`/`🎉`/`👥` in mobile
bar):

* `eggParty` — airhorn + party arpeggio + reels do a colour dance.
* `eggForceJackpot` — silent toggle; next spin is guaranteed a jackpot.
  The game title glows gold while armed.
* `eggCrowd` — Muttley laugh (if uploaded) or crowd "ooooh".

---

## History of major changes (most recent first)

* `v2026-07-10.1` — Configurable fruit set (`cfg.fruits`) + configurable
  jackpot symbol/rate (`cfg.jackpotFruit`, `cfg.jackpotChance`, decoupled
  from three-of-a-kind). See "Fruit & jackpot" under Settings.
* `v2026-07-03.3` — Volunteer display mode (`cfg.volunteerMode`, or
  `?volunteer=1` URL override): full-screen black overlay with each
  helper's fruit huge + countdown to their reel's lock time, then the
  result in huge type. Small ⚙ button top-right opens the same admin.
* `v2026-07-03.2` — Six-zone cabinet lighting in `button_wifi/`
  (top/main/buttons-panel surrounds + 3 reel-window borders on GPIO 18
  as one chain); page sends matched reel indices (`m`) in `result`;
  spin-button lamp on GPIO 23 driven by page state.
* `v2026-07-03.1` — **Booth architecture**: new `button_wifi/` firmware
  (button input on GPIO 19, egg switches moved off floating GPIO 35/36,
  ArduinoJson message parsing, serves `sounds/` from LittleFS, self-sync
  from GitHub Pages via `sync-manifest.txt`). Web: TAP TO START overlay
  (audio unlock), bundled-sound 404 → synth fallback, removed the
  pointerdown/click double-fire on the spin button, key auto-repeat
  guard, Transfer Settings JSON copy/paste in admin.
* `v2026-06-20.24` — Fix: leftover `stopRatchet()` call in `startSpin()`
  crashed every spin after the lever was removed.
* `v2026-06-20.21` — Lever retired; button-only input. Removed mobile lever
  drag / ratchet / lever-dots. Mobile bar = egg buttons only.
* `v2026-06-20.20` — Drum-style reel animation, bonus win sounds, auto-reset
  after result, ready-button flash + ESP32 `state` message.
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

See the header of `button_wifi/button_wifi.ino` for the current pinout,
library versions, and flash steps. Power: buck converter for the LED
strips (50 LEDs at full white ≈ 3 A) with common ground to the ESP32.
Do NOT put inputs on GPIO 34-39 — those pins are input-only with no
internal pull-ups (the old firmwares' egg switches on 35/36 floated and
fired at random; that is why they moved to 33/21/22).

The booth uses an iPad as the volunteer display, running this page in
full-screen Safari on the ESP32's access point. Fete-day iPad checklist:
forget/disable Auto-Join on every other WiFi network (iOS hops off
internet-less APs), disable auto-lock, enable Guided Access, mains or
battery power, wired audio out (3.5 mm jack on a 9th-gen iPad; USB-C PD
hub with 3.5 mm out on newer ones — never Bluetooth speakers), and one
tap on the TAP TO START overlay after loading the page.
