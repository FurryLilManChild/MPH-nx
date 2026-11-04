# Progress log

Newest first.

## 2026-11-04: switch-test1 (Build 1, boot test)

- Toolchain: ghcr.io/danfromtico/switch-dev-legacy-vulkan (devkitA64 GCC 15.2, SDL2, Mesa) in the cloud.
- Profile: portable path (no Dynarmic), SDL2, software 3D renderer, compute renderer off, HLE math and submit, FreeBIOS plus generated firmware, direct boot, networking off, widescreen off, internal resolution 1, governor off.
- Banks: 16 of 264 declared title banks build from the ROM (14 overlays, 2 ARM7 aliases, plus main ARM9/ARM7 closures). The other 248 are runtime/coverage banks made from RAM page captures that upstream keeps in a private store; they must come from our own coverage manifests (upstream tools/ingest_coverage_manifests.py). Expect more interpreter (tier3) time than the PC release.
- Retail BIOS banks stubbed (empty dispatch); FreeBIOS banks generated from third_party/freebios.
- Compile fixes (patch 0001): debug_server.cpp includes arm9_native.h unconditionally; host_info.cpp skips uname on Switch; live_overlay_platform.cpp returns early off Windows/Linux; debounced cartridge save write on Switch (ported from aabrole/ndsrecomp).
- Compat headers for libslirp (sys/uio.h, sys/un.h, net/if.h); sysconf, setsid, sigprocmask stand-ins.
- Link test with a stand-in bank passed; SDL2 Switch video (GLES2 renderer), audio and controller backends confirmed in the ELF.

## 2026-11-04: kickoff

- Verified Mark's dump: AMHE, revision 0, SHA-1 90164d1ac127ee5f9815ea4ae7de798c7b5fc629 (matches upstream).
- PC baseline: upstream v0.7.12-alpha on the Legion Go (Ryzen Z1 Extreme). Mostly 60 fps; emulation 4 to 10 ms per frame (6 to 9 typical). Two dips (about 30 fps at 15:06:11, 19 fps at 15:08:19) lined up with interpreter (tier3) bursts. Audio underruns at both.
- Source review (docs/source-review.md): the Windows build runs game code through dynarmic, which is enabled only on Windows x64. Switch starts on the portable path (banks plus interpreter), like the Thor port, which measured about 14 ms of CPU work per frame on a Snapdragon 8 Gen 2. Rough Switch estimate before optimization: 14 to 18 fps (unverified).
- Next: Build 1, a boot test on the portable path, to get the first real Switch CPU number.
