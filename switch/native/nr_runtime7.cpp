// MPH-nx native ARM7 runtime (native6).
//
// The ARM7 program (NitroSDK's system and sound code) runs as native code on
// its own stack, like the ARM9 (nr_runtime.cpp): every guest function is a C
// function with direct calls and returns. The existing scheduler still owns
// time: its ARM7 slice resumes this coroutine, and the native code yields back
// when the slice ends, the CPU halts or a DMA takes the bus.
//
// ARM7 differences from the ARM9 side: ARMv4T code (see tools/nrecomp/decode.py),
// no CP15, no overlays, the BIOS IRQ handler pointer at 0x03FFFFFC and the
// IntrWait flags at 0x03FFFFF8, halt through HALTCNT.
#include "nr_runtime7.h"
#include "nr_coro.h"
#include "runtime_arm.h"
#include "state.h"
#include "io.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

extern "C" {
extern const NrEntry nr7_tab[];
extern const uint32_t nr7_tab_count;
uintptr_t nr7_rd_tab[1u << 20];
uintptr_t nr7_wr_tab[1u << 20];
char* nr7_stack_limit = nullptr;
unsigned long long g_nr7_stats[16];
bool runtime_should_yield_slow(void);
void runtime_msr_cpsr(uint32_t value, uint32_t mask);
uint32_t runtime_mrs_spsr(void);
void runtime_msr_spsr(uint32_t value, uint32_t mask);
void runtime_clear_fast_limit(void);
extern void (*g_nx_arm7_native_hook)(void);
extern void (*g_nr7_remap)(void);
}
extern bool g_discover_static_misses;
void tier3_step_one(uint32_t pc);
uint32_t nds_irq_pending(int cpu);
bool nds_cpu_halted(int cpu);
void nds_cpu_enter_halt(int cpu);
bool nds_dma_cpu_stalled(int cpu);

enum { S_SERVICE, S_YIELD, S_IRQ, S_DYN, S_DYN_MISS, S_INTERP, S_SWI_HLE, S_SWI_BIOS,
       S_REDIRECT, S_IO, S_HALT };

extern "C" void nr7_stack_overflow(void) {
    std::fprintf(stderr, "[native7] host stack exhausted at %08X\n", g_cpu.R[15]);
    nds_halt("native7: host stack exhausted");
    nr7_stack_limit = nullptr;
}

// ---------------------------------------------------------------------------
// Entry lookup: one direct table per address cluster (crt0 in main RAM, the
// WRAM section, the main-RAM section)
// ---------------------------------------------------------------------------
struct DirTab {
    uint32_t lo = 0, hi = 0;
    std::vector<NrFn> arm, thm;
    inline NrFn get(uint32_t key) const {
        const uint32_t a = key & ~1u;
        if (a < lo || a >= hi) return nullptr;
        if (key & 1u) return thm[(a - lo) >> 1];
        return (a & 3u) ? nullptr : arm[(a - lo) >> 2];
    }
};
static std::vector<DirTab> g_dir;

static void dir_build() {
    std::vector<uint32_t> addrs;
    for (uint32_t i = 0; i < nr7_tab_count; ++i) addrs.push_back(nr7_tab[i].key & ~1u);
    std::sort(addrs.begin(), addrs.end());
    // clusters: split where entries are more than 64 KiB apart
    size_t s = 0;
    for (size_t i = 1; i <= addrs.size(); ++i) {
        if (i == addrs.size() || addrs[i] - addrs[i - 1] > 0x10000u) {
            DirTab t;
            t.lo = addrs[s] & ~3u;
            t.hi = (addrs[i - 1] + 4u) & ~3u;
            t.arm.assign((t.hi - t.lo) >> 2, nullptr);
            t.thm.assign((t.hi - t.lo) >> 1, nullptr);
            g_dir.push_back(std::move(t));
            s = i;
        }
    }
    for (uint32_t i = 0; i < nr7_tab_count; ++i) {
        const uint32_t k = nr7_tab[i].key, a = k & ~1u;
        for (DirTab& t : g_dir) {
            if (a < t.lo || a >= t.hi) continue;
            if (k & 1u) t.thm[(a - t.lo) >> 1] = nr7_tab[i].fn;
            else if (!(a & 3u)) t.arm[(a - t.lo) >> 2] = nr7_tab[i].fn;
        }
    }
}

