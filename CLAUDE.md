# MPH-nx

Native Nintendo Switch homebrew port of Metroid Prime: Hunters (Nintendo DS, USA revision 0, AMHE) built on mstan's MetroidPrimeHuntersRecomp and the ndsrecomp framework.

## Who does what

- Mark writes no code. Claude writes all code, scripts and docs.
- Mark runs PC builds and tests on his PC (Lenovo Legion Go) and his Switch (unpatched V1, Atmosphere, sys-clk), and drops logs in `C:\MPH-nx\out\sd\switch\switchlogs`.
- Switch NRO builds may be made in the cloud. PC builds are Mark's, on his PC.
- Confirm with Mark before starting any build. Issues he mentions in passing go on the list for the next planned build.
- After reading test logs: present findings and the plan for the next build, then wait for his go-ahead.

## Hard rules

- No em dashes anywhere (chat, docs, commits, release notes). No decorative emoji.
- Never commit game data: ROM, saves, BIOS/firmware, generated banks, coverage manifests, diagnostics, live-shard-cache. Upstream's own README says those contain snapshots of game code from the cartridge. Run `git status` before every commit.
- Commit as `FurryLilManChild <346934328+FurryLilManChild@users.noreply.github.com>`.
- Never ship a Switch build without its renderer, sound, input or game code. Check the artifact before handing it over.
- Upstream sources stay on pinned commits. Switch changes go in `switch/patches/*.patch`, applied idempotently by the build script.
- WE pick the path. Upstream projects (ndsrecomp, MetroidPrimeHuntersRecomp, melonDS, HuntersRecomp) are resources to learn from and pick pieces from, never the design. The goal is a native static recompilation: game functions are Switch functions with real calls and returns, game memory is plain Switch memory, timing is coarse and event-driven, and only hardware registers go through a hardware layer. Pinning upstream commits is source hygiene, not a design constraint; subsystems we write live in this repo.
- Be honest about expectations: say what each test is for and what Mark should see.

## Upstream pins

- Engine: RetroPortingToolKit/ndsrecomp, release/native-hle-v0.6.11, 179ac63d9e4055e1218d61b3fd03c34a94880a6e
- Title: mstan/MetroidPrimeHuntersRecomp (pin to be recorded at first build)
- Reference: aabrole/ndsrecomp v022-golden and aabrole/HuntersRecomp (Android/ARM64 port)

## Required ROM

USA revision 0, AMHE, 64 MiB, SHA-1 90164d1ac127ee5f9815ea4ae7de798c7b5fc629. Mark's dump matches.

## Layout

- `docs/feasibility.md`: original feasibility analysis
- `docs/source-review.md`: upstream source findings and performance evidence
- `docs/progress-log.md`: newest first, one entry per session
- `switch/`: Switch platform layer, patches and build scripts
