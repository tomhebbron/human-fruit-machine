# Project review — button hardware, ESP32-as-hub, iPad as volunteer display

Reviewed at `v2026-06-20.24` (commit 6990422). Scope: `index.html`, `lever/lever.ino`,
`lever_wifi/lever_wifi.ino`, deploy workflow, sound assets, and the new plan
(pushbutton instead of lever; ESP32 runs the booth; iPad is the volunteer display).

Findings are ordered by how likely they are to ruin fete day.

---

## P0 — will break the booth

### 1. Neither firmware has a button input

Both sketches still read a potentiometer on GPIO 34 and synthesise lever levels
1-5 plus a release event. There is no digital button input anywhere. If you wire
a pushbutton and leave the pot disconnected, GPIO 34 floats and the smoothed ADC
value wanders across the level thresholds — the firmware will emit phantom
lever/release events, i.e. spins and ahems at random.

**Fix:** add a dedicated button pin (use a pin with internal pull-ups, e.g.
GPIO 33 class — NOT 34/35/36/39, see finding 2), `INPUT_PULLUP`, 30-50 ms
debounce, and on press send the spin trigger directly (`Space` over BLE, or
`{"t":"release","coin":true}` over WS). Delete the pot-reading code, the
level 1-5 keystrokes, and `LM_PULLING` — dead plumbing now invites noise.

### 2. GPIO 35 and 36 have no internal pull-ups — random jackpot arming

`pinMode(EGG_SW[i], INPUT_PULLUP)` with `EGG_SW[] = {33, 35, 36}`: GPIO 34-39
are input-only pins **without** internal pull-up/pull-down hardware. The call
silently does nothing on 35 and 36, so those inputs float. Floating inputs read
LOW at random → egg buttons 1 and 2 fire spontaneously.

Egg button 1 (GPIO 35, key `w`) is **eggForceJackpot** — a floating pin will
randomly arm/disarm guaranteed jackpots all afternoon. Egg 2 (GPIO 36) spams the
crowd sound.

**Fix:** either add external 10 kΩ pull-ups to 3V3 on GPIO 35/36, or move the
egg switches to pins with working internal pull-ups (e.g. 25/26/27 are taken by
LEDs; 14, 18, 19, 21, 22, 23 are free). Applies to both firmwares.

### 3. Page served from the ESP32 has almost no sound

`lever_wifi` serves exactly one route (`/` → `index.html`) — every request to
`sounds/*.mp3` 404s. Worse, the web app's `playBundled()` "returns true
optimistically" and `playUrl()` has no error fallback, so the failure is
silent silence: coin, spin, pair/triple/jackpot sounds all produce **nothing**
(only `lose.mp3` and the two egg sounds go through `bundledEl()`, which tracks
404s and falls back to synth).

So in the exact configuration planned for the fete — iPad on the ESP32's AP —
the game is mostly mute.

**Fixes (do both):**
- Upload `sounds/` to LittleFS alongside `index.html` and add
  `server.serveStatic("/sounds", LittleFS, "/sounds");`. The MP3s total 1.5 MB
  and `index.html` is 100 KB — that does not fit the default 4 MB partition
  scheme's ~1.4 MB SPIFFS/LittleFS region once formatted. Select a partition
  scheme with ≥2 MB FS (e.g. "No OTA (2MB APP / 2MB FS)") and document it in
  the sketch header, or drop the 4 unused floraphonic files (~580 KB) to fit.
- Make `playBundled()` resilient anyway: attach an `error` listener when a pool
  element is given a `sounds/` URL and fall back to the baked synth, the same
  way `bundledEl()` already does. Then a missing file is a quieter fete, not a
  silent one.

### 4. Audio is locked until someone touches the iPad — with no indication

`_startAudio()` unlocks the HTML5 pool on the first trusted gesture
(`touchstart`/`click`/`keydown`). In the booth, nobody touches the iPad: all
input arrives over WebSocket, which is not a trusted gesture. After every page
load / reload / Safari tab resurrection, the machine runs visually but silently
until a volunteer happens to tap the screen.

**Fix:** show a full-screen "TAP TO START" overlay on load that only disappears
inside the tap handler (which also calls `unlockPool()` + primes the dedicated
elements). This turns an invisible failure mode into a one-tap setup step that
cannot be forgotten. Note: with the BLE variant a keyboard keystroke counts as
`keydown` and unlocks audio, but the WS variant gets no such freebie — the
overlay is required for the planned setup.

### 5. `lever_wifi` mis-parses the page's state messages — losers get the win light show

The page calls `sendToEsp32({t:'state', s:state})` on **every** `updateUI()`.
The firmware parses JSON by substring: `strhas(msg, "\"result\"")` matches
`{"t":"state","s":"result"}`. Sequence after every spin:

