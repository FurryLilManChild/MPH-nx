# Progress log

Newest first.

## 2026-11-09: switch-native14 (audio stretching; logbook list and Launch Ship pop-up)

- native13 (Mark): every menu-button mapping worked as intended. Two new captures: the logbook list (categories, five rows, four scroll buttons) and the Launch Ship confirm. Both added (16 captures, each recognised as itself); detection now runs in list order only, so the logbook list (more specific) wins over the category screen.
- Audio stretching (QoL item 1 in Mark's order): switch/src/nx_stretch.h is SA-nx's WSOLA stretcher (same authors). In drain_audio, after the 0.6% drift control, the mixer output goes through it; the ratio follows a short average of the queue (1x at or above 3/4 of the 2048-frame target, up to 2.5x when empty, eased). Checked on test signals: ratio 1 passes through unchanged, 1.5x and 2x stay continuous. Log: "audio: stretched N ms (peak R) underruns U" every 10 s; toggle "Audio stretch".

## 2026-11-09: switch-native13 (controller for the touch-only menus)

- Mark's L3 captures (14) and slide notes: title menus and the star map already move a highlight with the stick but nothing confirms; the landed ship menu, weapon select, logbook, options, the save/exit/enter-ship pop-ups and the scan arrows are touch only. Samus freezes while scan boxes and the enter-ship prompt are up. Wanted: stick/D-pad to move, Switch A to confirm, Switch B to go back (on his layout Switch A is morph and Switch B is jump by default).
- switch/src/nx_menunav.cpp: screens are recognised from coarse colour fingerprints of their buttons (tools/menunav/gen_menunav.py writes nx_menunav_data.h; no captured pixels are committed). On the 14 captures each screen matches only itself (best other score 0.66 against a 0.9 threshold).
- Kinds: game-highlighted screens (title, file select) get DS A on Switch A; the star map gets a Land Ship tap; port screens and pop-ups get a yellow highlight driven by stick/D-pad, A taps it, B taps Back/No. The sensitivity slider is moved by a short drag. Presses on a recognised screen are taken from the game (releases pass). Toggle "Menu buttons". Every screen change and tap is logged.

## 2026-11-09: switch-native12 (screen captures); optimization closed

- native11 (Mark): good performance at stock even at 2x. Log: stock 1020 MHz held 60 fps in game at 1x, 2x and 3x, emulation 10 to 12.5 ms a frame. The time-breakdown line did not work (about 0.3 ms reported against about 11 ms real: the 19.2 MHz system counter is too coarse for the per-region overhead subtraction); removed. Clock line kept.
- Mark closed optimization: the second-CPU-on-demand change is dropped (unclear gain, risky). Next: quality of life (docs/qol-backlog.md), starting with menus and scan prompts driven by stick/D-pad plus one select button.
- native12: L3 saves both DS screens stacked, 3x, as /switch/mph-nx/captures/cap-NNN.bmp (Mark's modded Switch cannot retrieve Album captures) so Mark can capture every menu and prompt for mockups.

## 2026-11-09: switch-native11 (fallback code native, clocks and time breakdown in the log)

- native10 confirmed by Mark: shaking gone, "backwards" 0 at every size, each 30 fps 3D frame up for exactly two refreshes. Gameplay 60 fps overclocked, emulation 5.6 to 9.5 ms a frame. Mark: release target is stock clock at 1x; higher scaler sizes may need an overclock.
- native10 still ran about 115k ARM9 instructions a second on the fallback interpreter, mostly an overlay 2 routine around 0x02118800. missroots.py over the native8g-native10 logs gave 107 new roots (main binary 2,840 to 2,915 functions).
- Second CPU on demand was mapped and moved to native12: it already runs in 512-cycle windows and is halted most of the time; its share of the frame is not known from the logs (it does about 2,500 hardware accesses a frame). native11 measures first.
- Log: real CPU/GPU clocks every 10 s; the emulation time breakdown (emu_profile, 1 round in 31, system counter ticks on the Switch) every 10 s.
- Cloud boot test: 50,487 commands identical to native8/9/10, identical final screen.

## 2026-11-09: switch-native10 (shaking: the real cause)

- Mark: shaking still present in native8g and native9 at 2x. The native8g cadence stats showed it directly: in game at 2x, 600 presents per 10 s, each 3D frame on screen for one present, and 300 steps backwards. The presented 3D image alternated between the newest frame and the previous one.
- Cause: the presenter looked up the 3D image through the slot the 2D engine composited (HiresSlot) at present time. MPH submits the next 3D frame at VBlank, before the present, and in steady pacing that frame goes into the very slot the 2D engine used (the previous slot is the one shown). So the present after a submit showed the brand-new frame, and the next one went back to the frame the 2D engine was showing: A A C B B D C C. At 1x the 3D lines are copied into the 2D frame while it is drawn, so 1x never showed it. The earlier theories (buffer reuse, present-time 2D layers, audio pacing) were real issues but not this one.
- native10: GetLine locks the image itself (buffer, scale, frame number) when it picks the frame; the presenter uses that lock. A new 3D frame is never drawn into the locked buffer, the one locked before it (the presenter may still be reading it) or the newest finished frame's buffer. The cadence "backwards" count should drop from about 300 to near 0.

## 2026-11-08/09: switch-native8b to native8g (scaler fixes, menu speed, judder), switch-native9 (Switch clock, video DMA)

- native8b: the 3x/4x shader sets were never loaded (the load loop stopped at 2x). Fixed; a set that fails to load keeps its size off.
- native8c (diagnostic): counters and snapshots showed 3x/4x rendered correctly but every other presented frame fell back to the 1x image: MPH submits each 30 fps 3D frame at VBlank, before the frame is presented, which cleared OutputWaited. native8d keeps the slot the 2D engine composited (HiresSlot); every size above 1x now shows sharp. The snapshot fence wait needed a queue flush (native8c froze).
- native8e: render-size 3D images get four rotating buffers so the presenter never reads one the next 3D frame is drawn into.
- native8f: title/menu slowdown since native8 was the VRAM copy-on-write check (12% of the emulation thread: every 2D VRAM write scanned 16 snapshots and polled every worker). Now a page-union mask exits most writes at once, and the rest read the workers once. Toggle VRAM copy-on-write. Above 1x, the HD layer surfaces are built per line from each line's latched state on the line workers (they were built at present from live state holding the next frame's VBlank updates). Toggle Per-line HD layers.
- Shaking (Mark, video): not the scaler (toggles changed nothing). The frame loop was paced by the 1024-sample audio chunks (31 ms); since native8 made frames 6 to 8 ms, frames arrived in pairs, the presenter dropped one and showed the next twice, and the 30 fps 3D stepped 1/3 refreshes instead of 2/2. native8g: display-locked pacing (a frame is handed over only after the presenter took the previous one) with audio rate control, plus cadence stats (presents, drops, how many presents each 3D frame stayed up).
- Mark: the frame loop is still upstream's DS-clocked emulation loop; the target is a native clock (Switch vsync drives the game's VBlank, sound mixed on demand, the hardware layer advancing only what the game waits on, ARM7 sound driver replaced by a native mixer).
- native9, first native-clock step: the Switch display is the clock for picture and sound (audio device opened at the rate the game makes sound at 60 Hz, rate control only trims drift). Video DMA (ARM9 DMA from main RAM/shared WRAM into palette, VRAM or OAM) is written a 16 KiB page at a time with one 2D-hardware update instead of a slow-bus write, fence check and bank lookup per halfword (16% of the emulation thread in native8e's profile); per-unit DMA timing and values unchanged. Toggle Bulk video DMA.

## 2026-11-08: switch-native7 results, switch-native8 (event-driven card and DMA, VRAM copy-on-write, 3x/4x fix)

- native7 on hardware: title and menus draw no 2D line on the main thread. In game one VRAM fence a frame still made the main thread draw 33 to 46 of 192 lines and wait 1.3 to 2.8 ms (roughly 15 to 25% of a 9 to 12 ms frame). Frames with card streaming ran 3,000 to 17,000 scheduler rounds (card fences up to 12,785 a frame) against about 1,000 without.
- 3x crashed and 4x would have: the rasteriser put each variant's work count in the Z group count (the Maxwell limit is 65535) and binning had no bound on the work buffers. Work now goes in X, the work index is 21 bits (polygon index 11), work that does not fit is dropped, and 3x/4x get the same 16-layer budget as 2x (data heap 448 MB). The crash left no report because a GPU fault ends the process from outside.
- native7 bug found on the way: a line job's view pointer was set to its snapshot and then overwritten with the live view, so queued lines read live views (VRAMCNT remaps without a fence could reach them). Fixed.
- VRAM copy-on-write: a BG/OBJ write, or an LCDC write to a bank a queued line still reads through a view latched before a remap, copies that 16 KiB page, points the queued snapshots at the copy and waits only for lines a worker is inside (each worker publishes the job it is claiming). The cloud test's VRAM drains went from 172 to 33 (the rest are the capture bank), fence wait 46 ms to 23 ms, final frame identical.
- Event-driven card: with the ARM9 owning the card, taking its IRQ and no card DMA armed, a card register read catches the card and the save chip up to the ARM9's live timestamp, so card polls no longer end the sync window. Timing is the faithful one. Speed test toggle Event-driven card.
- A running DMA keeps the long sync window (was 64-cycle rounds for the whole transfer). Toggle Long DMA slices.
- Per-round costs: DMA running state is a mask kept with the channels; the next timer overflow is cached (catch-up ticks do not move it) until a timer write or overflow.
- Logs: the 2d line gives cow pages and fallbacks per frame and the physical pages copied; the fps line gives card catch-ups.

## 2026-11-08: switch-native6 results, native6b, switch-native7 (2D off the main thread)

- native6 on hardware: gameplay emulation 7 to 10 ms a frame (native5b 10 to 14); ARM7 1.6% of the main thread (was 11%), scheduler and dispatch 6% (was 15%), idle 43%.
- native6b: port menu Speed > CPU clock (stock 1020 MHz or boost 1785 MHz; every build so far boosted the CPU only) and 3x/4x resolution (shader sets _S3/_S4, renderer buffers sized for 1024x768, data heap 352 MB).
- native7: the main thread mostly drew 2D lines because it had to wait for the workers: a drain at VBlank, a drain on every VRAMCNT remap (MPH flips banks C and D between engine B BG/OBJ and LCDC each frame) and on LCDC writes (texture uploads). Queued lines now read palette, OAM and the BG/OBJ views from snapshots taken at latch time; VBlank no longer drains; LCDC writes fence only for banks a queued line or the display/capture reads; remaps that touch only BG/OBJ rebuild views without a fence. Speed test toggle Async 2D. Cloud test: lines drawn by the main thread 172k to about 5k, fence wait 1.1 s to 0.04 s, final frame identical to native6, card commands match.
- Also: scaler layer pass gets 2 helper threads (NDS_GPU2D_ADAPTIVE_WORKERS); the GX command cache reuses its buffers and compares the raw words it hands out (native stores do not bump page generations); host test now compiles gpu2d, vram and gpu3d with the MPH-nx paths.

## 2026-11-08: switch-native5b results, switch-native6 (native ARM7)

- native5 crashed at launch: the deko3d shader code heap's block pool (64) ran out with 102 shaders (three render-size sets). native5b: pools raised to 1024; pool exhaustion now logs before it traps.
- native5b hung at boot (white screen): the ARM7 died in FreeBIOS's park loop. Turning Fast loading off boots; the native5 card change (card registers without CPU-sync fences, card status polls completed at once) is the cause. Fast loading is now off by default; loads are low priority for Mark, so it stays parked.
- Resolution scaler works on hardware (Mark): emulation time per frame about the same at 1x, 1.5x and 2x (12 to 14 ms); presenting goes from 0.14 ms to about 1.6 ms on its own thread.
- native5b gameplay profile: idle 26%, video 15%, scheduler and dispatch 15%, I/O, DMA, timers and bus 14%, ARM7 11%, ARM9 game code 3%, interpreter 0.3%.
- native6: the ARM7 program runs as native code (tools/nrecomp/gen_all7.py, switch/native/nr_runtime7.cpp). ARMv4T rules in the decoder (no BLX/CLZ/DSP/LDRD; LDR PC and LDM {pc} keep the instruction set). The startup code's autoload table gives the WRAM and main-RAM sections; roots are the old ARM7 engine's entry lists where nothing decoded yet (975 functions, 38k instructions, none failed). Own page table, coroutine, IRQ entry (handler at 0x03FFFFFC), IntrWait flags at 0x03FFFFF8, halt by SWI 06/1F and HALTCNT. BIOS calls run with IME held at 0 (the FreeBIOS dispatcher re-enables interrupts); GetSine/Pitch/VolumeTable results are memoised. Port menu: Speed > Native ARM7 (restart) to compare with the old engine.
- Generator fixes (both CPUs): a resume at an address that is also a function entry is marked with bit 31 (it was treated as a call with a stale LR); a return sets R15 and the T bit to the caller's state.
- Cloud boot test: card commands match the reference; reaches the Sylux intro; 3e9 cycles in 13.8 s against 20.0 s with the old ARM7 engine in the same build. ARM7 interpreted instructions about 11k a second.
- Build label: now a define on nx_main.cpp and frontend.cpp only (MPHNX_LABEL), so changing it recompiles two files. The old target-wide define stays frozen at switch-native5 until a full engine rebuild.

## 2026-11-07: switch-native4 results, switch-native5

- native4: best run yet; 60 fps most of the time, dips into the 40s and 50s, two drops to about 19 fps during loads. Profile of the emulation thread: game code about 2%, idle about 13%, video work about 11%, interpreter fallback about 7% (half of it coverage bookkeeping), ARM7 on the old engine about 8%, native lookup about 5%, scheduler and I/O about 13%.
- Load drops: Rev 1's card loop polls ROMCTRL bit 23 after every word; each poll cost a scheduler round (about 500k a second). native5 completes the pending card event on the poll when ARM9 PIO fast loading applies, and card registers take no fence there. (The ROM was already read into memory at boot.)
- native5: roots from the native4 miss log (tools/nrecomp/missroots.py walks mid-function misses back to the prologue, per overlay with ov=NN); resume labels after SWI, MCR and MSR; lean interpreter (no coverage or live-compiler bookkeeping, no ARM9 bank probe); direct native entry tables; 2D workers woken every 8 lines; 3D readback in CPU-cached memory with a per-frame cache invalidate; presenter on its own thread.
- Resolution scaler (port menu Display > Resolution: 1x, 1.5x, 2x): the deko3d 3D renderer renders at 384x288 or 512x384 from the sub-pixel vertex positions (one shader set per size) and downsamples for the 2D compositor; the presenter composes the top screen on the GPU (HdCompose.glsl) from the 2D engine's per-pixel top-two layers (upstream's HD frame) and the render-size 3D image. Scenes the 2D engine cannot describe stay at 1x.
- Cloud boot test: same first 6,802 card commands as the reference, then further (faster loads); reaches the Sylux intro; interpreted instructions about 15k a second (native4: 188k).

