// MPH-nx: Horizon platform fixes.
//
// * pthread_create: libnx gives 128 KiB stacks and starts every thread on
//   core 0. Recompiled code uses big frames, and the runner's worker pools
//   (2D scanlines, software 3D) must not share core 0 with the emulation
//   thread. Default to 1 MiB stacks and place new threads on cores 1 and 2.
// * pthread_detach returns ENOSYS on libnx, which makes std::thread::detach
//   throw. Report success instead (the thread still runs to completion).
// * CPU clock boost through clkrst, restored on exit.

#include <switch.h>

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#include "nx_platform.h"

int __real_pthread_create(pthread_t* t, const pthread_attr_t* attr,
                          void* (*fn)(void*), void* arg);
int __real_pthread_detach(pthread_t t);

typedef struct {
    void* (*fn)(void*);
    void* arg;
    int core;
} NxTrampoline;

static atomic_uint g_next_core = 0;

static void* nx_thread_trampoline(void* p) {
    NxTrampoline tr = *(NxTrampoline*)p;
    free(p);
    svcSetThreadCoreMask(CUR_THREAD_HANDLE, tr.core, 1u << tr.core);
    return tr.fn(tr.arg);
}

int __wrap_pthread_create(pthread_t* t, const pthread_attr_t* attr,
                          void* (*fn)(void*), void* arg) {
    NxTrampoline* tr = (NxTrampoline*)malloc(sizeof(NxTrampoline));
    if (!tr) return ENOMEM;
    tr->fn = fn;
    tr->arg = arg;
    tr->core = 1 + (int)(atomic_fetch_add(&g_next_core, 1u) % 2u);  // cores 1, 2

    int rc;
    if (!attr) {
        pthread_attr_t a;
        pthread_attr_init(&a);
        pthread_attr_setstacksize(&a, 1024 * 1024);
        rc = __real_pthread_create(t, &a, nx_thread_trampoline, tr);
        pthread_attr_destroy(&a);
    } else {
        rc = __real_pthread_create(t, attr, nx_thread_trampoline, tr);
    }
    if (rc != 0) free(tr);
    return rc;
}

int __wrap_pthread_detach(pthread_t t) {
    int rc = __real_pthread_detach(t);
    return rc == ENOSYS ? 0 : rc;
}

// ---------------------------------------------------------------------------
// POSIX calls newlib/libnx do not provide
// ---------------------------------------------------------------------------

#include <signal.h>
#include <unistd.h>

// host_info.cpp: CPU count and RAM for the diagnostics session header.
long sysconf(int name) {
    switch (name) {
    case _SC_NPROCESSORS_ONLN:
    case _SC_NPROCESSORS_CONF:
        return 3;  // cores available to an application
    case _SC_PAGESIZE:
        return 0x1000;
    case _SC_PHYS_PAGES: {
        u64 total = 0;
        svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
        return (long)(total / 0x1000);
    }
    default:
        errno = EINVAL;
        return -1;
    }
}

// libslirp's fork/exec helper (never reached: networking is off).
pid_t setsid(void) {
    errno = ENOSYS;
    return (pid_t)-1;
}

int sigprocmask(int how, const sigset_t* set, sigset_t* old) {
    (void)how; (void)set;
    if (old) sigemptyset(old);
    return 0;
}

// ---------------------------------------------------------------------------
// Clocks
// ---------------------------------------------------------------------------

#define NX_CPU_BOOST_HZ 1785000000u

static bool g_clk_open = false;
static ClkrstSession g_cpu_session;
static u32 g_cpu_saved_hz = 0;
static u32 g_cpu_applied_hz = 0;
static char g_clk_status[96] = "clocks: untouched";
static bool g_gpu_open = false, g_gpu_tried = false;
static ClkrstSession g_gpu_session;

void nx_clocks_apply(void) {
    if (hosversionBefore(8, 0, 0)) {
        snprintf(g_clk_status, sizeof g_clk_status, "clocks: firmware too old for clkrst");
        return;
    }
    if (R_FAILED(clkrstInitialize())) {
        snprintf(g_clk_status, sizeof g_clk_status, "clocks: clkrst unavailable");
        return;
    }
    if (R_FAILED(clkrstOpenSession(&g_cpu_session, PcvModuleId_CpuBus, 3))) {
        clkrstExit();
        snprintf(g_clk_status, sizeof g_clk_status, "clocks: cpu session failed");
        return;
    }
    g_clk_open = true;
    clkrstGetClockRate(&g_cpu_session, &g_cpu_saved_hz);
    if (R_SUCCEEDED(clkrstSetClockRate(&g_cpu_session, NX_CPU_BOOST_HZ)))
        g_cpu_applied_hz = NX_CPU_BOOST_HZ;
    printf("[mph-nx] cpu clock %u MHz -> %u MHz\n", g_cpu_saved_hz / 1000000u,
           g_cpu_applied_hz / 1000000u);
}

