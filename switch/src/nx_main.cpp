// MPH-nx: Nintendo Switch entry point for the ndsrecomp runner.
//
// Sets up the Horizon environment (logs on the SD card, crash handler,
// clocks), finds the player's ROM next to the NRO, builds the runner's
// command line and calls the upstream main (compiled as nds_upstream_main).

#include <switch.h>

#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <string>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "nx_platform.h"

int nds_upstream_main(int argc, char** argv);  // upstream main.cpp, renamed at compile time

#ifndef MPHNX_VERSION
#define MPHNX_VERSION "switch-test1"
#endif

static const char* kAppDir = "/switch/mph-nx";
static const char* kLogDir = "/switch/mph-nx/logs";

// ---------------------------------------------------------------------------
// Logs: one file per stream, previous launch kept as *.prev.*
// ---------------------------------------------------------------------------

static void rotate(const std::string& cur, const std::string& prev) {
    struct stat st{};
    if (stat(cur.c_str(), &st) != 0) return;
    // FAT refuses a rename onto an existing file: delete first.
    remove(prev.c_str());
    rename(cur.c_str(), prev.c_str());
}

static void open_logs() {
    mkdir(kAppDir, 0777);
    mkdir(kLogDir, 0777);
    const std::string out = std::string(kLogDir) + "/mph-nx.log";
    const std::string err = std::string(kLogDir) + "/mph-nx.err.log";
    rotate(out, std::string(kLogDir) + "/mph-nx.prev.log");
    rotate(err, std::string(kLogDir) + "/mph-nx.prev.err.log");
    if (freopen(out.c_str(), "a", stdout)) setvbuf(stdout, nullptr, _IOLBF, 0);
    if (freopen(err.c_str(), "a", stderr)) setvbuf(stderr, nullptr, _IOLBF, 0);
}

// ---------------------------------------------------------------------------
// Crash handler: writes through fsFileWrite only (no malloc, no stdio).
// ---------------------------------------------------------------------------

extern "C" {
alignas(16) u8 __nx_exception_stack[0x10000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);
}

static u64 g_module_base = 0;
static u64 g_module_size = 0;
static u32 g_crash_id = 0;

static void find_module_range() {
    MemoryInfo mi{};
    u32 pi = 0;
    if (R_SUCCEEDED(svcQueryMemory(&mi, &pi, (u64)&find_module_range))) {
        // Walk back to the start of the module's contiguous mappings.
        u64 base = mi.addr;
        for (;;) {
            MemoryInfo prev{};
            if (base == 0 || R_FAILED(svcQueryMemory(&prev, &pi, base - 1))) break;
            if (prev.type != MemType_CodeStatic && prev.type != MemType_CodeMutable) break;
            base = prev.addr;
        }
        u64 end = mi.addr + mi.size;
        for (;;) {
            MemoryInfo next{};
            if (R_FAILED(svcQueryMemory(&next, &pi, end))) break;
            if (next.type != MemType_CodeStatic && next.type != MemType_CodeMutable) break;
            end = next.addr + next.size;
        }
        g_module_base = base;
        g_module_size = end - base;
    }
}

static int fmt_hex(char* p, u64 v) {
    static const char hx[] = "0123456789abcdef";
    char tmp[16];
    int n = 0;
    do { tmp[n++] = hx[v & 15]; v >>= 4; } while (v);
    for (int i = 0; i < n; ++i) p[i] = tmp[n - 1 - i];
    return n;
}

static int put(char* buf, int pos, const char* s) {
    while (*s && pos < 4000) buf[pos++] = *s++;
    return pos;
}

static int put_reg(char* buf, int pos, const char* name, u64 v) {
    pos = put(buf, pos, name);
    pos = put(buf, pos, "=0x");
    pos += fmt_hex(buf + pos, v);
    if (g_module_base && v >= g_module_base && v < g_module_base + g_module_size) {
        pos = put(buf, pos, " (+0x");
        pos += fmt_hex(buf + pos, v - g_module_base);
        pos = put(buf, pos, ")");
    }
    pos = put(buf, pos, "\n");
    return pos;
}

