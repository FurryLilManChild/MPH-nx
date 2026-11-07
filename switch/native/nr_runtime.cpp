// MPH-nx native recompilation runtime.
//
// The native ARM9 program runs on its own stack (a coroutine on the emulation
// thread). The existing scheduler stays in charge of time: its ARM9 slice
// resumes the coroutine, and the native code yields back when the slice ends,
// the CPU halts or a DMA takes the bus. The ARM7, the devices and the frontend
// are unchanged.
#include "nr_runtime.h"
#include "nr_tables.h"
#include "nr_coro.h"
#include "runtime_arm.h"
#include "state.h"
#include "io.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <atomic>
#include <chrono>
#include <thread>

extern "C" {
uint32_t nr_redirect = 0;
uint32_t g_nr_q = 0;
uintptr_t nr_rd_tab[1u << 20];
uintptr_t nr_wr_tab[1u << 20];
unsigned long long g_nr_stats[16];  // see NR_STAT_* below
char* nr_stack_limit = nullptr;
void nr_stack_overflow(void) {
    std::fprintf(stderr, "[native] host stack exhausted (guest recursion too deep) at %08X\n", g_cpu.R[15]);
    nds_halt("native: host stack exhausted");
    nr_stack_limit = nullptr;   // let the unwind finish
}
uint32_t nds_exception_base(void);
bool runtime_should_yield_slow(void);
void runtime_msr_cpsr(uint32_t value, uint32_t mask);
uint32_t runtime_mrs_spsr(void);
void runtime_msr_spsr(uint32_t value, uint32_t mask);
void runtime_coproc_write(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
uint32_t runtime_coproc_read(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
void runtime_clear_fast_limit(void);
extern void (*g_nx_arm9_native_hook)(void);
extern void (*g_nx_remap_hook)(void);
}
bool g_nx_native_build = false;
void tier3_step_one(uint32_t pc);
uint32_t nds_irq_pending(int cpu);
bool nds_cpu_halted(int cpu);
void nds_cpu_enter_halt(int cpu);

enum { NR_STAT_SERVICE, NR_STAT_YIELD, NR_STAT_IRQ, NR_STAT_DYN, NR_STAT_DYN_MISS,
       NR_STAT_INTERP, NR_STAT_SWI_HLE, NR_STAT_SWI_BIOS, NR_STAT_REDIRECT, NR_STAT_OVSWITCH,
       NR_STAT_IO };

// ---------------------------------------------------------------------------
// Overlays: which compiled overlay is resident at each load address
// ---------------------------------------------------------------------------
static uint32_t g_ov_lo = 0xFFFFFFFFu, g_ov_hi = 0;
static int g_ov_resident[64];          // per overlay: 1 resident, 0 not, -1 unknown
static uint32_t g_ov_dirty = 1;
// Bus-side RAM writes (DMA, card transfers, slow-path stores) bump this; the
// native fast path does not, so a change means something else wrote RAM.
extern uint64_t g_coverage_write_epoch;
static uint64_t g_ov_epoch = ~0ull;

static void ov_bounds() {
    for (uint32_t i = 0; i < nr_overlay_count; ++i) {
        g_ov_lo = std::min(g_ov_lo, nr_overlays[i].ram);
        g_ov_hi = std::max(g_ov_hi, nr_overlays[i].ram + nr_overlays[i].size);
    }
}

static bool ov_present(const NrOverlay& o) {
    for (uint32_t k = 0; k < 64; k += 4) {
        const uint32_t a = o.ram + k;
        const uintptr_t b = nr_rd_tab[a >> 12];
        uint32_t v;
        if (!b) return false;
        memcpy(&v, (const void*)(b + (a & 0xFFFu)), 4);
        uint32_t e; memcpy(&e, o.head + k, 4);
        if (v != e) return false;
    }
    return true;
}

static void ov_identify() {
    for (uint32_t i = 0; i < nr_overlay_count; ++i)
        g_ov_resident[i] = ov_present(nr_overlays[i]) ? 1 : 0;
    g_ov_dirty = 0;
    g_ov_epoch = g_coverage_write_epoch;
    ++g_nr_stats[NR_STAT_OVSWITCH];
}

static const NrEntry* find(const NrEntry* tab, uint32_t n, uint32_t key) {
    const NrEntry* e = tab + n;
    const NrEntry* it = std::lower_bound(tab, e, key,
        [](const NrEntry& x, uint32_t k) { return x.key < k; });
    return (it != e && it->key == key) ? it : nullptr;
}

// key = address | thumb bit
static NrFn nr_lookup(uint32_t key) {
    const uint32_t a = key & ~1u;
    if (a >= g_ov_lo && a < g_ov_hi) {
        if (g_ov_dirty || g_ov_epoch != g_coverage_write_epoch) ov_identify();
        for (uint32_t i = 0; i < nr_overlay_count; ++i) {
            const NrOverlay& o = nr_overlays[i];
            if (g_ov_resident[i] == 1 && a >= o.ram && a < o.ram + o.size) {
                if (const NrEntry* e = find(o.tab, o.count, key)) return e->fn;
            }
        }
        return nullptr;
    }
    if (const NrEntry* e = find(nr_tab_static, nr_tab_static_count, key)) return e->fn;
    return nullptr;
}

// ---------------------------------------------------------------------------
// Memory map
// ---------------------------------------------------------------------------
static void map_range(uint32_t lo, uint32_t hi, const NdsBusFastWin& w, bool writable) {
    if (!w.data) return;
    for (uint64_t a = lo; a < hi; a += 4096u) {
        const uint32_t pg = (uint32_t)(a >> 12);
        const uintptr_t host = (uintptr_t)(w.data + ((uint32_t)a & w.mask & ~0xFFFu));
        nr_rd_tab[pg] = host;
        nr_wr_tab[pg] = writable ? host : 0;
    }
}

static bool g_arm9_started_flag = false;
extern "C" void nr_remap(void) {
    memset(nr_rd_tab, 0, sizeof nr_rd_tab);
    memset(nr_wr_tab, 0, sizeof nr_wr_tab);
    // Order matters: later ranges override earlier ones, as in resolve().
    map_range(0x02000000u, 0x03000000u, g_busf_main, true);
    map_range(0x03000000u, 0x03800000u, g_busf_wram_lo[NDS_ARM9], true);
    map_range(0x03800000u, 0x04000000u, g_busf_wram_hi[NDS_ARM9], true);
    if (g_busf_itcm_limit) map_range(0, g_busf_itcm_limit, g_busf_itcm, true);
    if (g_busf_dtcm_limit)
        map_range(g_busf_dtcm_base, g_busf_dtcm_base + g_busf_dtcm_limit, g_busf_dtcm, true);
    // Overlay load addresses: stores go through the slow path, which marks the
    // resident overlay set stale (an overlay load is a store into this range).
    for (uint32_t a = g_ov_lo & ~0xFFFu; a < g_ov_hi; a += 4096u)
        if (a - 0x02000000u < 0x01000000u) nr_wr_tab[a >> 12] = 0;
    g_ov_dirty = 1;
}

extern "C" uint32_t nr_io_rd32(uint32_t a) { ++g_nr_stats[NR_STAT_IO]; return bus_read_u32_slow(a); }
extern "C" uint32_t nr_io_rd16(uint32_t a) { ++g_nr_stats[NR_STAT_IO]; return bus_read_u16_slow(a); }
extern "C" uint32_t nr_io_rd8(uint32_t a) { ++g_nr_stats[NR_STAT_IO]; return bus_read_u8_slow(a); }
static inline void note_store(uint32_t a) { if (a >= g_ov_lo && a < g_ov_hi) g_ov_dirty = 1; }
extern "C" void nr_io_wr32(uint32_t a, uint32_t v) { ++g_nr_stats[NR_STAT_IO]; note_store(a); bus_write_u32_slow(a, v); }
extern "C" void nr_io_wr16(uint32_t a, uint32_t v) { ++g_nr_stats[NR_STAT_IO]; note_store(a); bus_write_u16_slow(a, (uint16_t)v); }
extern "C" void nr_io_wr8(uint32_t a, uint32_t v) { ++g_nr_stats[NR_STAT_IO]; note_store(a); bus_write_u8_slow(a, (uint8_t)v); }

// ---------------------------------------------------------------------------
// Coroutine: the native ARM9 runs on its own stack
// ---------------------------------------------------------------------------
static volatile uint32_t g_nr_last_pc = 0;
static volatile uint32_t g_nr_last_swi = 0xFFu;
static NrCoro g_sched_ctx, g_arm9_ctx;
static std::vector<uint8_t> g_arm9_stack;
static bool& g_arm9_started = g_arm9_started_flag;
static uint32_t g_boot_pc = 0;

static void nr_yield() {
    ++g_nr_stats[NR_STAT_YIELD];
    nr_coro_switch(&g_arm9_ctx, &g_sched_ctx);
}

static void nr_take_irq(uint32_t resume);

// Service point: the native code reached its fast limit (slice end, device
// deadline, or a state change asking for a re-poll). R15 holds the resume PC.
extern "C" void nr_service(void) {
    ++g_nr_stats[NR_STAT_SERVICE];
    const uint32_t resume = g_cpu.R[15];
    g_nr_last_pc = resume | ((g_cpu.cpsr & CPSR_T_BIT) ? 1u : 0u);
    if (runtime_should_yield_slow()) {
        // Slice over, CPU halted, DMA owns the bus, or terminal: back to the
        // scheduler. It resumes us at the next ARM9 slice.
        nr_yield();
        g_nds_unwinding = 0;
    }
    if (!(g_cpu.cpsr & CPSR_I_BIT) && nds_irq_pending(0))
        nr_take_irq(resume);
}

static void nr_run(uint32_t pc);
static void coro_main(void*) {
    nr_run(g_boot_pc);
    for (;;) nr_yield();   // never reached: nr_run does not return
}

// Called from the scheduler's ARM9 slice (run_slice) instead of the dispatcher.
extern "C" void nr_resume_arm9(void) {
    if (!g_arm9_started) {
        g_arm9_started = true;
        ov_bounds();
        nr_remap();
        g_boot_pc = g_cpu.R[15] | ((g_cpu.cpsr & CPSR_T_BIT) ? 1u : 0u);
        g_arm9_stack.resize(16u << 20);
        nr_stack_limit = (char*)g_arm9_stack.data() + (256u << 10);
        nr_coro_init(&g_arm9_ctx, g_arm9_stack.data(), g_arm9_stack.size(), coro_main, nullptr);
        std::fprintf(stderr, "[native] ARM9 native start at %08X (%u static entries, %u overlays)\n",
                     g_boot_pc, nr_tab_static_count, nr_overlay_count);
    }
    nr_coro_switch(&g_sched_ctx, &g_arm9_ctx);
}

// ---------------------------------------------------------------------------
// Interpreter fallback for code with no native entry
// ---------------------------------------------------------------------------
// Coverage log: the first few hundred distinct addresses that ran on the
// interpreter. The next build adds them as native entry points.
static void note_miss(uint32_t key) {
    static uint32_t seen[512]; static uint32_t n = 0;
    for (uint32_t i = 0; i < n; ++i) if (seen[i] == key) return;
    if (n < 512) seen[n++] = key;
    if (n <= 300) std::fprintf(stderr, "[native] no native code at %08X%s\n", key & ~1u, (key & 1) ? " (thumb)" : "");
}

static void interp_step() {
    ++g_nr_stats[NR_STAT_INTERP];
    tier3_step_one(g_cpu.R[15]);
    if (g_runtime_cycles >= g_nds_fast_limit) nr_service();
}

static inline uint32_t cur_key() {
    return g_cpu.R[15] | ((g_cpu.cpsr & CPSR_T_BIT) ? 1u : 0u);
}

// Top-level loop: the only dispatcher. Entered at boot and after every
// redirect (a return to somewhere other than the caller, a context switch).
static void nr_run(uint32_t key) {
    g_cpu.R[15] = key & ~1u;
    for (;;) {
        nr_redirect = 0;
        const uint32_t k = cur_key();
        if (NrFn fn = nr_lookup(k)) {
            fn(k & ~1u);
            if (nr_redirect) ++g_nr_stats[NR_STAT_REDIRECT];
        } else {
            note_miss(k);
            interp_step();
        }
    }
}

// Indirect call (BLX reg, function pointer, call into an overlay). LR is set.
extern "C" void nr_call_dyn(uint32_t target) {
    ++g_nr_stats[NR_STAT_DYN];
    const uint32_t ret = g_cpu.R[14];
    if (target & 1) g_cpu.cpsr |= CPSR_T_BIT; else g_cpu.cpsr &= ~CPSR_T_BIT;
    g_cpu.R[15] = target & ~1u;
    if (NrFn fn = nr_lookup(target)) { fn(0); return; }
    ++g_nr_stats[NR_STAT_DYN_MISS];
    note_miss(target);
    // Interpret until the callee returns to `ret`; hand back to native code
    // wherever a native entry appears.
    for (;;) {
        if (g_cpu.R[15] == (ret & ~1u)) return;
        const uint32_t k = cur_key();
        if (NrFn fn = nr_lookup(k)) {
            fn(k & ~1u);
            if (nr_redirect) {
                if (g_cpu.R[15] == (ret & ~1u)) { nr_redirect = 0; return; }
                return;   // a real redirect: unwind to the top loop
            }
            continue;
        }
        interp_step();
    }
}

// ---------------------------------------------------------------------------
// Exceptions and system instructions
// ---------------------------------------------------------------------------
static uint32_t dtcm_end() {
    return (runtime_coproc_read(15, 0, 9, 1, 0) & 0xFFFFF000u) + 0x4000u;
}

static inline uint32_t rd32(uint32_t a) {
    const uintptr_t b = nr_rd_tab[a >> 12];
    if (b) { uint32_t v; memcpy(&v, (const void*)(b + (a & 0xFFCu)), 4); return v; }
    return bus_read_u32_slow(a & ~3u);
}
static inline void wr32(uint32_t a, uint32_t v) {
    const uintptr_t b = nr_wr_tab[a >> 12];
    if (b) { memcpy((void*)(b + (a & 0xFFCu)), &v, 4); return; }
    bus_write_u32_slow(a & ~3u, v);
}

// IRQ entry as the hardware plus the BIOS IRQ vector do it, then the game's
// handler from [DTCM end - 4] as a native call. A handler that switches thread
// leaves a different return PC: that becomes a redirect.
static void nr_take_irq(uint32_t resume) {
    ++g_nr_stats[NR_STAT_IRQ];
    const uint32_t old = g_cpu.cpsr;
    const uint32_t ret_thumb = (old & CPSR_T_BIT) ? 1u : 0u;
    runtime_msr_cpsr((old & ~0x3Fu) | 0x92u, 0x1);         // IRQ mode, I set, ARM
    g_cpu.cpsr = (g_cpu.cpsr & ~CPSR_T_BIT) | CPSR_I_BIT;
    runtime_msr_spsr(old, 0xF);
    g_cpu.R[14] = resume + 4u;
    // BIOS: STMFD sp!, {r0-r3, r12, lr}
    uint32_t sp = g_cpu.R[13] - 24u;
    const uint32_t saved[6] = {g_cpu.R[0], g_cpu.R[1], g_cpu.R[2], g_cpu.R[3], g_cpu.R[12], g_cpu.R[14]};
    for (int i = 0; i < 6; ++i) wr32(sp + 4u * i, saved[i]);
    g_cpu.R[13] = sp;
    g_cpu.R[0] = dtcm_end();
    const uint32_t handler = rd32(g_cpu.R[0] - 4u);
    const uint32_t bios_ret = 0xFFFF0000u + 0x0128u;   // a BIOS address the handler returns to
    g_cpu.R[14] = bios_ret;
    const uint32_t keep_redirect = nr_redirect;
    nr_redirect = 0;
    nr_call_dyn(handler);
    if (nr_redirect && g_cpu.R[15] != bios_ret) {
        // The handler did not come back through the BIOS: leave it to the top loop.
        return;
    }
    nr_redirect = keep_redirect;
    // BIOS: LDMFD sp!, {r0-r3, r12, lr}; SUBS pc, lr, #4
    sp = g_cpu.R[13];
    g_cpu.R[0] = rd32(sp); g_cpu.R[1] = rd32(sp + 4); g_cpu.R[2] = rd32(sp + 8);
    g_cpu.R[3] = rd32(sp + 12); g_cpu.R[12] = rd32(sp + 16); g_cpu.R[14] = rd32(sp + 20);
    g_cpu.R[13] = sp + 24u;
    const uint32_t lr = g_cpu.R[14];
    const uint32_t spsr = runtime_mrs_spsr();
    runtime_msr_cpsr(spsr, 0xF);
    g_cpu.cpsr = spsr;
    const uint32_t back = lr - 4u;
    g_cpu.R[15] = back;
    if (back != resume || ((spsr & CPSR_T_BIT) ? 1u : 0u) != ret_thumb) nr_redirect = 1;
}

extern "C" void nr_msr_cpsr(uint32_t v, uint32_t mask) {
    runtime_msr_cpsr(v, mask);
    g_nr_q = (g_cpu.cpsr >> 27) & 1u;
    if (!(g_cpu.cpsr & CPSR_I_BIT) && nds_irq_pending(0)) runtime_clear_fast_limit();
}
extern "C" uint32_t nr_get_spsr(void) { return runtime_mrs_spsr(); }
extern "C" void nr_set_spsr(uint32_t v, uint32_t mask) { runtime_msr_spsr(v, mask); }

extern "C" void nr_exception_return(uint32_t pc) {
    const uint32_t spsr = runtime_mrs_spsr();
    runtime_msr_cpsr(spsr, 0xF);
    g_cpu.cpsr = spsr;
    g_cpu.R[15] = pc & ~1u;
    nr_redirect = 1;
}

extern "C" void nr_fallback(void) {
    // One instruction on the interpreter, then back to the top loop.
    interp_step();
    nr_redirect = 1;
}

extern "C" void nr_cp15_write(uint32_t opc1, uint32_t crn, uint32_t crm, uint32_t opc2, uint32_t v) {
    runtime_coproc_write(15, opc1, crn, crm, opc2, v);
    if (crn == 1 || crn == 9) nr_remap();
    if (nds_cpu_halted(0)) {
        // Wait for interrupt: give the time back to the scheduler until a wake.
        runtime_clear_fast_limit();
        nr_service();
    }
}
extern "C" uint32_t nr_cp15_read(uint32_t opc1, uint32_t crn, uint32_t crm, uint32_t opc2) {
    return runtime_coproc_read(15, opc1, crn, crm, opc2);
}

// ---------------------------------------------------------------------------
// BIOS calls. Common ones natively; anything else runs the BIOS code on the
// interpreter (enter SVC like the hardware, step until it returns).
// ---------------------------------------------------------------------------
static void halt_until_irq() {
    nds_cpu_enter_halt(0);
    runtime_clear_fast_limit();
    nr_service();
}

static void swi_intr_wait(bool discard, uint32_t mask) {
    const uint32_t flags_addr = dtcm_end() - 8u;
    bus_write_u32_slow(0x04000208u, 1u);   // IME = 1
    if (discard) wr32(flags_addr, rd32(flags_addr) & ~mask);
    const uint32_t resume = g_cpu.R[15];
    for (;;) {
        const uint32_t f = rd32(flags_addr);
        if (f & mask) { wr32(flags_addr, f & ~mask); return; }
        // The BIOS waits with interrupts enabled.
        const uint32_t cpsr = g_cpu.cpsr;
        g_cpu.cpsr &= ~CPSR_I_BIT;
        halt_until_irq();
        if (nds_irq_pending(0)) nr_take_irq(resume);
        g_cpu.cpsr = (g_cpu.cpsr & ~CPSR_I_BIT) | (cpsr & CPSR_I_BIT);
        if (nr_redirect) return;
    }
}

// Memory helpers for SWIs: page table first, the bus (with overlay
// bookkeeping) otherwise.
static inline uint16_t rd16h(uint32_t a) {
    const uintptr_t b = nr_rd_tab[a >> 12];
    if (b) { uint16_t v; memcpy(&v, (const void*)(b + (a & 0xFFEu)), 2); return v; }
    return (uint16_t)bus_read_u16_slow(a & ~1u);
}
static inline void wr16h(uint32_t a, uint16_t v) {
    const uintptr_t b = nr_wr_tab[a >> 12];
    if (b) { memcpy((void*)(b + (a & 0xFFEu)), &v, 2); return; }
    note_store(a); bus_write_u16_slow(a & ~1u, v);
}
static inline void wr32h(uint32_t a, uint32_t v) {
    const uintptr_t b = nr_wr_tab[a >> 12];
    if (b) { memcpy((void*)(b + (a & 0xFFCu)), &v, 4); return; }
    note_store(a); bus_write_u32_slow(a & ~3u, v);
}

// CpuSet (GBATEK): r0 source, r1 destination, r2 bits 0-20 unit count,
// bit 24 fill (fixed source), bit 26 32-bit units (else 16-bit).
static void swi_cpu_set() {
    uint32_t src = g_cpu.R[0], dst = g_cpu.R[1];
    const uint32_t ctl = g_cpu.R[2];
    uint32_t n = ctl & 0x1FFFFFu;
    const bool fill = (ctl >> 24) & 1u;
    g_runtime_cycles += 8u + n * 3u;   // about what the BIOS loop costs
    if ((ctl >> 26) & 1u) {
        src &= ~3u; dst &= ~3u;
        const uint32_t v0 = rd32(src);
        for (; n; --n, dst += 4u) {
            wr32h(dst, fill ? v0 : rd32(src));
            if (!fill) src += 4u;
        }
    } else {
        src &= ~1u; dst &= ~1u;
        const uint16_t v0 = rd16h(src);
        for (; n; --n, dst += 2u) {
            wr16h(dst, fill ? v0 : rd16h(src));
            if (!fill) src += 2u;
        }
    }
}

// CpuFastSet: 32-bit units, count rounded up to a multiple of 8 words.
static void swi_cpu_fast_set() {
    uint32_t src = g_cpu.R[0] & ~3u, dst = g_cpu.R[1] & ~3u;
    const uint32_t ctl = g_cpu.R[2];
    uint32_t n = ((ctl & 0x1FFFFFu) + 7u) & ~7u;
    const bool fill = (ctl >> 24) & 1u;
    g_runtime_cycles += 8u + n;
    const uint32_t v0 = rd32(src);
    for (; n; --n, dst += 4u) {
        wr32h(dst, fill ? v0 : rd32(src));
        if (!fill) src += 4u;
    }
}

static void swi_via_bios(uint32_t num) {
    ++g_nr_stats[NR_STAT_SWI_BIOS];
    const uint32_t ret = g_cpu.R[15];
    const uint32_t old = g_cpu.cpsr;
    runtime_msr_cpsr((old & ~0x1Fu) | 0x13u, 0x1);
    g_cpu.cpsr = (g_cpu.cpsr & ~CPSR_T_BIT) | CPSR_I_BIT;
    runtime_msr_spsr(old, 0xF);
    // LR_svc is the plain return address: the hardware never sets bit 0 here.
    // The BIOS reads the SWI number from the byte at [LR - 2] (both modes).
    g_cpu.R[14] = ret;
    g_cpu.R[15] = nds_exception_base() + 0x08u;
    g_nr_last_swi = num;
    // Step until the BIOS returns to the caller in its own mode.
    for (int guard = 0; guard < 50000000; ++guard) {
        if (g_cpu.R[15] == ret && (g_cpu.cpsr & 0x1Fu) == (old & 0x1Fu)) return;
        interp_step();
    }
    nds_halt("native: BIOS call did not return");
}

extern "C" void nr_swi(uint32_t num) {
    switch (num) {
    case 0x06: ++g_nr_stats[NR_STAT_SWI_HLE]; {           // Halt
        const uint32_t resume = g_cpu.R[15];
        halt_until_irq();
        if (!(g_cpu.cpsr & CPSR_I_BIT) && nds_irq_pending(0)) nr_take_irq(resume);
        return; }
    case 0x04: ++g_nr_stats[NR_STAT_SWI_HLE];              // IntrWait
        swi_intr_wait(g_cpu.R[0] == 1u, g_cpu.R[1]); return;
    case 0x05: ++g_nr_stats[NR_STAT_SWI_HLE];              // VBlankIntrWait
        swi_intr_wait(true, 1u); return;
    case 0x09: { ++g_nr_stats[NR_STAT_SWI_HLE];            // Div
        const int32_t n = (int32_t)g_cpu.R[0], d = (int32_t)g_cpu.R[1];
        if (d == 0) { swi_via_bios(num); return; }
        int32_t q = (n == INT32_MIN && d == -1) ? n : n / d;
        int32_t r = (n == INT32_MIN && d == -1) ? 0 : n % d;
        g_cpu.R[0] = (uint32_t)q; g_cpu.R[1] = (uint32_t)r;
        g_cpu.R[3] = (uint32_t)(q < 0 ? -q : q);
        return; }
    case 0x0D: { ++g_nr_stats[NR_STAT_SWI_HLE];            // Sqrt
        uint32_t v = g_cpu.R[0], r = 0, bit = 1u << 30;
        while (bit > v) bit >>= 2;
        while (bit) { if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; } else r >>= 1; bit >>= 2; }
        g_cpu.R[0] = r;
        return; }
    case 0x0B: ++g_nr_stats[NR_STAT_SWI_HLE]; g_nr_last_swi = num; swi_cpu_set(); return;      // CpuSet
    case 0x0C: ++g_nr_stats[NR_STAT_SWI_HLE]; g_nr_last_swi = num; swi_cpu_fast_set(); return; // CpuFastSet
    case 0x0F: ++g_nr_stats[NR_STAT_SWI_HLE]; g_cpu.R[0] = 0; return;   // IsDebugger
    default:
        swi_via_bios(num);
        return;
    }
}

extern "C" void nr_stats_snapshot(unsigned long long* out, unsigned n) {
    for (unsigned i = 0; i < n && i < 16; ++i) out[i] = g_nr_stats[i];
}

static void remap_hook() { if (g_arm9_started_flag) nr_remap(); }

// Once a second: flush the log to the SD card and write one status line, so a
// hang (or an app closed from the HOME menu) still leaves the full story.
static void heartbeat_main() {
    unsigned long long prev[16] = {};
    for (unsigned sec = 1;; ++sec) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        unsigned long long cur[16];
        for (int i = 0; i < 16; ++i) cur[i] = g_nr_stats[i];
        std::fprintf(stderr,
            "[native] t=%us pc=%08X swi=%02X | per s: service %llu yield %llu irq %llu dyn %llu "
            "dynmiss %llu interp %llu swi-hle %llu swi-bios %llu redirect %llu ovswitch %llu io %llu\n",
            sec, g_nr_last_pc, g_nr_last_swi & 0xFFu,
            cur[0] - prev[0], cur[1] - prev[1], cur[2] - prev[2], cur[3] - prev[3], cur[4] - prev[4],
            cur[5] - prev[5], cur[6] - prev[6], cur[7] - prev[7], cur[8] - prev[8], cur[9] - prev[9],
            cur[10] - prev[10]);
        std::fflush(stderr);
        std::fflush(stdout);
        for (int i = 0; i < 16; ++i) prev[i] = cur[i];
    }
}

// Installed by the Switch layer before the runner starts.
extern "C" void nr_install(void) {
    g_nx_native_build = true;
    g_nx_arm9_native_hook = nr_resume_arm9;
    g_nx_remap_hook = remap_hook;
    std::thread(heartbeat_main).detach();
}
