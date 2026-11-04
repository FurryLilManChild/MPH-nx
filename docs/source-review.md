# MPH-nx: upstream source review

November 4, 2026. Read from the exact sources your PC baseline (v0.7.12-alpha) was built from.

## What was read

- Engine: RetroPortingToolKit/ndsrecomp, branch release/native-hle-v0.6.11, commit 179ac63 (the exact pin in your SOURCE.txt and game.toml)
- Title: mstan/MetroidPrimeHuntersRecomp (main)
- Thor port: aabrole/ndsrecomp branch v022-golden (engine VERSION 0.0.1 era, last commit Sep 5 2026) and aabrole/HuntersRecomp, including docs/AUDIT-2026-09-05.md

## Answers to the open questions

### What "tier3" means
From docs/dispatch_architecture.md, the engine has three tiers:
- Tier 1: statically recompiled native C (the precompiled banks)
- Tier 2: a JIT for code copied into RAM (described as deferred / not built)
- Tier 3: the bounded ARM/Thumb interpreter, "the always-correct floor"

So tier3 in your diagnostics is interpreter fallback. Your PC run's two dips line up with tier3 bursts, so they were spots where the banks didn't cover the code that was running.

### How dynarmic is used
- runner/CMakeLists.txt only enables dynarmic when the host is Windows x64 (`WIN32 AND ... x86_64`).
- When it's enabled, the scheduler tries the dynarmic path first (scheduler.cpp calls nds_arm9_native_run) and falls back to the banks and the interpreter.
- So your Windows build mostly runs game code through the dynarmic JIT. The Linux AppImage and the Thor port do not; they use the "portable" path (banks plus interpreter).
- One of the four required patches (dynarmic-cold-register-allocation) edits dynarmic's x64 backend, so it has no ARM64 equivalent today.
- THIRD_PARTY_ATTRIBUTION.md: "Faithful and portable builds do not require the SDK."

Consequence: the Switch build starts on the portable path, the same one Thor uses. The PC numbers (4 to 10 ms per frame of emulation) do NOT transfer to Switch, because they include the JIT.

### Licensing
- Engine project code is now PolyForm Noncommercial 1.0.0, except runner/src, recompiler/armv4t, recompiler/support and oracle, which stay MIT (they link melonDS, so binaries are GPL-3.0-or-later).
- Fine for a free, personal homebrew release. It rules out any commercial use. Clearly licensed, so it doesn't hit your rule about unlicensed upstream repos.

## Performance evidence for low-power hardware

| Host | Engine path | Emulation ms/frame | FPS | Source |
|---|---|---|---|---|
| Your Ryzen Z1 Extreme | Windows, dynarmic JIT | 4 to 10 (mostly 6 to 9) | ~60 | your diagnostics |
| AYN Thor (Snapdragon 8 Gen 2) | portable banks, older engine | ~14 native CPU + ~2.5 present | 59.3 at 3x | Thor AUDIT-2026-09-05.md (B13) |
| Steam Deck (Mario Kart DS, not MPH) | portable, older engine | 18.0 | 51.5 | ndsrecomp docs/steamdeck-dispatch-cache-2026-09-13.md |

Switch estimate (unverified, my own reasoning): a Switch CPU core at 1785 MHz is several times slower single-threaded than the Thor's fastest cores. A rough guess is 4 to 5 times. Scaling Thor's 14 ms gives something like 55 to 70 ms per frame, or about 14 to 18 fps on the same code path. Newer engine releases include host-side speedups the Thor build predates, which may claw some back. The first Switch boot test will replace this guess with a measurement.

## Levers, revised

1. Measure first (boot test).
2. Bring the newer engine's portable-path speedups over (Thor is on an older engine).
3. Multi-core: upstream MPH runs "serial" (SOURCE.txt), and there's a CPU/render handoff profile for Mario Kart DS (cpu_render_handoff.cpp) that is not enabled for MPH. Switch has 3 usable cores.
4. Coverage: bake in every bank so the interpreter (tier3) never runs.
5. dynarmic on Switch: dynarmic supports AArch64 hosts, and an ARM guest on an ARM host is its best case. It would need libnx JIT memory support and ARM64 versions of the custom patches. This is the biggest possible win and also the biggest job. Treat it as a later phase, only if 2 to 4 aren't enough.
6. Overclock (sys-clk max).

## Notes for the repo

- Keep diagnostics/, live-shard-cache/ and any coverage manifests out of git. The release README says they contain snapshots of game code from your cartridge.
- Banks are generated from your ROM at build time and must never be committed.
