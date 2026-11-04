# Metroid Prime Hunters (DS) to Nintendo Switch: Feasibility Analysis

Prepared November 4, 2026

## Bottom line

Feasible, and a better starting position than most of our ports. A native PC recompilation of Metroid Prime Hunters already exists and is under very active development, and someone has already ported it to ARM64 Android with a GLES renderer, twin-stick controls and no interpreter in play. Our job would be a Switch platform layer plus the control and second-screen design you described, not a recompilation from scratch.

The one real risk is CPU performance on the Tegra X1. Everything else (code generation, renderer, input, audio, licensing) has a proven path. I'd rate it:

| Question | Confidence |
|---|---|
| Can it boot and play on Switch? | High |
| Can the control scheme you described be built? | High |
| Will it hold a locked 60 at native resolution? | Medium, unproven until we measure |
| Will it hold 60 with HD rendering on Switch? | Low to medium; plan on 1x or 2x |

## What already exists

### 1. Metroid Prime Hunters Recomp (mstan) - the base

- Repo: mstan/MetroidPrimeHuntersRecomp, built on mstan's ndsrecomp framework (now under RetroPortingToolKit/ndsrecomp).
- How it works: the game's ARM9 and ARM7 code is statically recompiled to C, and the DS hardware (2D/3D GPU, sound, Wi-Fi, timers) is reproduced around it using melonDS device models. So it is a recomp with a hardware layer, closer to our CV64/Rogue Squadron work than to a decomp-based port like Echoes Unbound.
- Status: public alpha, currently in the v0.7.x line (v0.7.6 per the README, release notes up to v0.7.8). v0.7.5 added compiled coverage for all 66 campaign rooms, including Gorea and the ending, with no interpreter fallback in the Gorea checks.
- Features already done: Prime-style controls (keyboard/mouse and full gamepad), Dynamic widescreen plus fixed 4:3, 5:3, 2:1 and 21:9, an HD rendering mod (up to 4x internal resolution with texture filtering), OpenGL or Software 3D renderer, saves, offline bot combat, and experimental Wiimmfi online play.
- Gamepad defaults already map nearly every touch-screen action to a button: right stick aim, RT shoot, LT scan-fire, A jump, B morph ball, X missile, LB beam, RB boost/zoom, R3 scan visor, D-pad left/right scan-message arrows, Start menu, and subweapons 1 to 6 bindable. There is also a "virtual stylus" mode for anything left over.
- License: the repo's own code is MIT; release binaries are GPL-3.0-or-later because the runner vendors melonDS code. Clean for our purposes (we'd publish our source, same as any GPL fork).
- AI use: openly AI-assisted (about 81 of 284 commits credit an AI co-author per Emulation King), same as Echoes Unbound.

### 2. Hunters Recomp for the AYN Thor (aabrole) - the ARM64 proof

- A native Android port of the recomp above, on a patched ndsrecomp fork (branch v022-golden) and a patched MPH recomp branch (thor-golden).
- Proves the three things that usually sink a port like this:
  - The recompiled banks compile and run correctly as native arm64.
  - The desktop GL 4.3 compute renderer works when ported to GLES 3.2.
  - With enough coverage, the game runs with no interpreter fallback in play.
- Measured 58 to 59 fps at 3x internal resolution on an AYN Thor Pro (Snapdragon 8 Gen 2). Their own notes say 4x makes the CPU wait on the GPU and audio can crackle.
- Also does real dual-screen (bottom screen on the second panel with touch) and twin-stick controls.
- License: GPL-3.0.

### 3. melonDS for Switch - the hardware precedent

- melonDS has an unofficial Switch port (first by Hydr8gon, later maintained by RSDuck, who added an ARM64 JIT). Its later releases moved both the 2D and 3D renderers to deko3d to cut CPU cost.
- That matters because ndsrecomp's GPU code comes from melonDS. A deko3d DS renderer that already lives in the melonDS family is a realistic fallback (or upgrade) if the GL compute renderer is too heavy on Switch.

### Not useful as a base

- melonPrimeDS: an emulator hack (memory injection, USA rev 1 only, keyboard/mouse focus). Good as a controls reference only, which the recomp already used.
- MphRead (NoneGiven): an MIT file-format, room and entity reference written in C#. The recomp uses it as a semantic reference; it is not a playable game engine we could port.

## The hard parts, ranked

### 1. CPU performance on Tegra X1 (main risk)