static inline NrFn lookup(uint32_t key) {
    for (const DirTab& t : g_dir)
        if (NrFn fn = t.get(key)) return fn;
    return nullptr;
}

// ---------------------------------------------------------------------------
// Memory map (the ARM7's view)
// ---------------------------------------------------------------------------
static void map_range(uint32_t lo, uint32_t hi, const NdsBusFastWin& w) {
    if (!w.data) return;
    for (uint64_t a = lo; a < hi; a += 4096u) {
        const uint32_t pg = (uint32_t)(a >> 12);
        const uintptr_t host = (uintptr_t)(w.data + ((uint32_t)a & w.mask & ~0xFFFu));
        nr7_rd_tab[pg] = host;
        nr7_wr_tab[pg] = host;
    }
}

static bool g_started = false;
static void remap() {
    if (!g_started) return;
    std::memset(nr7_rd_tab, 0, sizeof nr7_rd_tab);
    std::memset(nr7_wr_tab, 0, sizeof nr7_wr_tab);
    map_range(0x02000000u, 0x03000000u, g_busf_main);
    map_range(0x03000000u, 0x03800000u, g_busf_wram_lo[NDS_ARM7]);
    map_range(0x03800000u, 0x04000000u, g_busf_wram_hi[NDS_ARM7]);
}

// ---------------------------------------------------------------------------
// Coroutine
// ---------------------------------------------------------------------------
static NrCoro g_sched_ctx, g_arm7_ctx;
static std::vector<uint8_t> g_stack;
static uint32_t g_boot_pc = 0;
static volatile uint32_t g_last_pc = 0;
static volatile uint32_t g_last_swi = 0xFFu;
// nr_redirect and g_nr_q are shared with the ARM9 side; each CPU keeps its
// own values across the other's slices.
static uint32_t g_my_redirect = 0, g_my_q = 0;

static void yield7() {
    ++g_nr7_stats[S_YIELD];
    nr_coro_switch(&g_arm7_ctx, &g_sched_ctx);
}

static void run(uint32_t key);
static void coro_main(void*) {
    run(g_boot_pc);
    for (;;) yield7();
}

extern "C" void nr7_resume(void) {
    if (!g_started) {
        g_started = true;
        remap();
        g_boot_pc = g_cpu.R[15] | ((g_cpu.cpsr & CPSR_T_BIT) ? 1u : 0u);
        g_stack.resize(4u << 20);
        nr7_stack_limit = (char*)g_stack.data() + (128u << 10);
        nr_coro_init(&g_arm7_ctx, g_stack.data(), g_stack.size(), coro_main, nullptr);
        std::fprintf(stderr, "[native7] ARM7 native start at %08X (%u entries)\n",
                     g_boot_pc, nr7_tab_count);
    }
    const uint32_t keep_redirect = nr_redirect, keep_q = g_nr_q;
    nr_redirect = g_my_redirect; g_nr_q = g_my_q;
    nr_coro_switch(&g_sched_ctx, &g_arm7_ctx);
    g_my_redirect = nr_redirect; g_my_q = g_nr_q;
    nr_redirect = keep_redirect; g_nr_q = keep_q;
}

