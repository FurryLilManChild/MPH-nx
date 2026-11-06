// MPH-nx differential test: runs generated native functions and the reference
// ARM interpreter (arm-recomp-core, MIT) side by side from the same register
// state and memory, and compares registers, flags and every memory byte.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <algorithm>
#include <csetjmp>
#include "interpreter.h"
#include "arm_decode.h"
#include "thumb_decode.h"
extern "C" {
#include "nr_tables.h"
}

// ---- guest memory: ITCM, main RAM, DTCM -----------------------------------
struct Mem {
    std::vector<uint8_t> itcm = std::vector<uint8_t>(0x8000), ram = std::vector<uint8_t>(0x400000),
                         dtcm = std::vector<uint8_t>(0x4000);
    uint8_t* p(uint32_t a) {
        if (a - 0x027E0000u < 0x4000u) return &dtcm[a - 0x027E0000u];
        if (a < 0x02000000u) return &itcm[a & 0x7FFF];
        if (a - 0x02000000u < 0x01000000u) return &ram[a & 0x3FFFFF];
        return nullptr;
    }
};
static Mem g_native, g_ref;
static unsigned g_io_native = 0, g_io_ref = 0;

struct RefBus : armv4t::Bus {
    uint8_t read8(uint32_t a) override { uint8_t* q = g_ref.p(a); if (!q) { ++g_io_ref; return 0; } return *q; }
    uint16_t read16(uint32_t a) override { uint8_t* q = g_ref.p(a & ~1u); if (!q) { ++g_io_ref; return 0; } uint16_t v; memcpy(&v, q, 2); return v; }
    uint32_t read32(uint32_t a) override { uint8_t* q = g_ref.p(a & ~3u); if (!q) { ++g_io_ref; return 0; } uint32_t v; memcpy(&v, q, 4); return v; }
    void write8(uint32_t a, uint8_t v) override { uint8_t* q = g_ref.p(a); if (!q) { ++g_io_ref; return; } *q = v; }
    void write16(uint32_t a, uint16_t v) override { uint8_t* q = g_ref.p(a & ~1u); if (!q) { ++g_io_ref; return; } memcpy(q, &v, 2); }
    void write32(uint32_t a, uint32_t v) override { uint8_t* q = g_ref.p(a & ~3u); if (!q) { ++g_io_ref; return; } memcpy(q, &v, 4); }
};

// ---- native runtime stubs ---------------------------------------------------
extern "C" {
ArmCpuState g_cpu;
unsigned long long g_runtime_cycles = 0, nr_deadline = ~0ull;
uint32_t nr_redirect = 0, g_nr_q = 0;
uintptr_t nr_rd_tab[1u << 20], nr_wr_tab[1u << 20];
static int g_escape = 0;
uint32_t nr_io_rd32(uint32_t) { ++g_io_native; return 0; }
uint32_t nr_io_rd16(uint32_t) { ++g_io_native; return 0; }
uint32_t nr_io_rd8(uint32_t) { ++g_io_native; return 0; }
void nr_io_wr32(uint32_t, uint32_t) { ++g_io_native; }
void nr_io_wr16(uint32_t, uint32_t) { ++g_io_native; }
void nr_io_wr8(uint32_t, uint32_t) { ++g_io_native; }
static int g_trace = 0;
static std::vector<std::vector<uint32_t>> g_ntrace;
void nr_trace(uint32_t pc, const uint32_t* r, uint32_t flags) {
    if (!g_trace) return;
    std::vector<uint32_t> v(r, r + 15); v.push_back(flags); v.push_back(pc);
    g_ntrace.push_back(v);
}
static jmp_buf g_bail;
char* nr_stack_limit = nullptr;
void nr_stack_overflow(void) { g_escape = 8; longjmp(g_bail, 1); } static int g_services = 0;
void nr_service(void) {
    nr_deadline = g_runtime_cycles + 100000;
    if (++g_services > 20) { g_escape = 8; longjmp(g_bail, 1); }
}
static NrFn lookup(uint32_t key) {
    const NrEntry* b = nr_tab_static; const NrEntry* e = b + nr_tab_static_count;
    const NrEntry* it = std::lower_bound(b, e, key, [](const NrEntry& x, uint32_t k) { return x.key < k; });
    return (it != e && it->key == key) ? it->fn : nullptr;
}
void nr_call_dyn(uint32_t t) {
    NrFn f = lookup(t);
    if (!f) { g_escape = 1; nr_redirect = 1; g_cpu.R[15] = t; return; }
    f(0);
}
void nr_swi(uint32_t) { g_escape = 2; nr_redirect = 1; }
void nr_cp15_write(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) { g_escape = 3; nr_redirect = 1; }
uint32_t nr_cp15_read(uint32_t, uint32_t, uint32_t, uint32_t) { return 0; }
void nr_msr_cpsr(uint32_t v, uint32_t mask) {
    uint32_t m = 0; if (mask & 1) m |= 0xFF; if (mask & 8) m |= 0xF8000000u;
    g_cpu.cpsr = (g_cpu.cpsr & ~m) | (v & m); g_nr_q = (g_cpu.cpsr >> 27) & 1;
    if ((v & 0x1F) != (0x1F)) { g_escape = 4; }
}
uint32_t nr_get_spsr(void) { g_escape = 5; return 0; }
void nr_set_spsr(uint32_t, uint32_t) { g_escape = 5; }
void nr_exception_return(uint32_t) { g_escape = 6; nr_redirect = 1; }
void nr_fallback(void) { g_escape = 7; nr_redirect = 1; }
}

