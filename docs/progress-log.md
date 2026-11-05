# Progress log

Newest first.

## 2026-11-04: switch-test3 results, switch-test4

- test3: touch still did nothing; A passes "touch to start", B backs out, no button selects Adventure/Multiplayer (touch-only on DS). Logs had no input detail.
- Correction to the test2 diagnosis: SDL's Switch driver already defaults touch-to-mouse off, so touches never became mouse clicks; they were SDL finger events nobody used. SDL only polls the touchscreen while a window has focus and text input is inactive, and drops touches silently otherwise (likely cause, unconfirmed).
- switch-test4: nx_platform reads the touchscreen and pad directly through HID each frame (mphnx_input_poll); the frontend applies that touch after every other touch writer, holds quick taps 3 frames, and logs each touch down/up and any touch outside the bottom screen. SDL finger events are now only counted and logged, to show whether SDL delivers them at all. Hold ZL for a touch cursor (left stick moves, A taps); while it is on, buttons, stick and Prime pad input do not reach the game.
- mph-nx.nro sha256 6b83c7e25552cb15220ca24a0e37f5bb575613a9688cc0b7b74746f1b04bef59, verified on the PC.

## 2026-11-04: switch-test3 delivered

- mph-nx.nro sha256 ed71c1fd71203fcbb3446553a36e32b5d8224162c9ae026c27ba7b2fadd9d597, verified on the PC in C:\MPH-nx\out\sd\switch\mph-nx.
- Test goal: touch the bottom screen in handheld mode to get past the menus into Adventure, then play a few minutes. Speed and choppy audio expected unchanged; docked mode still has no touch.

## 2026-11-04: switch-test2 results, switch-test3 prep

- Picture works: renderer opengles2, output 1280x720, stacked screens visible. Snapshot at frame 1800 shows the Adventure / Multiplayer / Options menu drawn correctly. Prime Controls enabled ("melonPrimeDS bindings enabled"; pad aim engaged when sticks used).
- Blocker: touching the Switch screen did nothing, so the menu could not be passed. Cause (from frontend.cpp): SDL turns touches into mouse clicks, and with relative-mouse / Prime Controls a primary click is routed to aim capture or fire instead of the stylus.
- Speed: menus about 28 to 30 fps (CPU 30 to 34 ms/frame); some scenes 84 ms (11 fps). Interpreter still near zero (0 to 2 instructions per 2 s). Each snapshot stalled a frame by 60 to 100 ms. Audio choppy because the emulated DS produces audio at game speed, so below 60 fps the queue underruns.
- switch-test3 changes: Switch fingers mapped straight onto the DS bottom screen (SDL_RenderWindowToLogical, same hold-two-frames release rule as mouse taps); SDL_HINT_TOUCH_MOUSE_EVENTS=0 on Switch; snapshots only when /switch/mph-nx/snapshots.on exists.

## 2026-11-04: switch-test2 (display fix)

- Changes: Switch window created at 1280x720 and never resized; no integer scale; renderer name and output size logged; BMP snapshots at frames 120, 600, 1800, 3600 in /switch/mph-nx/logs; --relative-mouse-touch on (enables Prime controls).
- build.sh now fails if the NRO is not newer than the run start (the first test2 attempt silently skipped compiling when Docker had stopped).
- mph-nx.nro sha256 60a0e8e2eed9beb3cbf6b4feed8e55da78332e88432f7c198ff53a130d5fc726, verified on the PC. Delivered with SendUserFile plus device_commit_files (shell rejoin of 18 MB parts slowed to about 250 KB/s).

## 2026-11-04: switch-test1 results