// ---------------------------------------------------------------------------
// I/O
// ---------------------------------------------------------------------------
static inline void after_write(uint32_t a) {
    // A DMA control write may start an immediate DMA: the CPU waits for it.
    if (a >= 0x040000B0u && a < 0x040000E0u) {
        while (nds_dma_cpu_stalled(1)) { yield7(); g_nds_unwinding = 0; }
        return;
    }
    // HALTCNT: stop at the next service point (registers are in C locals
    // here, so the halt and the wake interrupt are handled there).
    if ((a & ~3u) == 0x04000300u && nds_cpu_halted(1)) runtime_clear_fast_limit();
}
extern "C" uint32_t nr7_io_rd32(uint32_t a) { ++g_nr7_stats[S_IO]; return bus_read_u32_slow(a); }
extern "C" uint32_t nr7_io_rd16(uint32_t a) { ++g_nr7_stats[S_IO]; return bus_read_u16_slow(a); }
extern "C" uint32_t nr7_io_rd8(uint32_t a) { ++g_nr7_stats[S_IO]; return bus_read_u8_slow(a); }
extern "C" void nr7_io_wr32(uint32_t a, uint32_t v) { ++g_nr7_stats[S_IO]; bus_write_u32_slow(a, v); after_write(a); }
extern "C" void nr7_io_wr16(uint32_t a, uint32_t v) { ++g_nr7_stats[S_IO]; bus_write_u16_slow(a, (uint16_t)v); after_write(a); }
extern "C" void nr7_io_wr8(uint32_t a, uint32_t v) { ++g_nr7_stats[S_IO]; bus_write_u8_slow(a, (uint8_t)v); after_write(a); }

static inline uint32_t rd32(uint32_t a) {
    const uintptr_t b = nr7_rd_tab[a >> 12];
    if (b) { uint32_t v; std::memcpy(&v, (const void*)(b + (a & 0xFFCu)), 4); return v; }
    return bus_read_u32_slow(a & ~3u);
}
static inline void wr32(uint32_t a, uint32_t v) {
    const uintptr_t b = nr7_wr_tab[a >> 12];
    if (b) { std::memcpy((void*)(b + (a & 0xFFCu)), &v, 4); return; }
    bus_write_u32_slow(a & ~3u, v);
}
static inline uint16_t rd16(uint32_t a) {
    const uintptr_t b = nr7_rd_tab[a >> 12];
    if (b) { uint16_t v; std::memcpy(&v, (const void*)(b + (a & 0xFFEu)), 2); return v; }
    return (uint16_t)bus_read_u16_slow(a & ~1u);
}
static inline void wr16(uint32_t a, uint16_t v) {
    const uintptr_t b = nr7_wr_tab[a >> 12];
    if (b) { std::memcpy((void*)(b + (a & 0xFFEu)), &v, 2); return; }
    bus_write_u16_slow(a & ~1u, v);
}
static inline uint8_t rd8(uint32_t a) {
    const uintptr_t b = nr7_rd_tab[a >> 12];
    if (b) return *(const uint8_t*)(b + (a & 0xFFFu));
    return (uint8_t)bus_read_u8_slow(a);
}

// ---------------------------------------------------------------------------
// Service point and interrupts
// ---------------------------------------------------------------------------
static const uint32_t IRQ_RET = 0xFFFF0FF0u;   // not ARM7 code: the BIOS IRQ epilogue
static void take_irq(uint32_t resume);
static void irq_exit();

extern "C" void nr7_service(void) {
    ++g_nr7_stats[S_SERVICE];
    const uint32_t resume = g_cpu.R[15];
    g_last_pc = resume | ((g_cpu.cpsr & CPSR_T_BIT) ? 1u : 0u);
    if (runtime_should_yield_slow()) {
        yield7();
        g_nds_unwinding = 0;
    }
    if (!(g_cpu.cpsr & CPSR_I_BIT) && nds_irq_pending(1))
        take_irq(resume);
}

// BIOS: LDMFD sp!, {r0-r3, r12, lr}; SUBS pc, lr, #4
static void irq_exit() {
    uint32_t sp = g_cpu.R[13];
    g_cpu.R[0] = rd32(sp); g_cpu.R[1] = rd32(sp + 4); g_cpu.R[2] = rd32(sp + 8);
    g_cpu.R[3] = rd32(sp + 12); g_cpu.R[12] = rd32(sp + 16); g_cpu.R[14] = rd32(sp + 20);
    g_cpu.R[13] = sp + 24u;
    const uint32_t lr = g_cpu.R[14];
    const uint32_t spsr = runtime_mrs_spsr();
    runtime_msr_cpsr(spsr, 0xF);
    g_cpu.cpsr = spsr;
    g_cpu.R[15] = (lr - 4u) & ~1u;
}