extern "C" void __libnx_exception_handler(ThreadExceptionDump* ctx) {
    static char buf[4096];
    int pos = 0;
    pos = put(buf, pos, "MPH-nx crash, build " MPHNX_VERSION "\n");
    pos = put_reg(buf, pos, "error_desc", ctx->error_desc);
    pos = put_reg(buf, pos, "module_base", g_module_base);
    pos = put_reg(buf, pos, "module_size", g_module_size);
    pos = put_reg(buf, pos, "pc", ctx->pc.x);
    pos = put_reg(buf, pos, "lr", ctx->lr.x);
    pos = put_reg(buf, pos, "sp", ctx->sp.x);
    pos = put_reg(buf, pos, "far", ctx->far.x);
    for (int i = 0; i < 29; ++i) {
        char name[4] = {'x', (char)('0' + i / 10), (char)('0' + i % 10), 0};
        pos = put_reg(buf, pos, name, ctx->cpu_gprs[i].x);
    }
    pos = put_reg(buf, pos, "fp", ctx->fp.x);
    // Return addresses found on the stack inside the module (cheap backtrace).
    pos = put(buf, pos, "stack:\n");
    u64* sp = (u64*)ctx->sp.x;
    int found = 0;
    for (int i = 0; i < 512 && found < 32 && pos < 3900; ++i) {
        MemoryInfo mi{}; u32 pi = 0;
        if (R_FAILED(svcQueryMemory(&mi, &pi, (u64)&sp[i])) || !(mi.perm & Perm_R)) break;
        u64 v = sp[i];
        if (g_module_base && v >= g_module_base && v < g_module_base + g_module_size) {
            pos = put_reg(buf, pos, "  ret", v);
            ++found;
        }
    }

    FsFileSystem* fs = fsdevGetDeviceFileSystem("sdmc");
    if (fs) {
        char path[96] = "/switch/mph-nx/logs/crash-";
        int p = (int)strlen(path);
        p += fmt_hex(path + p, g_crash_id);
        path[p++] = '.'; path[p++] = 't'; path[p++] = 'x'; path[p++] = 't'; path[p] = 0;
        fsFsDeleteFile(fs, path);
        if (R_SUCCEEDED(fsFsCreateFile(fs, path, pos, 0))) {
            FsFile f;
            if (R_SUCCEEDED(fsFsOpenFile(fs, path, FsOpenMode_Write, &f))) {
                fsFileWrite(&f, 0, buf, pos, FsWriteOption_Flush);
                fsFileClose(&f);
            }
        }
    }
    nx_clocks_restore();
    svcExitProcess();
}

static void on_terminate() {
    const char* what = "unknown";
    try {
        if (auto e = std::current_exception()) std::rethrow_exception(e);
    } catch (const std::exception& e) {
        what = e.what();
    } catch (...) {
    }
    std::fprintf(stderr, "[mph-nx] fatal: uncaught exception: %s\n", what);
    std::fflush(stderr);
    nx_clocks_restore();
    svcExitProcess();
}

// ---------------------------------------------------------------------------
// ROM discovery: any 64 MiB .nds in the app folder (the runner verifies SHA-1)
// ---------------------------------------------------------------------------

static bool ends_with_nds(const char* n) {
    size_t l = strlen(n);
    if (l < 4) return false;
    const char* e = n + l - 4;
    return e[0] == '.' && (e[1] | 32) == 'n' && (e[2] | 32) == 'd' && (e[3] | 32) == 's';
}

static std::string find_rom() {
    DIR* d = opendir(kAppDir);
    if (!d) return {};
    std::string found;
    while (dirent* de = readdir(d)) {
        if (!ends_with_nds(de->d_name)) continue;
        std::string p = std::string(kAppDir) + "/" + de->d_name;
        struct stat st{};
        if (stat(p.c_str(), &st) == 0 && st.st_size == 64 * 1024 * 1024) {
            found = p;
            break;
        }
    }
    closedir(d);
    return found;
}

// ---------------------------------------------------------------------------
// Simple on-screen message when the app cannot start
// ---------------------------------------------------------------------------

static void show_message_and_wait(const char* text) {
    consoleInit(nullptr);
    std::printf("\n  MPH-nx (%s)\n\n", MPHNX_VERSION);
    std::printf("%s\n\n  Press + to exit.\n", text);
    consoleUpdate(nullptr);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
        consoleUpdate(nullptr);
    }
    consoleExit(nullptr);
}

// ---------------------------------------------------------------------------
// Heartbeat: proves the process is alive and shows memory use over time.
// ---------------------------------------------------------------------------

static std::atomic<bool> g_heartbeat_run{true};
static Thread g_heartbeat;

