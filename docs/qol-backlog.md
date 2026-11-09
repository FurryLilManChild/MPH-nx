# Quality of life backlog (after optimization)

Started 2026-11-09 from Mark's list. Not scheduled yet; picked up once the
in-game speed work is done.

## 1. Audio stretching through slow dips
- native14: done (SA-nx's WSOLA stretcher in drain_audio).
- Problem: when a frame runs long, the audio queue runs dry and the sound
  crackles.
- Now: native9 steers the playback rate by at most 0.6% for clock drift only.
  That is far too little to cover a real dip.
- Plan: time-stretch on underrun, as in the Dreamcast ports (look up the
  dream-recomp audio path for the reference). When the queue gets low, play
  the last audio stretched instead of silence, and catch up gently afterwards.
- Best done on top of the native mixer (native clock step 2), since that is
  where the sound gets made. A smaller version on the current output path is
  possible sooner.

## 2. Map the remaining second-screen items to buttons
- native13: first version done (stick/D-pad highlight, A confirm, B back on the captured screens). More screens as Mark captures them.
- The bottom screen has weapon select, morph ball, scan visor and so on. The
  port already makes the touch input, so a button can tap a fixed spot on the
  bottom screen. That is cheap and safe.
- Later, where it is cleaner, call the game's own function for that action
  directly.
- Needed from Mark: the list of items still needing a touch, and which
  buttons he wants for them (the remap menu can expose them).

## 3. Play without the second screen (top-only experience)
- Goal: a true single-screen port. What the bottom screen shows that matters
  in play moves onto the top screen's HUD.
- Yes, we can add to the HUD. The presenter already composes the final image
  itself (the HD compose pass), so it can draw extra panels over the top
  screen.
- First version: cut the useful parts (radar, ammo, weapon icons, and in
  multiplayer the scores and octolith status) out of the bottom screen the
  game already draws, and place them as small HUD panels on the top screen,
  at the chosen resolution. No game changes needed; the content is always
  correct because the game drew it.
- Later version, if wanted: native HUD elements drawn from the game's state,
  for a cleaner look.
- Touch-only actions on that screen are covered by item 2. The full bottom
  screen stays one button away (the - menu or a hold) for anything rare.
- Would ship as a new screen layout ("Top only + HUD"), so the current layouts
  stay.

## 4. Lock-on targeting (as in the other Prime games)
- Hold a button (L, as in Prime 1 to 3) to lock onto the nearest visible
  enemy in front of Samus; the aim then tracks it and strafing circles it.
- How: the game is native code with its memory in plain Switch memory, so
  the port can read the enemy list and Samus's position and aim each frame,
  pick a target, and feed the aim input the game already reads (the same
  path the right stick uses). No changes to the game's own code are needed.
- Needs: the layout of MPH's entity and player data. NoneGiven's MphRead (a
  reimplementation of the game's engine) is the reference to learn it from.
- Open questions: single player only, or multiplayer too (lock-on against
  other players changes the balance, so likely off or optional there); a
  reticle drawn on the target with the HUD work in item 3; target cycling.
