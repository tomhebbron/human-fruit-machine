# Sounds

Audio assets bundled with the game and served by GitHub Pages.

All files are played through HTML5 `<audio>` elements (not the Web Audio graph)
so they work reliably on iOS Safari — the system media route overrides the
ring/silent switch and survives FaceTime interruptions. Volume follows the
in-game volume setting.

Priority order for every sound: **operator's custom upload** (admin → SFX tab) →
**bundled file below** → **synthesised fallback** baked in JavaScript.

## Bundled files and their game events

| File | Event | Game key |
|------|-------|----------|
| `lose.mp3` | Loser spin (sad trombone) | `nowin` |
| `freesound_community-cash-register-purchase-87313.mp3` | Coin inserted | `coin` |
| `SlotMachine Jägerhuus.mp3` | Spinning reels (plays once — has built-in reel-stop sounds) | `spin` |
| `floraphonic-you-win-sequence-1-183948.mp3` | Pair win fanfare | `pairWin` |
| `floraphonic-you-win-sequence-2-183949.mp3` | Three-of-a-kind fanfare | `tripleWin` |
| `Jackpot-Millionaire.mp3` | Star-jackpot fanfare | `jackpot` |
| `mlg-airhorn.mp3` | Secret button 1 (⭐ / Q) | `egg1` |

## Files referenced but not yet uploaded

The game will look for these; if missing it falls back to a synth sound. Drop
the file in this directory with the exact filename to enable.

| File | Event | Falls back to |
|------|-------|----------------|
| `muttley-wheeze-or-laugh.mp3` | Secret button 2 (👥 / E) | crowd "ooooh" synth |

## Reel-stop timing

The Jägerhaus clip is **played once** (not looped) because it contains its own
mechanical lever-pull and three reel-stop clunks. `cfg.spinDuration` (1600ms)
and `cfg.reelDelay` (650ms) are tuned so the three reels lock during the clip's
~3.2s runtime, lining up the visible reel-stops with the clip's audible
stop-clunks. The per-reel "ching" bell and the lever-bottom "clunk" are
intentionally silent — they'd duplicate the sounds already in the clip.

If you swap in a different spin clip, retune those two settings in
**Admin → Settings → Timing** by ear.

## Unused / spare files

Currently in this directory but not wired to any event. They can be assigned
via the admin SFX upload panel as overrides.

- `floraphonic-playful-casino-slot-machine-bonus-1-183918.mp3`
- `floraphonic-playful-casino-slot-machine-bonus-2-183919.mp3`
- `floraphonic-playful-casino-slot-machine-bonus-3-183920.mp3`
- `floraphonic-playful-casino-slot-machine-jackpot-3-183921.mp3`

## `lose.mp3`

Played when a spin is a loser (`playNoWin()` → `playLoseSound()`), and
available as the **😬 Lose sound** test button in **Admin → Stats → Audio
Diagnostics**. If missing, the game falls back to a synthesised "wah-wah"
descending tone.

Source: https://quicksounds.com/uploads/tracks/1672552052_1643681967_1476870334.mp3
