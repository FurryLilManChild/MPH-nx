// MPH-nx host boot test: install the native ARM9 engine before upstream
// main() runs (the Switch build does this from nx_main.cpp). NR_DISABLE=1
// leaves it out, so the ARM9 runs on upstream's interpreter as a reference.
#include <cstdlib>
extern "C" void nr_install(void);

namespace {
struct Install {
    Install() {
        const char* off = std::getenv("NR_DISABLE");
        if (!off || off[0] != '1') nr_install();
    }
} g_install;
}