## 2026-11-07: switch-native2 and native3 results, switch-native3, switch-native4

- native2: boots on Accurate; Fastest crashed during boot (the ARM7 ran wild). Controls felt great (Mark). Speed a bit below test13; about 51% of emulation time went to a call-stack list from the old engine that the interpreter fallback filled and native code never emptied.
- native3: call-stack list cleared after each interpreted step; 182 missed routines plus the IRQ handler made native; every launch holds Accurate CPU sync for 20 s ("boot hold over" in the log), then uses the saved setting. Result: about 60 fps most of the time, large rooms still below full speed; title-menu transitions dropped to about 14 fps.
- Menu cause: fast card loading was keyed to Rev 0 PCs, so on Rev 1 every 4-byte card data read took a full scheduler round.
- native4: native card PIO path (ARM9 card reads with fast loading on get the next word at once; card DMA keeps exact timing); 2,782 static functions native with roots filtered by tools/nrecomp/rootfilter.py (a bad root after an svc broke the cloud test first); the DMA stall check runs only after DMA register writes. Cloud test matches the reference; interpreted instructions fell from 575k/s to 188k/s.

## 2026-11-07: switch-native1 results, cloud boot test, switch-native2

- native1 white screen: Thumb SWIs went to the BIOS with LR bit 0 set (the BIOS reads the SWI number at [LR-2]), and the interpreter refused to run BIOS code (no write provenance).
- New cloud boot test (switch/host): headless x86-64 build of the native engine; NR_DISABLE=1 runs the interpreter as a reference; card/DMA command trace, call trace and exit screenshot for comparison. It found and fixed: BIOS code under the interpreter, write provenance for native stores, mid-function entries and returns (redirect 2), IRQ epilogue at any dispatch point, DMA stall after I/O stores, overlay re-identification, and nr_call_dyn returning early when a native function returned into interpreted code.
- Native SWIs: CpuSet, CpuFastSet, WaitByLoop, GetCRC16, BitUnPack, LZ77, Huffman, RL, Diff. Log heartbeat flushes every second.
- Analysis roots come from the reference's real call targets (1,109); interpreted code dropped from about 2.5M to 0.6M instructions per second in the cloud test.
- Result: 45 s emulated, native matches the reference (same card commands, same end PC); the hunter intro movie renders on both screens.
- Prime controls aim and weapon switching on for Rev 1 (addresses mapped by tools/nrecomp/addrmap.py; aim and morph agree with melonPrimeDS US 1.1).

