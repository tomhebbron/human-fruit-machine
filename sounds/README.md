# Sounds

Audio assets bundled with the game and served by GitHub Pages.

## `lose.mp3`

Played when a spin is a loser (via `playNoWin()` → `playLoseSound()`), and
available as the "😬 Lose sound" test button in **Admin → Stats → Audio
Diagnostics**.

It is played through an HTMLAudioElement (not the Web Audio graph) because the
HTML5 media path is far more reliable on iOS Safari — it uses the system media
route that overrides the ring/silent switch. Volume follows the in-game volume
setting via the element's `.volume`.

If `lose.mp3` is missing, the game falls back to a synthesised "wah-wah"
descending tone, so nothing breaks — but the bundled sound won't play until the
file is added here.

Source: https://quicksounds.com/uploads/tracks/1672552052_1643681967_1476870334.mp3