The recomp is not "just native code." It still runs both DS CPUs through an event-aligned scheduler with cycle accounting, and it composites the DS 2D layers in software. The author's v0.6.9 notes show how much this costs even on PC: a "performance release for machines that dip" took a test machine from about 46 to about 60 fps, partly by finally enabling a threaded scanline renderer and threading the widescreen compositor.

The Thor hits 58 to 59 on a Snapdragon 8 Gen 2, which is a much faster CPU than the Switch's (unverified estimate: very roughly 2 to 3 times faster per core than a Switch at its 1785 MHz overclock). So my honest expectation for a first Switch build is somewhere well under 60, possibly in the 30s or 40s, before optimization. That is a guess until we measure, and I'll treat the first performance test as exactly that: a measurement, not a promise.

Levers we have, roughly in order of payoff:
- Use the overclock (you already run sys-clk).
- Move 2D compositing to the GPU (the deko3d approach melonDS-nx took).
- Tune worker threads for the 3 application cores Horizon gives us (we learned on Rogue Squadron that Horizon does not time-slice between priorities, so thread layout matters).
- Ship full native bank coverage so the interpreter never runs.
- Relax timing accuracy in hot paths where the game does not care (the same idea as the "HLE shortcuts" upstream started adding in v0.7.6).
- Keep internal resolution at 1x or 2x.

### 2. The runtime overlay compiler

Since v0.6.0, upstream ships an embedded CPython and TinyCC so the game can recompile overlay code it has not seen yet, while you play. That will not come along to Switch as-is. The fix is the Thor approach: precompile all overlay coverage into the build (upstream now covers all 66 campaign rooms), keep the interpreter as a safety net for anything missed, and log any interpreter hits so we can add them in the next build. Multiplayer arenas need their coverage checked too.

### 3. Renderer choice

Three options, in the order I'd try them:
1. Software renderer, only to get first boot (it will be slow, and that is expected).
2. The GL/GLES compute renderer on Switch Mesa. Unverified: whether devkitPro's Switch Mesa exposes the GL 4.3 / GLES 3.2 compute features it needs. I'll check this early with a tiny test build.
3. A deko3d DS renderer modeled on melonDS-nx, if option 2 is missing features or too slow.

### 4. SDL version

Upstream moved to SDL3 in v0.6.0; the Thor fork credits SDL2, so it likely tracks an older framework line (my inference). Switch has a solid SDL2 port; SDL3 on Switch is something I have not verified. If SDL3 is a problem, the runner's frontend is thin enough that we can write a libnx/SDL2 frontend for it.

### 5. Things that are easy here

- Memory: the DS has 4 MB of main RAM and the ROM is 64 MB. No repeat of the Legacy of Darkness heap fight.
- Audio: the DS sound chip is reproduced in the runner, already paced.
- Saves: cartridge saves already persist; we just point the path at the SD card.
- Build system: CMake with Python tools for bank generation, so it fits your PC build loop.

## Control and second-screen design

Your plan, plus a few things I'd suggest adding.

### Twin stick and button mapping

Start from upstream's gamepad defaults, which already cover movement, aim, fire, scan-fire, jump, morph ball, missiles, beam cycling, boost, scan visor, scan-message arrows and menu. Adapted for Joy-Con / Pro Controller:

| Switch input | Action |
|---|---|
| Left stick | Move (D-pad in menus) |
| Right stick | Aim |
| ZR / ZL | Shoot / scan-fire |
| B | Jump |
| A | Morph ball (alt form) |
| Y | Missile |
| X | UI OK (confirm on scan messages, prompts) |
| L / R | Beam select / boost-zoom |
| R3 | Scan visor |
| D-pad left/right | Scan-message arrows |
| D-pad up/down | Spare: candidates are subweapon cycle or quick map |
| + | Pause menu |
| - | Bottom-screen menu (see below) |

All rebindable from an in-game NX Options menu, like cv64-nx.

### The minus-button bottom screen

- Press - and the DS bottom screen appears as an overlay on top of the game view; press - again (or B) to close it.
- Handheld mode: the Switch has a touchscreen, so touches on the overlay go straight through as stylus input. This is the cleanest version of the idea and costs us almost nothing.
- Docked: a cursor driven by the right stick (or gyro), with ZR or A as "tap" and hold as "drag."
- One design question for you: the DS bottom screen is live during combat, so do we keep the game running while the overlay is open (faithful, and it matters for things like weapon swaps mid-fight) or pause it? Since we control the scheduler, we could offer both as an option. My suggestion: live by default, with a "pause while open" toggle.