## 2026-11-06: switch-native1

- First build on the native recompilation engine (docs/native-recomp-design.md), USA Rev 1 only. ARM9 static image plus 18 overlays compiled to C (about 9,900 functions); interpreter fallback logs every miss. ARM7 still on the old banks.
- Controls deadline (Mark): controls must be worked on in switch-native2 or switch-native3 at the latest. That means porting Prime controls aim to Rev 1 addresses, then the twin-stick, touch and button mapping and the remap menu.

## 2026-11-06: switch-test11 and test12 results, switch-test13

- test11 hung on a white screen after loading at Fastest and showed flipped screens in menus. Flip: steady 3D pacing showed the previous 3D frame, which in dual-screen 3D menus belongs to the other screen (fixed in test12 by matching the POWCNT1 swap bit). The hang persisted in test12 with fast writes, return chaining and run-through-I/O all off; Mark found Accurate (64) boots, then Fastest is fine in play. The remaining Fastest-only change was removing the 3D status fence, which also gave no measurable speed. Hallway about 0.85x, unchanged.
- test12 heavy stretches: up to 13,500 card-register and 4,900 card-data-port fences per frame. The emulation thread waits about 2% on the 3D GPU fence.
- test13: 3D status fence back on by default (new settings key); card register and card data port fences on a live "Card sync stop" toggle; Switch log stays fully buffered (upstream main.cpp set stderr unbuffered); the debug-server bus event ring is skipped on every slow access.

