// MPH-nx host boot test: install the native ARM9 engine before upstream
// main() runs (the Switch build does this from nx_main.cpp).
//   NR_DISABLE=1        leave it out: the ARM9 runs on upstream's interpreter
//                       as a reference
//   NR_DUMP=a:n,a:n     at exit, print n words from each guest address a
//   NR_SHOT=file.ppm    at exit, write both screens (top above bottom)
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
extern "C" void nr_install(void);
extern bool g_nx_fast_card;
extern "C" uint32_t bus_read_u32_slow(uint32_t addr);
const uint32_t* nds_gpu2d_framebuffer(int screen);

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
void dump_at_exit() {
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
struct Install {
    Install() {
        const char* off = std::getenv("NR_DISABLE");
        if (!off || off[0] != '1') nr_install();
        if (const char* fc = std::getenv("NR_FASTCARD")) g_nx_fast_card = fc[0] == '1';
        std::atexit(dump_at_exit);
    }
} g_install;
}