- Ran without crashing; audio played (choppy); button presses were recognized (intro skipped to the title screen). TV showed black the whole time.
- Diagnostics (23 samples, about 46 s): 1,012 frames presented, so the renderer was presenting. Interpreter (tier3) ran zero ARM9 and zero ARM7 instructions: the recovered banks covered everything this session ran.
- CPU time per frame (emulation only, CPU at 1785 MHz): intro movie 56 to 87 ms (11 to 18 fps), title screen steady about 35.5 ms (26 fps), one light stretch 14 to 19 ms (about 50 fps). Presentation about 2 ms; 2D render 2 to 5 ms on worker threads. Audio underruns about 64 per 2 s, as expected below full speed. The emulation thread is the bottleneck.
- Black screen cause (likely): upstream creates a DS-sized window and then switches to fullscreen desktop. SDL2's Switch driver never resizes its NWindow for fullscreen desktop, so the renderer draws for a 1920x1080 window into a smaller buffer. Clears still work, but upstream clears to RGB 20,20,20, which reads as black on a TV.
- Prime controls were off: upstream enables them only with --relative-mouse-touch on.

## 2026-11-04: switch-test1 (Build 1, boot test)

- Toolchain: ghcr.io/danfromtico/switch-dev-legacy-vulkan (devkitA64 GCC 15.2, SDL2, Mesa) in the cloud.
- Profile: portable path (no Dynarmic), SDL2, software 3D renderer, compute renderer off, HLE math and submit, FreeBIOS plus generated firmware, direct boot, networking off, widescreen off, internal resolution 1, governor off.
- Banks: 16 of 264 declared title banks build from the ROM (14 overlays, 2 ARM7 aliases, plus main ARM9/ARM7 closures). The other 248 are runtime/coverage banks made from RAM page captures that upstream keeps in a private store; they must come from our own coverage manifests (upstream tools/ingest_coverage_manifests.py). Expect more interpreter (tier3) time than the PC release.
- Retail BIOS banks stubbed (empty dispatch); FreeBIOS banks generated from third_party/freebios.
- Compile fixes (patch 0001): debug_server.cpp includes arm9_native.h unconditionally; host_info.cpp skips uname on Switch; live_overlay_platform.cpp returns early off Windows/Linux; debounced cartridge save write on Switch (ported from aabrole/ndsrecomp).
- Compat headers for libslirp (sys/uio.h, sys/un.h, net/if.h); sysconf, setsid, sigprocmask stand-ins.
- Link test with a stand-in bank passed; SDL2 Switch video (GLES2 renderer), audio and controller backends confirmed in the ELF.
- Coverage banks recovered: tools/extract_banks_from_exe.py rebuilt all 248 private runtime/coverage pages from the validation tables and COFF symbols in Mark's v0.7.12 nds_runner.exe (about 19,700 functions, zero conflicting bytes; overlay 0 cross-check: 141,967 bytes, zero mismatches with the ROM). Pages average about 21% known bytes; the recompiler finds fewer functions on partial pages (FMV runtime 1,310 vs 2,355 in the exe), so expect some interpreter time there. Private output lives outside the repo.
- Build: 264/264 title banks (MPH_ALLOW_MISSING_BANKS=OFF), 1,249 steps, about 85 minutes on 2 cores. mph-nx.nro 344 MB, sha256 d210cde48aefa34437298e3680205a48bb47eb4b006f6405a9b48f3494bac1e3. ELF text 341 MB, data 21 MB, bss 121 MB. All 266 dispatch tables present. ELF archived for crash offsets.
- Delivered to C:\MPH-nx\out\sd\switch\mph-nx\ (nro, game.toml, README.txt, ROM), sent as 20 parts of 18 MB and rejoined on the PC; sha256 verified there.

## 2026-11-04: kickoff

- Verified Mark's dump: AMHE, revision 0, SHA-1 90164d1ac127ee5f9815ea4ae7de798c7b5fc629 (matches upstream).
- PC baseline: upstream v0.7.12-alpha on the Legion Go (Ryzen Z1 Extreme). Mostly 60 fps; emulation 4 to 10 ms per frame (6 to 9 typical). Two dips (about 30 fps at 15:06:11, 19 fps at 15:08:19) lined up with interpreter (tier3) bursts. Audio underruns at both.
- Source review (docs/source-review.md): the Windows build runs game code through dynarmic, which is enabled only on Windows x64. Switch starts on the portable path (banks plus interpreter), like the Thor port, which measured about 14 ms of CPU work per frame on a Snapdragon 8 Gen 2. Rough Switch estimate before optimization: 14 to 18 fps (unverified).
- Next: Build 1, a boot test on the portable path, to get the first real Switch CPU number.