1. Page sends `{"t":"result","type":"none"}` → firmware sets `LM_LOSE`. Correct.
2. Page sets `state='result'`, calls `updateUI()` → sends
   `{"t":"state","s":"result"}` → firmware matches `"result"`, finds neither
   `jackpot` nor `"none"` in the payload → `setLight(LM_WIN)`.

Every outcome — lose and jackpot included — ends in the generic rainbow WIN
animation a few milliseconds after the correct one starts. The distinct
jackpot strobe / red lose fade can never be seen.

**Fix:** parse properly. ArduinoJson is one include and ~6 lines
(`doc["t"] == "result"`), or at minimum match on `"\"t\":\"result\""`. While
there, handle the `state` message deliberately (it is useful for re-syncing
lights after an ESP32 reboot) instead of letting it alias other messages.

### 6. Tapping the spin button on the iPad double-fires — free games in coin mode

`#spin-btn` has **two** handlers on touch devices: `click` (registered
unconditionally, line ~1919) and `pointerdown` (registered when
`isTouchDevice`, line ~2470). One tap runs both. In `result` state:

- `pointerdown` → `onSpace()` → resets to idle state;
- `click` (fires right after) → in credit mode `onSpace()` → **starts a spin
  immediately** — tap "Again!" and it spins with no deliberate trigger;
- in coin mode the reset lands on `nocoin`, and the click handler's
  `if (state === 'nocoin') onCoin()` **credits a game without a coin**.

**Fix:** delete the `pointerdown` handler and keep only `click` (moving
`unlockPool()` into the click handler). `click` fires on tap; there is no need
for both.

---

## P1 — will make it a pain to flash, build, and operate

### 7. The flashing toolchain described in the sketch headers no longer exists

- `lever_wifi` says "Tools → ESP32 Sketch Data Upload". That plugin is Arduino
  IDE 1.x only. In IDE 2.x you need the separate **arduino-littlefs-upload**
  plugin (`.vsix` dropped into `~/.arduinoIDE/plugins/`, then
  Ctrl+Shift+P → "Upload LittleFS"), or PlatformIO's `uploadfs` target.
- `ESPAsyncWebServer by lacamera` is an abandoned fork that fails to compile
  against ESP32 Arduino core 3.x. The maintained fork is the **ESP32Async**
  org's ESPAsyncWebServer + AsyncTCP (both now in Library Manager).
- `ESP32-BLE-Keyboard by T-vK` is **not** in Library Manager (header says it
  is) and also breaks on core 3.x unless you use a patched fork or pin the
  ESP32 core to 2.0.x.