void nx_clocks_restore(void) {
    if (!g_clk_open) return;
    if (g_cpu_saved_hz) clkrstSetClockRate(&g_cpu_session, g_cpu_saved_hz);
    if (g_gpu_open) { clkrstCloseSession(&g_gpu_session); g_gpu_open = false; }
    clkrstCloseSession(&g_cpu_session);
    clkrstExit();
    g_clk_open = false;
}

// Port menu Speed > CPU clock: 1 boost (NX_CPU_BOOST_HZ), 0 stock 1020 MHz.
void nx_clocks_set_boost(int on) {
    if (!g_clk_open) return;
    const u32 hz = on ? NX_CPU_BOOST_HZ : 1020000000u;
    if (R_SUCCEEDED(clkrstSetClockRate(&g_cpu_session, hz))) g_cpu_applied_hz = hz;
    u32 now = 0;
    clkrstGetClockRate(&g_cpu_session, &now);
    printf("[mph-nx] cpu clock set to %u MHz (reads back %u MHz)\n", hz / 1000000u, now / 1000000u);
}

// native11: the clocks actually running (a sys-clk profile included), for
// the 10 s stats line. The GPU session is opened on first use.
unsigned nx_clocks_cpu_mhz(void) {
    if (!g_clk_open) return 0;
    u32 now = 0;
    clkrstGetClockRate(&g_cpu_session, &now);
    return now / 1000000u;
}
unsigned nx_clocks_gpu_mhz(void) {
    if (!g_clk_open) return 0;
    if (!g_gpu_tried) {
        g_gpu_tried = true;
        g_gpu_open = R_SUCCEEDED(clkrstOpenSession(&g_gpu_session, PcvModuleId_GPU, 3));
    }
    if (!g_gpu_open) return 0;
    u32 now = 0;
    clkrstGetClockRate(&g_gpu_session, &now);
    return now / 1000000u;
}

const char* nx_clocks_status(void) {
    if (g_clk_open) {
        u32 now = 0;
        clkrstGetClockRate(&g_cpu_session, &now);
        snprintf(g_clk_status, sizeof g_clk_status, "cpu=%uMHz", now / 1000000u);
    }
    return g_clk_status;
}

// ---- MPH-nx direct input (touchscreen and touch-cursor buttons) ----------
// SDL's own touch path only runs while a window has focus and no text input
// is active, and drops touches silently otherwise. Reading HID directly keeps
// touch independent of that. The pad state is our own; SDL configured the
// npad styles, and reading shared memory does not disturb SDL's reads.
void mphnx_input_poll(MphnxInput* out) {
    static PadState pad;
    static int ready = 0;
    if (!ready) {
        hidInitializeTouchScreen();
        padInitializeDefault(&pad);
        ready = 1;
    }
    out->touch_count = 0;
    out->touch_x = out->touch_y = 0;
    HidTouchScreenState ts = {0};
    if (hidGetTouchScreenStates(&ts, 1) && ts.count > 0) {
        out->touch_count = (int)ts.count;
        out->touch_x = (int)ts.touches[0].x;
        out->touch_y = (int)ts.touches[0].y;
    }
    padUpdate(&pad);
    const u64 held = padGetButtons(&pad);
    out->zl = (held & HidNpadButton_ZL) != 0;
    out->a = (held & HidNpadButton_A) != 0;
    out->minus = (held & HidNpadButton_Minus) != 0;
    out->b = (held & HidNpadButton_B) != 0;
    out->up = (held & HidNpadButton_Up) != 0;
    out->down = (held & HidNpadButton_Down) != 0;
    out->left = (held & HidNpadButton_Left) != 0;
    out->right = (held & HidNpadButton_Right) != 0;
    const HidAnalogStickState ls = padGetStickPos(&pad, 0);
    out->lx = (float)ls.x / 32767.0f;
    out->ly = -(float)ls.y / 32767.0f;
}