// IRQ entry as the hardware and the ARM7 BIOS vector do it:
// STMFD sp!, {r0-r3, r12, lr}; MOV r0, #0x04000000; ADD lr, pc, #0;
// LDR pc, [r0, #-4]  (the handler pointer at 0x03FFFFFC).
static void take_irq(uint32_t resume) {
    ++g_nr7_stats[S_IRQ];
    const uint32_t old = g_cpu.cpsr;
    const uint32_t ret_thumb = (old & CPSR_T_BIT) ? 1u : 0u;
    runtime_msr_cpsr((old & ~0x3Fu) | 0x92u, 0x1);
    g_cpu.cpsr = (g_cpu.cpsr & ~CPSR_T_BIT) | CPSR_I_BIT;
    runtime_msr_spsr(old, 0xF);
    g_cpu.R[14] = resume + 4u;
    uint32_t sp = g_cpu.R[13] - 24u;
    const uint32_t saved[6] = {g_cpu.R[0], g_cpu.R[1], g_cpu.R[2], g_cpu.R[3], g_cpu.R[12], g_cpu.R[14]};
    for (int i = 0; i < 6; ++i) wr32(sp + 4u * i, saved[i]);
    g_cpu.R[13] = sp;
    g_cpu.R[0] = 0x04000000u;
    const uint32_t handler = rd32(0x03FFFFFCu);
    g_cpu.R[14] = IRQ_RET;
    const uint32_t keep_redirect = nr_redirect;
    nr_redirect = 0;
    nr7_call_dyn(handler);
    if (g_cpu.R[15] != IRQ_RET) {
        if (!nr_redirect) nr_redirect = 1;
        return;
    }
    nr_redirect = keep_redirect;
    irq_exit();
    if (g_cpu.R[15] != resume || ((g_cpu.cpsr & CPSR_T_BIT) ? 1u : 0u) != ret_thumb) nr_redirect = 1;
}

extern "C" void nr7_msr_cpsr(uint32_t v, uint32_t mask) {
    runtime_msr_cpsr(v, mask);
    g_nr_q = 0;
    if (!(g_cpu.cpsr & CPSR_I_BIT) && nds_irq_pending(1)) runtime_clear_fast_limit();
}

extern "C" void nr7_exception_return(uint32_t pc) {
    const uint32_t spsr = runtime_mrs_spsr();
    runtime_msr_cpsr(spsr, 0xF);
    g_cpu.cpsr = spsr;
    g_cpu.R[15] = pc & ~1u;
    nr_redirect = 1;
}