## 2026-11-06: switch-test10 results, switch-test11

- test10: deko3d presenter works (present 0.5 ms per frame). The saved CPU sync was still Accurate (64) from test8, so test9 and the first test10 run never opened a wide window. With Fastest (512) the hallway went from 0.73x to about 0.85x, but 30 to 97% of windows were still ended by I/O fences (up to 22,000 rounds per frame in a heavy stretch). Profile: game code about 20% of emulation time; dispatcher 16%, scheduler 12.5%, bus 12.5%, per-store code-write bookkeeping 7%. About 28% of frames showed the previous 3D frame.
- test11: no fence on 3D status/matrix reads (nds_gpu3d_read already syncs the geometry engine, as melonDS does); per-class fence counters; CPU sync saved under a new key so the default Fastest (512) applies once; fast writes in flat bodies (DTCM and main-RAM 64-byte chunks that hold no validated code skip generation bumps and invalidation, provenance marks kept; chunks are marked before any cache entry or static guard captures a page generation); return chaining (out-of-region transfers in flat regions tail-call the target through the dispatcher's fast slot; fast slots are dropped on any dispatch epoch change); run through I/O (a device access that changed neither cycles nor the slice limit no longer ends the region); steady 3D pacing (always show the previous finished frame). All five are live toggles under Speed tests.

## 2026-11-06: switch-test9 results, switch-test10

- test9 (flat + deko3d + block linking): median 28.7 fps, about +30% over test8. Profile by category on the emulation thread: game code 10%, dispatch 22%, mesa/GL/kernel 22% (SDL texture upload allocates a new GPU buffer per frame), scheduler 13%, memory helpers 10%. The ARM9 is guest-halted at 89% of round starts; rounds are about 3,500 to 4,500 per frame, set by device deadlines (SPU 1024 cycles, RTC 1022, LCD) and by every hardware divide ending the slice.
- test10: fast scheduler (instant divider/sqrt, SPU and RTC catch up without splitting rounds, ARM9 I/O fences only on registers another agent can change, ARM7-leads rounds while the ARM9 sleeps, stopping at the first ARM9 interrupt); deko3d presenter replaces SDL renderer and mesa on the frame path (2D-engine blits from CPU pitch-linear images, CPU-drawn cursor and HUD); 3D readback shows the previous finished frame instead of stalling; upstream sampling diagnostics compiled armed-only, perf JSON and coverage manifest off by default. Speed log adds emu/present ms and rounds, ARM7-lead rounds and divides per frame.

## 2026-11-06: switch-test8 results, switch-test9

- test8 gameplay medians: accurate+software 22.0 fps, flat+software 20.4, flat+deko3d 22.3. deko3d renders correctly on hardware and removes the 3D wait (6-9% to 0.1%). Flat code halves game-code cost but dispatch rises to 21% and unadmitted MSR/MRS fall to the interpreter. CPU sync windows are cut by I/O fences; no-fence and small dispatch cache gave nothing.
- test9: flat regions tail-call each other directly (3,462 musttail link sites, guarded by a per-page written-since-validation bit); dispatcher fast slots for flat-model targets checked by page generation; MSR/MRS admitted in flat regions (mode changes still single-step); live Block linking toggle and fast-slot counters.

## 2026-11-06: switch-test7 results, switch-test8

- test7: startup about 2 s (from about 10); card reads instant inside MPH's PIO loop; CPU sync windows had no effect (about 3,500 scheduler rounds per frame on every setting).
- Research: melonDS for Switch (RSDuck, release 6) gets its speed from an ARM64 JIT (block cycle sums, register cache, fastmem, block linking) plus deko3d 2D/3D. DrasticDS_nx keeps DraStic's software 3D and presents through deko3d, Vulkan or OpenGL.
- test8: lean ARM9 bank bodies and -fPIE banks; upstream's portable flat-AOT generator applied to MPH's ARM9 binary (mph_perf_arm9, 22,503 functions), selectable at launch (--execution-model performance); RSDuck's deko3d compute 3D renderer ported to the object-based melonDS API with a CPU-readable output buffer for the software 2D compositor, selectable at launch; CPU sync window counters and a no-fence test setting; live dispatch cache size; buffered diagnostics; per-launch log names.

## 2026-11-05: switch-test6 results, switch-test7

- test6 profile (2 ms host samples of the emulation thread): about 25 to 30% generated guest code, about 20% dispatcher, 9% scheduler and timers, 7% memory bus, 5% I/O and DMA, 5% graphics emulation plus 4% waiting on the 3D thread, 3% GL texture upload in mesa.
- Startup: the stream-iterator ROM read took about 8 s and the SHA-1 about 1 s.
- Loads: the plateaus at 11 fps are MPH's card PIO loop (0x020919EC). Fast loading v1 left them unchanged because each card word still cost a scheduler round.
- Both quits crashed after "runner returned 0", during static teardown.
- test7: CPU sync window option (64/128/256/512 system cycles, reusing upstream's performance-window fences, geometry ports exempt, GX FIFO stall ends the slice), fast loading v2 (instant words inside the PIO loop, default on), bulk ROM read with a cached SHA-1, fully buffered logs flushed each second, _exit after shutdown, per-launch profile files and a timed speed line every 10 s.

## 2026-11-05: switch-test5 results, switch-test6

- test5 (Mark): save recovered and loaded; + pauses; ZL scans (door opened); remapping works; layouts and touch work; settings persist across builds.
- A vs B at the same spots: 17 to 19 fps and 22 to 25 fps in both. The -fPIE + hidden visibility change (GOT loads removed) gained nothing measurable; dropped. Lesson: one cached load per global is not where the time goes.
- Pokemon DS Widescreen (mamesj, GPL-3.0) runs melonDS with its FAST JIT and deko3d on Switch at 1785 MHz; not a native port. Takeaways: a JIT that keeps guest registers in host registers is the speed reference; deko3d is the expected renderer (Mark: if we skip deko3d, the performance must justify it).
- switch-test6: emulation-thread profiler (svcSetThreadActivity pause + svcGetThreadContext3, 500 Hz, PC plus 6 frame records, split by guest PC into card loading and the rest) writing logs/profile.txt every 30 s; "Fast loading" menu option (default off): at MPH's NitroSDK card wait loops (0x020919EC, 0x02091D10, 0x0209194C), a ROMCTRL read while busy and not ready advances the ARM9 clock to the published fast limit (the live slice cap, never past the next event or ARM7 rendezvous) instead of spinning.
- mph-nx.nro sha256 f6786fb6d7fa7529e2f04c8e08700733abcc03a8dd411945afeee6eb99424c99, verified on the PC in out\sd-test6.

## 2026-11-05: switch-test5 (A and B)

- Saves: Switch branch in battery_save.cpp moves the old file to .bak, renames the new one in, then deletes .bak (FAT refuses a rename onto an existing file; this rule was already in switch-port-base and was missed). Writes are skipped when bytes are unchanged; a failed flush waits 10 s instead of retrying every frame. nx_main moves a leftover mph.sav.tmp into place on start (old file kept as mph.sav.old).
- Input: + is DS Start (upstream used it to open its menu, so the game could never pause). Tap minus toggles the touch cursor (left stick, A taps) and, in Top only, the bottom-screen overlay. Hold minus 0.5 s opens the MPH-nx menu. ZL is scan/fire again. Menu uses Switch A to accept, B to go back.
- Layouts (window pixels, logical size off on Switch): Stacked (default, 480x360 each), Big top + small bottom (960x720 + 320x240 bottom right), Top only (960x720; minus shows the bottom screen at 853x640 over a dimmed top). Touch maps through the drawn bottom rect.
- MPH-nx menu (upstream runtime UI with our items): screen layout, show speed (FPS and speed vs 59.83 Hz), remapping of jump, morph, boost, visor, fire, scan/fire, beam, missile, confirm, pause; reset; resume; quit. Saved to /switch/mph-nx/mph-nx-settings.ini.
- Speed data from switch-test4 (corrected): gameplay 14 to 16 fps (35 to 45 ms emu per frame, flat profile); loading about 9 fps with about 75% of dispatches in the NitroSDK card PIO loop at 0x020919EC (ROMCTRL bit 23/31 polling, 0xB7 reads). Generated code is faithful-v1: guest registers in g_cpu, per-instruction yield check, instruction count, hook check, cycle accounting.
- Build B (MPHNX_PIE_LIBS=ON): static libraries -fPIE, banks also -include compat/mphnx_hidden.h (visibility hidden). First B attempt with -fPIE alone produced identical code (AArch64 GCC still uses the GOT for extern data); caught by disassembly before delivery. Final B addresses globals with adrp+add, no GOT load; NRO 322 MB vs 344 MB.
- Card PIO shortcut deferred: upstream's MKDS version (hle_mkds_card_pio.cpp) is tied to MKDS addresses and the native performance model; needs a careful MPH port.
- A sha256 9d5eed55eab12608ccb6de92f005496de560eb03605173466df5271dc7406c21, B sha256 d0bf5eea7d055f341ab2c98480da81b70ece8c6a9678677c20271abee703df91, both verified on the PC in out\sd-test5.

## 2026-11-04: switch-test4 results

- Touch works (22 finger taps mapped correctly) and the ZL cursor works. SDL did deliver finger events, so the test3 "SDL drops touches" theory was wrong; the test3 cause is unknown and moot now.
- Save bug: every flush failed ("cannot replace save file: File exists", 3,731 times). upstream battery_save.cpp writes mph.sav.tmp then rename()s over mph.sav; Horizon's rename does not replace an existing file. The newest save is likely in mph.sav.tmp. The failing flush also retried every 500 ms, rewriting 256 KB each time.
- Controls (Mark): B jump, A morph ball, X confirm in most menus, R3 visor, RT fire. ZL scan did not work because the cursor took ZL, which blocks progress at scan-locked doors. Bottom-screen icon taps work.
- Speed in game: about 9 fps, 83 ms per frame, of which ARM9 recompiled code is 70 ms; ARM7 3 ms, timers/devices a few ms each, present 2 ms. Banks are already -O2. Speed needs hot-routine profiling first.
- Mark wants soon: an in-game menu with button remapping and screen layout choices, and performance work. The upstream + menu only has mouse sensitivity, frame interpolation and quit.
- Proposed next: test5 (save fix plus recovery of mph.sav.tmp, cursor moved to a minus toggle), test6 (guest hot-routine sampler), then the MPH-nx menu.

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
