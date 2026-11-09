// MPH-nx host boot test: install the native ARM9 engine before upstream
// main() runs (the Switch build does this from nx_main.cpp).
//   NR_DISABLE=1        leave it out: the ARM9 runs on upstream's interpreter
//                       as a reference
//   NR_DUMP=a:n,a:n     at exit, print n words from each guest address a
//   NR_SHOT=file.ppm    at exit, write both screens (top above bottom)
//   NR_STATE_SAVE=file@cycles  save a state once ARM9 time passes cycles
//   NR_STATE_LOAD=file@cycles  load a state once ARM9 time passes cycles
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include "gpu2d.h"
void nds_gpu2d_cow_stats(uint64_t* pages, uint64_t* fallbacks, uint64_t* page_mask);
extern "C" void nr_install(void);
extern bool g_nx_fast_card;
extern "C" uint32_t bus_read_u32_slow(uint32_t addr);
const uint32_t* nds_gpu2d_framebuffer(int screen);

extern unsigned long long g_nx_dma_blocks, g_nx_dma_block_units;   // io.cpp (native9)
extern bool g_nx_dma_block;
#include "savestate.h"
#include "../native/nr_state.h"
extern void (*g_nx_host_round_hook)(uint64_t arm9_cycles);

namespace {
void shot_at_exit() {
    const char* f = std::getenv("NR_SHOT");
    if (!f) return;
    std::FILE* o = std::fopen(f, "wb");
    if (!o) return;
    std::fprintf(o, "P6\n256 384\n255\n");
    for (int sc = 0; sc < 2; ++sc) {
        const uint32_t* fb = nds_gpu2d_framebuffer(sc);
        for (int i = 0; i < 256 * 192; ++i) {
            const uint32_t p = fb ? fb[i] : 0;
            const unsigned char rgb[3] = {(unsigned char)(p >> 16), (unsigned char)(p >> 8), (unsigned char)p};
            std::fwrite(rgb, 1, 3, o);
        }
    }
    std::fclose(o);
}
void gpu2d_stats_at_exit() {
    NdsGpu2dProfile p{};
    nds_gpu2d_profile(&p);
    static const char* names[] = {"vram", "vramcnt", "palette", "oam", "frame", "present", "slots", "capture"};
    std::fprintf(stderr, "[gpu2d] threaded %llu inline %llu helped %llu wait %.1f ms\n",
                 (unsigned long long)p.threaded_lines, (unsigned long long)p.inline_lines,
                 (unsigned long long)p.fence_helped_lines, p.fence_wait_ns / 1e6);
    for (int i = 0; i < NDS_GPU2D_FENCE_CAUSE_COUNT && i < 8; ++i)
        std::fprintf(stderr, "[gpu2d]   fence %-8s drains %llu lines %llu\n", names[i],
                     (unsigned long long)p.fence_drains[i], (unsigned long long)p.fenced_lines[i]);
    uint64_t cp = 0, cf = 0, cm = 0;
    nds_gpu2d_cow_stats(&cp, &cf, &cm);
    std::fprintf(stderr, "[gpu2d] cow pages %llu fallbacks %llu page mask %llx\n",
                 (unsigned long long)cp, (unsigned long long)cf, (unsigned long long)cm);
    std::fprintf(stderr, "[dma] video blocks %llu units %llu (bulk %s)\n", g_nx_dma_blocks, g_nx_dma_block_units,
                 g_nx_dma_block ? "on" : "off");
}
void dump_at_exit() {
    gpu2d_stats_at_exit();
    shot_at_exit();
    const char* d = std::getenv("NR_DUMP");
    if (!d) return;
    std::string s(d);
    size_t p = 0;
    while (p < s.size()) {
        size_t q = s.find(',', p); if (q == std::string::npos) q = s.size();
        const std::string item = s.substr(p, q - p);
        const size_t c = item.find(':');
        const uint32_t a = (uint32_t)std::strtoul(item.substr(0, c).c_str(), nullptr, 16);
        const uint32_t n = c == std::string::npos ? 4 : (uint32_t)std::strtoul(item.substr(c + 1).c_str(), nullptr, 0);
        std::fprintf(stderr, "[dump] %08X:", a);
        for (uint32_t i = 0; i < n; ++i) std::fprintf(stderr, " %08X", bus_read_u32_slow(a + 4 * i));
        std::fprintf(stderr, "\n");
        p = q + 1;
    }
}
struct StateStep { std::string path; unsigned long long at = 0; bool done = true; };
StateStep g_save_step, g_load_step;
StateStep parse_step(const char* v) {
    StateStep st;
    if (!v) return st;
    std::string s(v);
    const size_t at = s.find('@');
    st.path = s.substr(0, at);
    st.at = at == std::string::npos ? 0 : std::strtoull(s.substr(at + 1).c_str(), nullptr, 0);
    st.done = false;
    return st;
}
void round_hook(uint64_t cyc) {
    const NdsSavestateIdentity id{"mphnx-host", "a8679d4f8dcb445cbe946a424241ef44901b0caf"};
    static unsigned waits = 0;
    if (!g_save_step.done && cyc >= g_save_step.at) {
        if (!nx_state_ready()) { ++waits; return; }
        std::string err;
        const bool ok = nx_state_save(g_save_step.path, id, &err);
        std::fprintf(stderr, "[state] save at %llu cycles (%u rounds waited): %s %s\n",
                     (unsigned long long)cyc, waits, ok ? "ok" : "FAILED", err.c_str());
        g_save_step.done = true;
    }
    if (!g_load_step.done && cyc >= g_load_step.at) {
        std::string err;
        const bool ok = nx_state_load(g_load_step.path, id, &err);
        std::fprintf(stderr, "[state] load at %llu cycles: %s %s\n", (unsigned long long)cyc,
                     ok ? "ok" : "FAILED", err.c_str());
        g_load_step.done = true;
    }
}
struct Install {
    Install() {
        const char* off = std::getenv("NR_DISABLE");
        if (!off || off[0] != '1') nr_install();
        if (const char* fc = std::getenv("NR_FASTCARD")) g_nx_fast_card = fc[0] == '1';
        if (std::getenv("NR_DMA_BLOCK_OFF")) g_nx_dma_block = false;
        std::atexit(dump_at_exit);
        g_save_step = parse_step(std::getenv("NR_STATE_SAVE"));
        g_load_step = parse_step(std::getenv("NR_STATE_LOAD"));
        if (!g_save_step.done || !g_load_step.done) g_nx_host_round_hook = round_hook;
    }
} g_install;
}
