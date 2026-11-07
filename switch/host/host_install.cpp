// MPH-nx host boot test: install the native ARM9 engine before upstream
// main() runs (the Switch build does this from nx_main.cpp).
extern "C" void nr_install(void);

namespace {
struct Install { Install() { nr_install(); } } g_install;
}
