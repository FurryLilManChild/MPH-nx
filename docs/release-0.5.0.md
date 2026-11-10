# MPH-nx 0.5.0

The first public release of MPH-nx, a native Nintendo Switch port of Metroid Prime: Hunters (Nintendo DS).

MPH-nx is a static recompilation. The game's DS code was translated ahead of time into Switch code and runs as a native homebrew app with its own renderer, audio and controls. It is not an emulator.

**No game data is included.** You need your own dump of the game: USA Rev 1 (AMHE, ROM version 1), untrimmed 64 MiB, SHA-1 `a8679d4f8dcb445cbe946a424241ef44901b0caf`. The .nro holds translated program code only; graphics, sound, levels and text are read from your ROM at launch.

## Highlights

- **Native code.** Both of the DS's processors run as translated Switch code, with a GPU renderer on deko3d.
- **Prime-style controls.** Twin-stick aim, ZR to shoot, and the touch screen's buttons on the controller.
- **Lock-on.** Hold ZL to lock the aim onto the enemy nearest the centre of view, as in the Prime games (adventure mode).
- **Weapons.** D-pad left and right cycle weapons. Hold D-pad up for the weapon wheel and pick with the stick.
- **Top only + HUD layout** (the default). The top screen fills the display, with the weapon strip, radar and scan results drawn over it. Menus switch to the full bottom screen on their own.
- **Other layouts.** Stacked, Big top + small bottom, and Top only.
- **Save states.** Four slots, from the port menu.
- **Smoother audio.** Audio is stretched during slowdowns instead of crackling.
- **Faster boot.** The white boot splash is skipped.
- **Port menu (hold -).** Layout, resolution, CPU clock, controls rebinding, save states and more.

## Install

1. Copy the `switch` folder from the zip to the root of your SD card.
2. Put your ROM in `/switch/mph-nx/`.
3. Launch through title takeover: hold R while starting any game, then pick MPH-nx. Applet mode (the Album) does not have enough memory.

## Known limits

- Single-player adventure is the tested path. Multiplayer modes have barely been tested.
- In the Top only + HUD layout, a screen the port does not recognise shows the top screen only. Tap - to see the bottom screen there.
- Higher internal resolutions may need the CPU clock option to hold full speed.
- Save states only load in the build that made them. Use the game's own save to carry progress between versions.
- Online play and DS wireless are not supported.
- USA Rev 1 only. Rev 0 and other regions are not supported.

## Licence

The .nro includes melonDS code and is distributed under GPL-3.0-or-later. Third-party notices are in the zip's `LICENSES` folder.

Metroid Prime: Hunters is a trademark of Nintendo. This project is not affiliated with or endorsed by Nintendo.
