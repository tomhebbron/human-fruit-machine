# Sounds

Audio assets bundled with the game and served by GitHub Pages.

All files are played through HTML5 `<audio>` elements (not the Web Audio graph)
so they work reliably on iOS Safari — the system media route overrides the
ring/silent switch and survives FaceTime interruptions. Volume follows the
in-game volume setting.

The upload-SFX panel in **Admin → SFX** lets operators replace any sound with a
custom upload, which takes priority over the bundled files below. Bundled files
take priority over the synthesised fallbacks baked in the JavaScript.

## Bundled files and their game events

| File | Event | Game key |
|------|--------|----------|
| `lose.mp3` | Loser spin (sad trombone) | `nowin` |
| `freesound_community-cash-register-purchase-87313.mp3` | Coin inserted | `coin` |
| `mlg-airhorn.mp3` | No-coin ahem alert | `ahem` |
| `floraphonic-playful-casino-slot-machine-bonus-1-183918.mp3` | Reel stop ching | `ching` |
| `floraphonic-you-win-sequence-1-183948.mp3` | Win (pair / 3-of-a-kind) | `win` |
| `Jackpot-Millionaire.mp3` | Jackpot fanfare | `jackpot` |
| `floraphonic-you-win-sequence-2-183949.mp3` | Party / celebration | `party` |
| `floraphonic-playful-casino-slot-machine-jackpot-3-183921.mp3` | Crowd "ooooh" | `crowd` |
| `SlotMachine Jägerhuus.mp3` | Spinning reels (loops) | `spin` |

## Unused / spare files

These are in the directory but not currently wired to a game event. They can
be assigned via the admin SFX upload panel.

- `floraphonic-playful-casino-slot-machine-bonus-2-183919.mp3`
- `floraphonic-playful-casino-slot-machine-bonus-3-183920.mp3`

## `lose.mp3`

Played when a spin is a loser (via `playNoWin()` → `playLoseSound()`), and
available as the "😬 Lose sound" test button in **Admin → Stats → Audio
Diagnostics**.

If `lose.mp3` is missing the game falls back to a synthesised "wah-wah"
descending tone.

Source: https://quicksounds.com/uploads/tracks/1672552052_1643681967_1476870334.mp3