extern "C" void nr7_cp15_write(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {}
extern "C" uint32_t nr7_cp15_read(uint32_t, uint32_t, uint32_t, uint32_t) { return 0; }

// ---------------------------------------------------------------------------
// Interpreter fallback
// ---------------------------------------------------------------------------
static void note_miss(uint32_t key, bool call = false) {
    static uint32_t seen[1024]; static uint32_t n = 0;
    for (uint32_t i = 0; i < n; ++i) if (seen[i] == key) return;
    if (n < 1024) seen[n++] = key;
    if (n <= 800) std::fprintf(stderr, "[native7] no native code at %08X%s%s\n", key & ~1u,
                               (key & 1) ? " (thumb)" : "", call ? " (call)" : "");
}

static inline uint32_t cur_key() { return g_cpu.R[15] | ((g_cpu.cpsr & CPSR_T_BIT) ? 1u : 0u); }

// One interpreted instruction. Native stores keep no write provenance, so the
// interpreter is allowed to run any ARM7 code (it otherwise insists on it).
static int g_trace_interp = -1;   // host debug: NR7_TRACE_INTERP=N logs N steps
static void interp_step() {
    ++g_nr7_stats[S_INTERP];
    if (g_trace_interp < 0) { const char* e = std::getenv("NR7_TRACE_INTERP"); g_trace_interp = e ? std::atoi(e) : 0; }
    if (g_trace_interp > 0) {
        --g_trace_interp;
        std::fprintf(stderr, "[native7] interp %08X cpsr %08X lr %08X sp %08X cyc %llu limit %llu\n", g_cpu.R[15], g_cpu.cpsr,
                     g_cpu.R[14], g_cpu.R[13], (unsigned long long)g_runtime_cycles, (unsigned long long)g_nds_fast_limit);
    }
    const bool keep = g_discover_static_misses;
    g_discover_static_misses = true;
    const uint32_t pc0 = g_cpu.R[15], mode0 = g_cpu.cpsr & 0x1Fu;
    tier3_step_one(g_cpu.R[15]);
    g_discover_static_misses = keep;
    if (runtime_call_stack_depth()) runtime_call_stack_restore(nullptr, 0);
    const uint32_t lr = g_cpu.R[14] & ~1u, pc = g_cpu.R[15];
    if ((lr == pc0 + 4u || lr == pc0 + 2u) && pc != lr && pc >= 0x4000u && pc0 >= 0x4000u &&
        (g_cpu.cpsr & 0x1Fu) == mode0) {
        const uint32_t k = cur_key();
        if (!lookup(k)) note_miss(k, true);
    }
    if (g_runtime_cycles >= g_nds_fast_limit) nr7_service();
}

static void run(uint32_t key) {
    g_cpu.R[15] = key & ~1u;
    static int trace_run = -1;   // host debug: NR7_TRACE_RUN=N logs N top-loop entries
    if (trace_run < 0) { const char* e = std::getenv("NR7_TRACE_RUN"); trace_run = e ? std::atoi(e) : 0; }
    for (;;) {
        nr_redirect = 0;
        if (g_cpu.R[15] == IRQ_RET) { irq_exit(); continue; }
        const uint32_t k = cur_key();
        if (trace_run > 0) { --trace_run; std::fprintf(stderr, "[native7] run %08X lr %08X cyc %llu\n", k, g_cpu.R[14], (unsigned long long)g_runtime_cycles); }
        if (NrFn fn = lookup(k)) {
            fn((k & ~1u) | 0x80000000u);   // a resume: LR is not a return address here
            if (nr_redirect) ++g_nr7_stats[S_REDIRECT];
        } else {
            if (g_cpu.R[15] >= 0x4000u) note_miss(k);
            interp_step();
        }
    }
}

static int g_trace_dyn = -1;
extern "C" void nr7_call_dyn(uint32_t target) {
    if (g_trace_dyn < 0) { const char* e = std::getenv("NR7_TRACE_DYN"); g_trace_dyn = e ? std::atoi(e) : 0; }
    ++g_nr7_stats[S_DYN];
    const uint32_t ret = g_cpu.R[14] & ~1u;
    if (target & 1) g_cpu.cpsr |= CPSR_T_BIT; else g_cpu.cpsr &= ~CPSR_T_BIT;
    g_cpu.R[15] = target & ~1u;
    bool first = true;
    for (;;) {
        if (g_cpu.R[15] == IRQ_RET && ret != IRQ_RET) { irq_exit(); nr_redirect = 0; continue; }
        const uint32_t k = cur_key();
        if (NrFn fn = lookup(k)) {
            fn(first ? (k & ~1u) : ((k & ~1u) | 0x80000000u));   // later passes resume
            if (g_trace_dyn > 0) { --g_trace_dyn; std::fprintf(stderr, "[native7] dyn %08X ret %08X: ran %08X -> pc %08X cpsr %08X redirect %u\n", target, ret, k, g_cpu.R[15], g_cpu.cpsr, nr_redirect); }
            if (g_cpu.R[15] == ret) { nr_redirect = 0; return; }
            if (!nr_redirect || nr_redirect == 2) { nr_redirect = 0; first = false; continue; }
            return;
        }
        if (first && g_cpu.R[15] >= 0x4000u) { ++g_nr7_stats[S_DYN_MISS]; note_miss(k, true); }
        first = false;
        interp_step();
        if (nr_redirect == 1) return;
        nr_redirect = 0;
        if (g_cpu.R[15] == ret) return;
    }
}

extern "C" void nr7_fallback(void) {
    interp_step();
    nr_redirect = 1;
}

// ---------------------------------------------------------------------------
// BIOS calls (GBATEK "BIOS Functions", NDS7 numbering)
// ---------------------------------------------------------------------------
static void halt_until_irq() {
    ++g_nr7_stats[S_HALT];
    nds_cpu_enter_halt(1);
    runtime_clear_fast_limit();
    nr7_service();
}

static void swi_via_bios(uint32_t num) {
    ++g_nr7_stats[S_SWI_BIOS];
    const uint32_t ret = g_cpu.R[15];
    const uint32_t old = g_cpu.cpsr;
    runtime_msr_cpsr((old & ~0x1Fu) | 0x13u, 0x1);
    g_cpu.cpsr = (g_cpu.cpsr & ~CPSR_T_BIT) | CPSR_I_BIT;
    runtime_msr_spsr(old, 0xF);
    g_cpu.R[14] = ret;
    g_cpu.R[15] = 0x00000008u;
    g_last_swi = num;
    { static int tr = -1; if (tr < 0) { const char* e = std::getenv("NR7_TRACE_SWI"); tr = e ? std::atoi(e) : 0; }
      if (tr > 0) { --tr; g_trace_interp = 400; std::fprintf(stderr, "[native7] swi %02X via BIOS ret %08X old cpsr %08X\n", num, ret, old); } }
    // The BIOS's call dispatcher re-enables interrupts (SYS mode, I clear),
    // and the interpreter would then run the game's interrupt handler on
    // itself. Hold IME at 0 for the call: an interrupt waits until the call
    // returns, a few instructions later. (The waiting calls, Halt and
    // IntrWait, are handled natively above and never come here.)
    const uint32_t ime = bus_read_u32_slow(0x04000208u);
    if (ime & 1u) bus_write_u32_slow(0x04000208u, 0u);
    for (int guard = 0; guard < 50000000; ++guard) {
        if (g_cpu.R[15] == ret && (g_cpu.cpsr & 0x1Fu) == (old & 0x1Fu)) {
            if (ime & 1u) bus_write_u32_slow(0x04000208u, ime);
            return;
        }
        interp_step();
        if (nr_redirect) break;
    }
    if (ime & 1u) bus_write_u32_slow(0x04000208u, ime);
    if (nr_redirect) return;
    std::fprintf(stderr, "[native7] BIOS call %02X did not return (pc %08X cpsr %08X, ret %08X old cpsr %08X)\n",
                 num, g_cpu.R[15], g_cpu.cpsr, ret, old);
    nds_halt("native7: BIOS call did not return");
}

static void swi_intr_wait(bool discard, uint32_t mask) {
    const uint32_t flags_addr = 0x03FFFFF8u;
    bus_write_u32_slow(0x04000208u, 1u);   // IME = 1
    if (discard) wr32(flags_addr, rd32(flags_addr) & ~mask);
    const uint32_t resume = g_cpu.R[15];
    for (;;) {
        const uint32_t f = rd32(flags_addr);
        if (f & mask) { wr32(flags_addr, f & ~mask); return; }
        const uint32_t cpsr = g_cpu.cpsr;
        g_cpu.cpsr &= ~CPSR_I_BIT;
        halt_until_irq();
        if (nds_irq_pending(1)) take_irq(resume);
        g_cpu.cpsr = (g_cpu.cpsr & ~CPSR_I_BIT) | (cpsr & CPSR_I_BIT);
        if (nr_redirect) return;
    }
}

static void swi_cpu_set() {
    uint32_t src = g_cpu.R[0], dst = g_cpu.R[1];
    const uint32_t ctl = g_cpu.R[2];
    uint32_t n = ctl & 0x1FFFFFu;
    const bool fill = (ctl >> 24) & 1u;
    g_runtime_cycles += 8u + n * 3u;
    if ((ctl >> 26) & 1u) {
        src &= ~3u; dst &= ~3u;
        const uint32_t v0 = rd32(src);
        for (; n; --n, dst += 4u) { wr32(dst, fill ? v0 : rd32(src)); if (!fill) src += 4u; }
    } else {
        src &= ~1u; dst &= ~1u;
        const uint16_t v0 = rd16(src);
        for (; n; --n, dst += 2u) { wr16(dst, fill ? v0 : rd16(src)); if (!fill) src += 2u; }
    }
}

static void swi_cpu_fast_set() {
    uint32_t src = g_cpu.R[0] & ~3u, dst = g_cpu.R[1] & ~3u;
    const uint32_t ctl = g_cpu.R[2];
    uint32_t n = ((ctl & 0x1FFFFFu) + 7u) & ~7u;
    const bool fill = (ctl >> 24) & 1u;
    g_runtime_cycles += 8u + n;
    const uint32_t v0 = rd32(src);
    for (; n; --n, dst += 4u) { wr32(dst, fill ? v0 : rd32(src)); if (!fill) src += 4u; }
}

extern "C" void nr7_swi(uint32_t num) {
    switch (num) {
    case 0x06: { ++g_nr7_stats[S_SWI_HLE];               // Halt
        const uint32_t resume = g_cpu.R[15];
        halt_until_irq();
        if (!(g_cpu.cpsr & CPSR_I_BIT) && nds_irq_pending(1)) take_irq(resume);
        return; }
    case 0x1F: {                                         // CustomHalt: HALTCNT = r2
        const uint8_t v = (uint8_t)g_cpu.R[2];
        if ((v & 0xC0u) == 0x80u) {
            ++g_nr7_stats[S_SWI_HLE];
            const uint32_t resume = g_cpu.R[15];
            halt_until_irq();
            if (!(g_cpu.cpsr & CPSR_I_BIT) && nds_irq_pending(1)) take_irq(resume);
            return;
        }
        swi_via_bios(num); return; }
    case 0x04: ++g_nr7_stats[S_SWI_HLE]; swi_intr_wait(g_cpu.R[0] == 1u, g_cpu.R[1]); return;
    case 0x05: ++g_nr7_stats[S_SWI_HLE]; swi_intr_wait(true, 1u); return;
    case 0x03: { ++g_nr7_stats[S_SWI_HLE];               // WaitByLoop
        const int32_t n = (int32_t)g_cpu.R[0];
        if (n > 0) g_runtime_cycles += (uint64_t)n * 4u;
        g_cpu.R[0] = 0;
        if (g_runtime_cycles >= g_nds_fast_limit) nr7_service();
        return; }
    case 0x09: { ++g_nr7_stats[S_SWI_HLE];               // Div
        const int32_t n = (int32_t)g_cpu.R[0], d = (int32_t)g_cpu.R[1];
        if (d == 0) { swi_via_bios(num); return; }
        const int32_t q = (n == INT32_MIN && d == -1) ? n : n / d;
        const int32_t r = (n == INT32_MIN && d == -1) ? 0 : n % d;
        g_cpu.R[0] = (uint32_t)q; g_cpu.R[1] = (uint32_t)r;
        g_cpu.R[3] = (uint32_t)(q < 0 ? -q : q);
        return; }
    case 0x0D: { ++g_nr7_stats[S_SWI_HLE];               // Sqrt
        uint32_t v = g_cpu.R[0], r = 0, bit = 1u << 30;
        while (bit > v) bit >>= 2;
        while (bit) { if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; } else r >>= 1; bit >>= 2; }
        g_cpu.R[0] = r;
        return; }
    case 0x0B: ++g_nr7_stats[S_SWI_HLE]; swi_cpu_set(); return;
    case 0x0C: ++g_nr7_stats[S_SWI_HLE]; swi_cpu_fast_set(); return;
    case 0x0E: { ++g_nr7_stats[S_SWI_HLE];               // GetCRC16
        uint32_t crc = g_cpu.R[0] & 0xFFFFu;
        const uint32_t a = g_cpu.R[1], n = g_cpu.R[2];
        for (uint32_t i = 0; i < n; ++i) {
            crc ^= rd8(a + i);
            for (int k = 0; k < 8; ++k) crc = (crc & 1u) ? (crc >> 1) ^ 0xA001u : (crc >> 1);
        }
        g_cpu.R[0] = crc;
        g_runtime_cycles += 8u + n * 2u;
        return; }
    case 0x0F: ++g_nr7_stats[S_SWI_HLE]; g_cpu.R[0] = 0; return;   // IsDebugger
    case 0x1A: case 0x1B: case 0x1C: {                   // GetSine/Pitch/VolumeTable
        // Pure table reads from the BIOS (the sound driver calls them per
        // channel update). The first call for each index runs the BIOS code;
        // its result is kept, so later calls skip the interpreter.
        static std::vector<uint64_t> memo[3];
        std::vector<uint64_t>& m = memo[num - 0x1A];
        const uint32_t idx = g_cpu.R[0];
        if (idx < 0x400u) {
            if (m.empty()) m.assign(0x400u, 0);
            const uint64_t e = m[idx];
            if (e >> 63) {
                ++g_nr7_stats[S_SWI_HLE];
                g_cpu.R[0] = (uint32_t)e & 0xFFFFu;
                g_cpu.R[1] = (uint32_t)(e >> 16) & 0xFFFFFFFFu;
                g_runtime_cycles += 24u;
                return;
            }
            const uint32_t r2 = g_cpu.R[2], r3 = g_cpu.R[3], r12 = g_cpu.R[12];
            swi_via_bios(num);
            if (!nr_redirect && g_cpu.R[2] == r2 && g_cpu.R[3] == r3 && g_cpu.R[12] == r12 &&
                g_cpu.R[0] <= 0xFFFFu)
                m[idx] = (1ull << 63) | ((uint64_t)g_cpu.R[1] << 16) | g_cpu.R[0];
            return;
        }
        swi_via_bios(num); return; }
    default: swi_via_bios(num); return;
    }
}

