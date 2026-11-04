# Progress log

Newest first.

## 2026-11-04: kickoff

- Verified Mark's dump: AMHE, revision 0, SHA-1 90164d1ac127ee5f9815ea4ae7de798c7b5fc629 (matches upstream).
- PC baseline: upstream v0.7.12-alpha on the Legion Go (Ryzen Z1 Extreme). Mostly 60 fps; emulation 4 to 10 ms per frame (6 to 9 typical). Two dips (about 30 fps at 15:06:11, 19 fps at 15:08:19) lined up with interpreter (tier3) bursts. Audio underruns at both.
- Source review (docs/source-review.md): the Windows build runs game code through dynarmic, which is enabled only on Windows x64. Switch starts on the portable path (banks plus interpreter), like the Thor port, which measured about 14 ms of CPU work per frame on a Snapdragon 8 Gen 2. Rough Switch estimate before optimization: 14 to 18 fps (unverified).
- Next: Build 1, a boot test on the portable path, to get the first real Switch CPU number.