**Fix:** pin exact versions in the sketch headers ("tested with ESP32 core
X.Y.Z, library A vN.N") and write down the actual LittleFS upload steps,
including the partition-scheme choice from finding 3. Ten minutes of
documentation now versus an evening of dependency archaeology the night before
the fete.

### 8. No repeatable path from `index.html` to the ESP32's filesystem

There is no `lever_wifi/data/` directory in the repo and no copy step. Every
web change requires manually copying `index.html` (and now `sounds/`) into
`data/` and re-uploading — forget it once and the booth runs a stale build
while GitHub Pages runs the new one, with only the tiny admin version string to
tell you. **Fix:** commit a `lever_wifi/data/` that is populated by a one-line
script (`cp index.html sounds/ -r lever_wifi/data/`), and check the admin
version string as part of the flash ritual.

### 9. iOS will silently hop off the ESP32's access point

The `FruitMachine` AP has no internet. iOS deprioritises internet-less networks
and will auto-join a remembered network that has connectivity (a phone hotspot,
school WiFi) — the iPad drops off the AP mid-fete, the WS dot goes grey, and the
button stops working with no obvious cause. **Fix (operational):** on the booth
iPad, forget or disable Auto-Join on every other network, then enable Guided
Access (also solves auto-lock and children poking the admin panel). Add this to
a fete-day checklist in the README.

### 10. The "volunteer display" is currently an afterthought strip

If the iPad is the volunteer display, the volunteers' actual UI is
`#vol-preview`: a 3 rem fruit strip at the bottom of a player-oriented screen.
Across a booth, in daylight, that is squint territory — and everything above it
(spinning reels, prize banner) is noise for volunteers and a spoiler if a child
can glimpse the screen.

**Fix:** add a proper volunteer mode (a `?volunteer=1` URL flag or admin
toggle): full-screen layout showing the three chosen fruits at maximum size the
moment the spin starts, each with a countdown to its reel-lock time
(`cfg.reelStops`) so each volunteer knows when to hold their fruit up, then the
outcome (WIN / LOSE / JACKPOT) in huge type. The existing game screen remains
for laptop testing and any player-facing use.

### 11. Held keys auto-repeat through the state machine

The keydown handler ignores `e.repeat`. Holding Space cycles
result → reset → spin → … chain-spinning the machine. Harmless from the
firmware (single keystrokes) but easy to hit in testing and trivially fixed:
`if (e.repeat) return;` at the top of the handler.

---

## P2 — loose ends and drift

### 12. Sound documentation contradicts the code

`sounds/README.md` and CLAUDE.md say pairWin/tripleWin are the floraphonic
"you win sequence" files and jackpot is `Jackpot-Millionaire.mp3`, and list the
floraphonic *bonus* files as "unused/spare". The code (`_BUNDLED_FILES`) wires
the opposite: `bonus-1`/`bonus-2` for pair/triple and
`slot-machine-jackpot-3` for jackpot; `Jackpot-Millionaire.mp3` and both
"you win sequence" files are the ones actually unused. An operator swapping a
sound will edit the wrong file. Fix the two docs to match the code. (The
README's "Reel-stop timing" section also still describes the retired
`spinDuration`/`reelDelay` settings; it is now `reelStops`/`spinEnd`.)

### 13. CLAUDE.md still describes the lever era

Lever states, `1-5` keys, drag-lever mobile UI, `spinDuration`/`reelDelay`
defaults, coin-mode default `true` — all superseded. Since CLAUDE.md is the
context every future session starts from, stale content here actively causes
wrong changes. Rewrite the Controls / Game flow / Settings sections for
button-only input and update the changelog.

### 14. Dead lever plumbing in the web app

`onLeverLevel`, `setLeverLevel`, the `pulling` state, keys `1-5`, the `lever`
WS message, and the admin Keys tab's five lever-pull rows are all vestigial.
Not harmful, but they widen the state machine (e.g. the release/ahem gate) for
no benefit and the Keys tab now documents controls that don't exist. Prune once
the firmware button rework (finding 1) lands, so page and firmware speak the
same, smaller protocol.

### 15. BLE firmware's light timings are stale

`SPIN_MS 3000 / REEL_GAP_MS 900` versus the game's actual
1890/2890/3890/5500 ms — the LED reel-lock choreography is out of sync with
the screen. If you keep the BLE variant at all, copy the current defaults; if
the fete runs on `lever_wifi` (recommended — it gets real events), consider
deleting `lever/` to remove a second firmware to maintain, or mark it clearly
as the fallback.

### 16. Coin mode swallows a second coin

`coinReady` on the ESP32 and `ready` on the page are booleans — a second £1
inserted while ready is eaten with no credit. Fine in credit mode (the fete
default), but if coin mode is ever used, either physically block the slot or
add a small credit counter.

### 17. Minor notes

- Stats "Takings" assumes £1 × plays even in credit mode; the calibrate tab
  already handles this distinction — the Stats tab could reuse it.
- `eggForceJackpot` results are logged as real jackpots and skew the profit
  stats by £4 a pop; consider flagging forced spins in history.
- BLE variant only: while a BLE keyboard is connected, iOS hides the on-screen
  keyboard, so editing prize text in admin needs the keyboard icon workaround;
  and if an admin text input has focus, firmware keystrokes type garbage into
  it instead of driving the game. The WS variant has neither problem.
- `FastLED.show()` disables interrupts and can glitch async WiFi under load;
  if the strips flicker or WS drops during animations, cap the frame rate or
  move `show()` to the second core.
- If the board is a WROVER (PSRAM), GPIO 16 is unavailable for strip 3 —
  pinouts assume WROOM.
- LED surges on a sagging battery can brown out the ESP32 and drop the AP
  mid-game; `LED_BRIGHT 180` helps, but keep the buck converter's headroom and
  add a bulk capacitor across the strip supply.

---

## Recommended architecture (answering "the ESP32 runs everything")

Keep the game brain in the browser; make the ESP32 the booth's *infrastructure*:

- ESP32 (`lever_wifi` variant): WiFi AP, serves `index.html` + `sounds/` from
  LittleFS, reads the button + coin sensor + egg buttons, drives the LEDs from
  real game events over WS.
- iPad: joins the AP, loads `http://192.168.4.1`, runs the game logic, plays
  audio, shows the volunteer display. One tap on the start overlay at power-up.

Moving the outcome logic/odds/pity/stats into firmware would be a rewrite that
sacrifices the admin panel, the simulator, and localStorage history for no
operational gain — the page already survives everything except the iPad dying,
and if the iPad dies there is no display or sound anyway. The genuinely
load-bearing changes are P0 items 1-6 plus the flash-pipeline fixes (7, 8).

Suggested order of work:

1. Firmware: button input + egg pin/pull-up fix + ArduinoJson parsing
   (findings 1, 2, 5) — one new `button_wifi` sketch, retire the pot code.
2. Web: sound 404 fallback, start overlay, remove the `pointerdown` double-fire,
   `e.repeat` guard (3, 4, 6, 11).
3. Serve `sounds/` from LittleFS + partition scheme + `data/` copy script and
   documented flash steps (3, 7, 8).
4. Volunteer display mode (10).
5. Docs: CLAUDE.md, sounds/README.md, fete-day checklist (9, 12, 13).
