# MPH-nx native recompilation design

Status: in progress (started 2026-11-06). Target ROM: Metroid Prime Hunters USA Rev 1
(AMHE, ROM version byte 1, SHA-1 a8679d4f8dcb445cbe946a424241ef44901b0caf).

## Why

Tests 7 to 13 ran the game on ndsrecomp's execution model: translated code, but every
function call and return goes through a dispatcher, every load and store through
region checks and write bookkeeping, and the DS clock is counted per instruction. On
the Switch the game's own translated instructions were about 15% of emulation time.
The rest was that machinery, plus an emulated second CPU (ARM7, 5 to 7 ms a frame).
Tuning inside that model gave small, hard-to-feel changes. This design replaces it.

ndsrecomp, MetroidPrimeHuntersRecomp, melonDS and HuntersRecomp stay useful as
references and as a source of hardware device code we pick from. They do not set
this design.

## Goal

Metroid Prime Hunters as a native Switch program:

1. Every game function is a Switch function. A DS call is a real call; a return is a
   real return.
2. Game memory is plain Switch memory. A load or store is a page-table lookup and a
   direct access. Only hardware register addresses go to the hardware layer.
3. Timing is coarse. Cycles are added per block, and hardware time only advances at
   block boundaries (loop back-edges, calls, returns), driven by the events the game
   waits on: scanlines, vblank, timers, interrupts.
4. The second CPU's system and sound driver is eventually replaced by native code
   (HLE). Until then it runs on the existing ARM7 path, called from the hardware
   service step.

## Pieces

### 1. ROM tools (ours)
- Read the header, ARM9 and ARM7 binaries, the 18 ARM9 overlays and the file table.
- Undo the DS "BLZ" compression on the ARM9 binary and on compressed overlays.
- Output plain images with their load addresses. Nothing extracted is committed.

### 2. Code discovery (ours)
- Recursive descent from known entries: the ARM9 entry point, the interrupt
  handler the game installs, overlay static-initializer tables, and every BL/BLX
  target.
- ARM and Thumb tracked through BX/BLX targets.
- Function-pointer tables found by scanning data for words that point at the start of
  discovered code with a plausible prologue.
- Overlays are discovered per overlay, at their own load address. Several overlays
  share addresses; each gets its own set of functions.
- Anything still unknown at runtime falls back to the interpreter and is logged, so the
  next build includes it (coverage loop: Mark plays, we regenerate).

### 3. Native code generator (ours)
- One C function per guest function: `void f_02004800(void)`.
- Guest registers are function locals. The compiler keeps them in Switch registers.
  They are written back to the context only at calls, returns and hardware-service
  points.
- Condition flags are separate locals (N, Z, C, V). The compiler drops the ones
  nothing reads.
- Branches inside a function are `goto`s.
- BL to a known function is a direct C call. The callee returns normally when its
  return target equals the return address it was called with. Anything else (a
  return to another place, a context switch) is handed back up through a redirect
  flag to a small top-level loop, which is the only dispatcher left.
- Indirect calls and jumps (BLX register, LDR PC, POP into PC with an unknown target)
  look up a hash table of entry addresses to functions, then call directly.
- Overlay calls go through a per-overlay table chosen when the game loads an overlay.
  The overlay region's pages are watched for writes; the first call after a write
  re-identifies which overlay is resident by hashing it.
- Loads and stores: page table of 4 KiB pages to host pointers for ITCM, DTCM, main
  RAM, WRAM, VRAM, palette and OAM where safe. Reads use the read table; writes use a
  write table in which pages holding compiled code are absent, so code writes take
  the slow path and invalidate. Hardware registers always take the slow path into the
  hardware layer.
- Cycles: a constant per block (instruction count with simple load/store costs),
  added at block end. At loop back-edges, calls and returns:
  `if (cycles >= deadline) service();`
- Instructions the generator cannot express (rare system instructions) call a small
  helper. CP15 wait-for-interrupt and BIOS halt calls go to `service()` until an
  interrupt.

### 4. Hardware service step (picked from ndsrecomp, wrapped by us)
- `service()` publishes the registers, runs the hardware up to the current time
  (display lines, timers, DMA, 3D engine, sound mix, card, IPC, the ARM7), then
  delivers any pending ARM9 interrupt by calling the game's interrupt handler
  natively, with banked registers saved and restored, and returns.
- Device code (2D and 3D, SPU, DMA, timers, IPC FIFO, card, RTC, touch) is reused from
  the current runner. It is already correct, and it is a small part of the frame.

### 5. ARM7
- Phase 1: unchanged existing ARM7 execution, stepped from `service()`. The ARM7
  binary is byte-identical between Rev 0 and Rev 1, so its existing banks still apply.
- Phase 2: HLE of the ARM7 sound and system driver (sound commands over IPC to a native
  mixer, touch and RTC answered natively). The ARM7 then costs close to nothing.

## Milestones

1. ROM tools and discovery report for Rev 1 (function count, coverage of the ARM9
   binary and of each overlay, unknowns).
2. Generator producing compilable C for the whole ARM9 binary; host build of the
   generated code.
3. First Switch build: native ARM9 main binary and overlays, service step, existing
   ARM7, interpreter fallback with logging. Goal: boots and plays, with a per-frame
   time breakdown in the log.
4. Coverage and correctness passes from Mark's logs.
5. ARM7 HLE.
6. Tuning (register write-back reduction, block linking, fast-path memory for more
   regions, PGO).

## Risks

- Code discovery misses (function pointers in unusual tables): covered by the
  interpreter fallback and the coverage log, at a speed cost until regenerated.
- Interrupts arrive at block boundaries instead of exact instructions. Games almost
  always tolerate this; timing-sensitive spots get targeted fixes.
- Guest code that returns somewhere other than its caller: handled by the redirect
  flag, at the cost of an unwind.
- Self-modifying or copied code (the ITCM interrupt handler, overlay loads): copied
  code with a known source gets compiled at its run address; anything else runs on the
  interpreter.