// ---------------------------------------------------------------------------
// Status line and install
// ---------------------------------------------------------------------------
static void heartbeat_main() {
    unsigned long long prev[16] = {};
    for (unsigned sec = 1;; ++sec) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (!g_started) continue;
        unsigned long long cur[16];
        for (int i = 0; i < 16; ++i) cur[i] = g_nr7_stats[i];
        if (sec % 5 == 0)
            std::fprintf(stderr,
                "[native7] t=%us pc=%08X swi=%02X | per s: service %llu yield %llu irq %llu dyn %llu "
                "dynmiss %llu interp %llu swi-hle %llu swi-bios %llu redirect %llu io %llu halt %llu\n",
                sec, g_last_pc, g_last_swi & 0xFFu,
                (cur[0] - prev[0]) / 5, (cur[1] - prev[1]) / 5, (cur[2] - prev[2]) / 5,
                (cur[3] - prev[3]) / 5, (cur[4] - prev[4]) / 5, (cur[5] - prev[5]) / 5,
                (cur[6] - prev[6]) / 5, (cur[7] - prev[7]) / 5, (cur[8] - prev[8]) / 5,
                (cur[9] - prev[9]) / 5, (cur[10] - prev[10]) / 5);
        if (sec % 5 == 0) for (int i = 0; i < 16; ++i) prev[i] = cur[i];
    }
}

extern "C" void nr7_install(void) {
    dir_build();
    g_nx_arm7_native_hook = nr7_resume;
    g_nr7_remap = remap;
    std::thread(heartbeat_main).detach();
}

#ifdef NR_CALLTRACE
extern "C" void nr7_fentry(uint32_t) {}
#endif
#ifdef NR_TRACE_ON
extern "C" void nr7_trace(uint32_t, const uint32_t*, uint32_t) {}
#endif