### Suggestions beyond your spec

- Gyro aiming (optional, off by default): Joy-Con gyro added to right-stick aim. Metroid Prime Remastered set that expectation on Switch, and it suits Hunters' fast aiming.
- Picture-in-picture radar: a small corner copy of the bottom screen's radar area, toggleable, so you rarely need to open the overlay at all.
- Direct buttons for everything we can find, so the overlay is only needed for menus and rare actions. Upstream already got most of the way there.

## Before we start: ROM revision

The recomp supports only one dump: Metroid Prime Hunters USA revision 0 (game code AMHE, 64 MiB, SHA-1 90164d1ac127ee5f9815ea4ae7de798c7b5fc629). Revision 1, Europe and Japan are rejected. That is the opposite of melonPrimeDS, which needs revision 1. The Thor project suggests dumping a cartridge with GodMode9 on a modded 3DS. You'll need a dump from your own cart that matches that hash; if you don't have a way to dump DS carts, that's the first thing to solve.

## Proposed phases

1. PC baseline (your PC): build upstream as-is with your ROM, confirm it plays. Also gives us a reference for logs and coverage files.
2. Switch bring-up: devkitPro aarch64 build of the runner and generated banks with the live overlay compiler removed, interpreter kept as fallback, SD card logging and a safe crash handler (reusing what we built for cv64LoD-nx), software renderer. Goal: boots to the title screen. Expect slow.
3. GPU renderer: GL/GLES compute path first, deko3d if needed. Goal: in-game at a measurable frame rate.
4. First performance test: measure in a few known rooms with sys-clk at max. This decides how much optimization work phase 5 needs.
5. Optimization pass (as many rounds as it takes).
6. Controls and UI: twin-stick mapping, minus-button overlay with handheld touch and docked cursor, gyro option, NX Options menu with remapping.
7. Polish: widescreen modes, internal resolution option, save path, icon and launcher.

Wiimmfi online is out of scope for the first release; offline campaign and bot combat come first.

## Effort estimate

Comparable to Rogue Squadron or Legacy of Darkness: we inherit a working PC port, but the hardware layer and renderer need real Switch work and performance is the open question. It would also be our first DS port, so the workflow is worth packaging into a new DS skill afterward, in the same family as the N64, Xbox, Xbox 360 and Dreamcast ones.

## Unverified items (to check during bring-up)

- Whether devkitPro's Switch Mesa supports the compute features the GL renderer needs.
- Whether SDL3 builds on Switch, or we need an SDL2/libnx frontend.
- How the Tegra X1 compares to the Snapdragon 8 Gen 2 in this workload (my 2 to 3x figure is a rough estimate, not a measurement).
- Whether the recomp's Prime Controls rely on any rev 0 memory hooks we'd need to adapt (they shouldn't, since we'd use the same ROM).

## Sources

- [mstan/MetroidPrimeHuntersRecomp](https://github.com/mstan/MetroidPrimeHuntersRecomp) and its [current README](https://raw.githubusercontent.com/mstan/MetroidPrimeHuntersRecomp/main/README.md)
- [MPH Recomp v0.6.9 release notes (performance)](https://newreleases.io/project/github/mstan/MetroidPrimeHuntersRecomp/release/v0.6.9-alpha)
- [MPH Recomp v0.6.0 release notes (live overlays, SDL3)](https://newreleases.io/project/github/mstan/MetroidPrimeHuntersRecomp/release/v0.6.0-alpha)
- [RetroPortingToolKit/ndsrecomp releases](https://github.com/RetroPortingToolKit/ndsrecomp/releases)
- [aabrole/HuntersRecomp (AYN Thor Android port)](https://github.com/aabrole/HuntersRecomp)
- [Emulation King: Metroid Prime Hunters Recomp](https://emulationking.com/nintendo/nds/games/metroidprimehunters/ports/metroidprimehuntersrecomp/)
- [GameBrew: melonDS Switch](https://gamebrew.org/wiki/MelonDS_Switch)
- [Emulation General Wiki: melonDS](https://emulation.gametechwiki.com/index.php/MelonDS)
- [makinori/melonPrimeDS](https://github.com/makinori/melonPrimeDS)