static void heartbeat_main(void*) {
    u64 start = armGetSystemTick();
    while (g_heartbeat_run.load()) {
        svcSleepThread(5000000000ull);
        if (!g_heartbeat_run.load()) break;
        u64 used = 0, total = 0;
        svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
        svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
        double secs = (double)armTicksToNs(armGetSystemTick() - start) / 1e9;
        std::fprintf(stdout, "[heartbeat] t=%.0fs mem=%lluMB/%lluMB %s\n", secs,
                     (unsigned long long)(used >> 20), (unsigned long long)(total >> 20),
                     nx_clocks_status());
        std::fflush(stdout);
    }
}

// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    g_crash_id = (u32)(armGetSystemTick() ^ (armGetSystemTick() >> 32));
    find_module_range();
    open_logs();
    std::set_terminate(on_terminate);

    std::printf("[mph-nx] %s starting\n", MPHNX_VERSION);
    std::printf("[mph-nx] module base 0x%llx size 0x%llx crash id %08x\n",
                (unsigned long long)g_module_base, (unsigned long long)g_module_size, g_crash_id);

    const AppletType at = appletGetAppletType();
    u64 total = 0;
    svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    std::printf("[mph-nx] applet type %d, memory %llu MB\n", (int)at, (unsigned long long)(total >> 20));
    if (at != AppletType_Application && at != AppletType_SystemApplication) {
        show_message_and_wait(
            "  Launch MPH-nx through title takeover:\n"
            "  hold R while opening any game, then pick MPH-nx.\n"
            "  The Album (applet mode) does not have enough memory.");
        return 0;
    }

    const std::string rom = find_rom();
    if (rom.empty()) {
        show_message_and_wait(
            "  No ROM found.\n"
            "  Put your Metroid Prime Hunters (USA, revision 0) .nds file\n"
            "  in /switch/mph-nx/ on the SD card, next to mph-nx.nro.");
        return 0;
    }
    std::printf("[mph-nx] rom: %s\n", rom.c_str());

    nx_clocks_apply();

    threadCreate(&g_heartbeat, heartbeat_main, nullptr, nullptr, 0x4000, 0x3F, 0);
    threadStart(&g_heartbeat);

    const std::string app = kAppDir;
    const std::string diag = app + "/diagnostics";
    mkdir((app + "/bios").c_str(), 0777);
    mkdir(diag.c_str(), 0777);

    std::vector<std::string> args;
    auto add = [&](const std::string& s) { args.push_back(s); };
    add(app + "/mph-nx.nro");
    add(app + "/bios");                       // positional: bios dir (FreeBIOS used)
    add("--rom"); add(rom);
    add("--config"); add(app + "/game.toml");
    add("--freebios");
    add("--boot"); add("direct");
    add("--generated-firmware");
    add("--interactive");
    add("--screen-layout"); add("stacked");
    add("--fullscreen"); add("borderless");   // fill 1280x720, aspect kept by SDL logical size
    add("--mph-prime-controls"); add("on");
    add("--save-path"); add(app + "/mph.sav");
    add("--firmware-state-path"); add(app + "/firmware-state.bin");
    add("--runtime-settings-path"); add(app + "/runtime-settings.ini");
    add("--diagnostics"); add("on");
    add("--diagnostics-dir"); add(diag);
    add("--diagnostics-interval-ms"); add("2000");
    add("--discover-static-misses");
    add("--network"); add("off");
    add("--wfc"); add("off");
    add("--performance-governor"); add("off");
    add("--internal-resolution"); add("1");
    // Build 1 measures the plain DS workload: no widened compositor pass.
    add("--adaptive-widescreen"); add("none");

    std::vector<char*> cargv;
    std::printf("[mph-nx] args:");
    for (auto& s : args) {
        cargv.push_back(const_cast<char*>(s.c_str()));
        std::printf(" %s", s.c_str());
    }
    std::printf("\n");
    cargv.push_back(nullptr);
    std::fflush(stdout);

    const int rc = nds_upstream_main((int)args.size(), cargv.data());

    std::printf("[mph-nx] runner returned %d\n", rc);
    g_heartbeat_run.store(false);
    threadWaitForExit(&g_heartbeat);
    threadClose(&g_heartbeat);
    nx_clocks_restore();
    std::fflush(stdout);
    std::fflush(stderr);
    return rc;
}