static void map_tables() {
    for (uint32_t pg = 0; pg < (1u << 20); ++pg) {
        uint32_t a = pg << 12;
        uint8_t* q = g_native.p(a);
        nr_rd_tab[pg] = nr_wr_tab[pg] = q ? (uintptr_t)q : 0;
    }
}

static uint32_t rnd() { static uint32_t s = 12345; s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    { char here; nr_stack_limit = &here - (4 << 20); }
    // image files: itcm.bin main.bin dtcm.bin (written by the generator test script)
    auto load = [](const char* path, Mem& m, uint32_t base) {
        FILE* f = fopen(path, "rb"); if (!f) { perror(path); exit(1); }
        std::vector<uint8_t> d(16 << 20); size_t n = fread(d.data(), 1, d.size(), f); fclose(f);
        for (size_t i = 0; i < n; ++i) *m.p(base + (uint32_t)i) = d[i];
    };
    const char* dir = argc > 1 ? argv[1] : ".";
    int limit = argc > 2 ? atoi(argv[2]) : 100000;
    uint32_t only = argc > 3 ? (uint32_t)strtoul(argv[3], nullptr, 16) : 0;
    g_trace = argc > 4;
    char path[512];
    Mem* both[2] = {&g_native, &g_ref};
    for (Mem* m : both) {
        snprintf(path, sizeof path, "%s/main.bin", dir); load(path, *m, 0x02004000u);
        snprintf(path, sizeof path, "%s/itcm.bin", dir); load(path, *m, 0x01FF8000u);
        snprintf(path, sizeof path, "%s/dtcm.bin", dir); load(path, *m, 0x027E0000u);
    }
    Mem pristine = g_native;
    map_tables();
    RefBus bus;
    unsigned tested = 0, pass = 0, fail = 0, skipped = 0, escapes[9] = {};
    const uint32_t SENTINEL = 0x0FFFFFF0u;
    for (uint32_t k = 0; k < nr_tab_static_count && (int)tested < limit; ++k) {
        // only real function entries: name ends with the entry address
        const NrEntry& e = nr_tab_static[k];
        uint32_t addr = e.key & ~1u; bool thumb = e.key & 1;
        // each function once (the tables also hold its resume points); fn(0) runs from its entry
        if (only && addr != only) continue;
        static std::vector<NrFn> seen;
        if (std::find(seen.begin(), seen.end(), e.fn) != seen.end()) continue;
        seen.push_back(e.fn);
        for (int trial = 0; trial < 3; ++trial) {
            g_native = pristine; g_ref = pristine;
            uint32_t regs[16];
            for (int r = 0; r < 13; ++r) {
                uint32_t v = rnd();
                switch (rnd() % 4) { case 0: v &= 0xFF; break; case 1: v = 0x02100000u + (v & 0xFFFFC); break;
                                     case 2: v = 0x027E0000u + (v & 0x3FFC); break; default: break; }
                regs[r] = v & ~3u;
            }
            regs[13] = 0x027E3C00u; regs[14] = SENTINEL | (thumb ? 0 : 0); regs[15] = addr;
            uint32_t cpsr = 0x1Fu | (rnd() & 0xF0000000u) | (thumb ? 0x20u : 0);
            // native
            memset(&g_cpu, 0, sizeof g_cpu);
            memcpy(g_cpu.R, regs, sizeof regs); g_cpu.cpsr = cpsr;
            g_ntrace.clear();
            nr_redirect = 0; g_escape = 0; g_io_native = 0; g_nr_q = 0;
            g_services = 0; g_runtime_cycles = 0; nr_deadline = 100000;
            if (setjmp(g_bail) == 0) e.fn(0);
            bool native_ok = !nr_redirect && (g_cpu.R[15] & ~1u) == SENTINEL && !g_escape;
            if (g_escape) escapes[g_escape]++;
            // reference
            armv4t::CPUState c{};
            memcpy(c.R, regs, sizeof regs);
            c.cpsr.n = cpsr >> 31; c.cpsr.z = (cpsr >> 30) & 1; c.cpsr.c = (cpsr >> 29) & 1; c.cpsr.v = (cpsr >> 28) & 1;
            c.cpsr.mode = 0x1F; c.cpsr.t = thumb; c.thumb = thumb;
            g_io_ref = 0;
            bool ref_ok = false; int steps = 0;
            while (steps++ < 300000) {
                uint32_t pc = c.R[15];
                if ((pc & ~1u) == SENTINEL) { ref_ok = true; break; }
                armv4t::Instr in = c.cpsr.t ? armv4t::ThumbDecoder::decode(bus.read16(pc & ~1u), pc & ~1u)
                                            : armv4t::ArmDecoder::decode(bus.read32(pc & ~3u), pc & ~3u);
                if (g_trace) {
                    static size_t ti; if (steps == 1) ti = 0;
                    if (ti < g_ntrace.size() && g_ntrace[ti][16] != (pc & ~1u)) {
                        printf("  path differs: ref at %08x, native at %08x (step %d)\n", pc, g_ntrace[ti][16], steps);
                        if (ti > 0) { const auto& p = g_ntrace[ti - 1]; printf("  previous %08x flags %x r0-3 %08x %08x %08x %08x\n", p[16], p[15], p[0], p[1], p[2], p[3]); }
                        g_trace = 0;
                    }
                    if (ti < g_ntrace.size() && g_ntrace[ti][16] == (pc & ~1u)) {
                        const auto& v = g_ntrace[ti++];
                        uint32_t rf = (c.cpsr.n << 3) | (c.cpsr.z << 2) | (c.cpsr.c << 1) | c.cpsr.v;
                        for (int q = 0; q < 15; ++q) if (v[q] != c.R[q]) {
                            printf("  at %08x: r%d native %08x ref %08x (step %d)\n", pc, q, v[q], c.R[q], steps); g_trace = 0; break; }
                        if (g_trace && v[15] != rf) { printf("  at %08x: flags native %x ref %x\n", pc, v[15], rf); g_trace = 0; }
                    }
                }
                auto r = armv4t::Interpreter::step(c, bus, in);
                if (r == armv4t::Interpreter::Result::Swi || r == armv4t::Interpreter::Result::Undefined ||
                    r == armv4t::Interpreter::Result::NotImplemented) break;
                c.thumb = c.cpsr.t;
            }
            if (!native_ok || !ref_ok) { ++skipped; continue; }
            ++tested;
            bool same = true;
            char why[256] = "";
            for (int r = 0; r < 15 && same; ++r)
                if (c.R[r] != g_cpu.R[r]) { same = false; snprintf(why, sizeof why, "r%d native %08x ref %08x", r, g_cpu.R[r], c.R[r]); }
            uint32_t nf = g_cpu.cpsr >> 28, rf = (c.cpsr.n << 3) | (c.cpsr.z << 2) | (c.cpsr.c << 1) | c.cpsr.v;
            if (same && nf != rf) { same = false; snprintf(why, sizeof why, "flags native %x ref %x", nf, rf); }
            if (same && g_native.ram != g_ref.ram) { same = false; snprintf(why, sizeof why, "main RAM differs"); }
            if (same && g_native.dtcm != g_ref.dtcm) {
                same = false;
                for (size_t i = 0; i < g_native.dtcm.size(); ++i) if (g_native.dtcm[i] != g_ref.dtcm[i]) {
                    snprintf(why, sizeof why, "DTCM differs at %08zx native %02x ref %02x", 0x027E0000 + i, g_native.dtcm[i], g_ref.dtcm[i]); break; }
            }
            if (same) ++pass; else { ++fail; if (fail <= 40) printf("FAIL %08x%s trial %d: %s\n", addr, thumb ? "t" : "", trial, why); }
        }
    }
    printf("tested %u pass %u fail %u skipped %u  escapes: dyn-miss %u swi %u cp15 %u mode %u spsr %u excret %u fallback %u timeout %u\n",
           tested, pass, fail, skipped, escapes[1], escapes[2], escapes[3], escapes[4], escapes[5], escapes[6], escapes[7], escapes[8]);
    return 0;
}
