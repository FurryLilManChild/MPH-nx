// MPH-nx: deko3d screen presenter (see nx_present.h).
// Swapchain setup follows RSDuck's melonDS Switch port (frontend/switch/Gfx.cpp,
// GPL-3.0); the drawing is 2D-engine blits from CPU-written pitch-linear images.
#include "nx_present.h"
#include "nx_deko.h"

#include <switch.h>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

bool g_nx_display_lock = true;   // native8g: see nx_present_frame

namespace
{
constexpr int kFbW = 1280, kFbH = 720;
constexpr int kSlots = 3;
constexpr int kDsW = 256, kDsH = 192;
constexpr int kHudW = 256, kHudH = 16;
constexpr uint32_t kCmdSlotSize = 0x4000;

bool g_active = false;
dk::Queue g_queue;
dk::Swapchain g_swapchain;
dk::MemBlock g_fb_mem, g_src_mem, g_cmd_mem;
dk::Image g_fb[kSlots];
// Per swapchain slot: top, bottom, HUD. One extra black image for clears.
dk::Image g_src[kSlots][3];
uint32_t* g_src_cpu[kSlots][3];
uint32_t g_src_off[kSlots][3];
dk::Image g_black;
dk::CmdBuf g_cmd;
dk::Fence g_fence[kSlots];
bool g_fence_used[kSlots];

// Resolution scaler: render-size top screen composed by HdCompose.
constexpr int kHiMaxW = 1024, kHiMaxH = 768;
constexpr uint32_t kHdLayerBytes = kDsW * kDsH * 8;     // RG32 pairs
constexpr uint32_t kHdParamsBytes = 256;
dk::Shader g_compose;
bool g_compose_ok = false;
dk::MemBlock g_hd_in_mem, g_hd_out_mem;
uint32_t g_hd_in_off[kSlots], g_hd_out_off[kSlots];
// views of one buffer per render size: 1.5x, 2x, 3x, 4x
constexpr uint32_t kHdDims[4][2] = {{384, 288}, {512, 384}, {768, 576}, {1024, 768}};
dk::Image g_hd_out_img[kSlots][4];

// native16 HUD panels: when a frame has panels it is composed into this
// pitch-linear copy (blits), the panels are blended in by PanelBlend, and the
// copy is blitted to the swapchain image.
dk::Shader g_panel;
bool g_panel_ok = false;
dk::MemBlock g_comp_mem;
dk::Image g_comp;
struct PanelParams {
    uint32_t dst_x, dst_y, dst_w, dst_h, src_x, src_y, src_w, src_h, alpha_dark, alpha_bright, pitch;
};

// native8c diagnostics
std::atomic<unsigned> g_stat_hd[4] = {}, g_stat_refused{0}, g_stat_plain{0};
// native8g frame cadence: presents, frames dropped unshown, how many presents
// each 3D frame stayed up (1, 2, 3, 4+), 3D frames shown out of order, and
// the emulation thread's wait for the display.
std::atomic<unsigned> g_stat_presents{0}, g_stat_dropped{0}, g_stat_back{0};
std::atomic<unsigned> g_stat_run[5] = {};
std::atomic<uint64_t> g_stat_wait_ticks{0};
std::condition_variable g_taken_cv;   // the presenter took the ready job

struct HdParams {
    uint32_t out_w, out_h, bldcnt, eva, evb, evy, master_bright, prio3d, order3d,
             render_xpos, output_width, step_x, step_y;
};

// Presenter thread: the emulation thread copies a frame into `g_job[w]` and
// moves on; the thread presents the newest complete frame (an older one that
// was never shown is dropped, as a display at 60 Hz would drop it anyway).
struct Job {
    std::vector<uint32_t> top, bottom, hd_top, hd_below;
    NxPresentFrame f{};
    std::string hud, notice;
};
Job g_job[2];
int g_job_ready = -1;        // index of the newest unpresented job, -1 none
int g_job_busy = -1;         // index the thread is presenting
std::mutex g_job_m;
std::condition_variable g_job_cv;
std::thread g_thread;
void present_now(const NxPresentFrame& f);
void thread_main()
{
    for (;;) {
        int idx;
        {
            std::unique_lock<std::mutex> lock(g_job_m);
            g_job_cv.wait(lock, [] { return g_job_ready >= 0; });
            idx = g_job_ready;
            g_job_ready = -1;
            g_job_busy = idx;
        }
        g_taken_cv.notify_all();
        present_now(g_job[idx].f);
        {
            std::lock_guard<std::mutex> lock(g_job_m);
            g_job_busy = -1;
        }
    }
}

uint32_t align_up(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

void copy_screen(uint32_t* dst, const uint32_t* src, bool dim)
{
    if (!src) { std::memset(dst, 0, kDsW * kDsH * 4); return; }
    if (!dim) {
        for (int i = 0; i < kDsW * kDsH; ++i) dst[i] = src[i] | 0xFF000000u;
    } else {
        // ~40% brightness, like the old SDL alpha-150 black overlay.
        for (int i = 0; i < kDsW * kDsH; ++i) {
            const uint32_t p = src[i];
            dst[i] = 0xFF000000u | (((p >> 1) & 0x7F7F7Fu) - ((p >> 3) & 0x1F1F1Fu));
        }
    }
}

void put(uint32_t* buf, int w, int h, int x, int y, uint32_t c)
{
    if (x >= 0 && y >= 0 && x < w && y < h) buf[y * w + x] = c;
}

void draw_cursor(uint32_t* buf, float fx, float fy)
{
    const int x = (int)(fx + 0.5f), y = (int)(fy + 0.5f), r = 6;
    for (int d = -r - 1; d <= r + 1; ++d)
        for (int t = -1; t <= 1; ++t) {
            put(buf, kDsW, kDsH, x + d, y + t, 0xFF000000u);
            put(buf, kDsW, kDsH, x + t, y + d, 0xFF000000u);
        }
    for (int d = -r; d <= r; ++d) {
        put(buf, kDsW, kDsH, x + d, y, 0xFFFFFFFFu);
        put(buf, kDsW, kDsH, x, y + d, 0xFFFFFFFFu);
    }
}

int draw_text(uint32_t* buf, int w, int h, int x, int y, const char* text,
              const char* (*glyph)(char))
{
    int n = 0;
    for (const char* p = text; *p; ++p, ++n) {
        const char* g = glyph ? glyph(*p) : nullptr;
        if (!g) continue;
        for (int gy = 0; gy < 7; ++gy)
            for (int gx = 0; gx < 5; ++gx)
                if (g[gy * 5 + gx] == '1') put(buf, w, h, x + n * 6 + gx, y + gy, 0xFFFFFFFFu);
    }
    return n;
}

void draw_notice(uint32_t* buf, const char* text, const char* (*glyph)(char))
{
    constexpr int kCols = 40, kRows = 5;
    const int bx = 5, by = kDsH - kRows * 8 - 5;
    for (int yy = 0; yy < kRows * 8 + 4; ++yy)
        for (int xx = 0; xx < kCols * 6 + 6; ++xx)
            put(buf, kDsW, kDsH, bx + xx, by + yy, 0xFF0A0A0Au);
    int row = 0, col = 0;
    char one[2] = {0, 0};
    for (const char* p = text; *p && row < kRows; ++p) {
        if (*p == '\n' || col == kCols) {
            ++row; col = 0;
            if (*p == '\n') continue;
            if (row >= kRows) break;
        }
        one[0] = *p;
        draw_text(buf, kDsW, kDsH, bx + 3 + col * 6, by + 2 + row * 8, one, glyph);
        ++col;
    }
}

bool make_pitch_image(dk::Image& img, uint32_t*& cpu, uint32_t w, uint32_t h, uint32_t& offset,
                      uint32_t* where = nullptr)
{
    dk::ImageLayout layout;
    dk::ImageLayoutMaker{NxDeko::Device}
        .setFlags(DkImageFlags_PitchLinear | DkImageFlags_Usage2DEngine)
        .setFormat(DkImageFormat_BGRA8_Unorm)
        .setDimensions(w, h)
        .setPitchStride(w * 4)
        .initialize(layout);
    offset = align_up(offset, (uint32_t)layout.getAlignment());
    img.initialize(layout, g_src_mem, offset);
    if (where) *where = offset;
    cpu = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(g_src_mem.getCpuAddr()) + offset);
    offset += (uint32_t)layout.getSize();
    return true;
}

DkImageRect rect(const NxRect& r) { return {(uint32_t)r.x, (uint32_t)r.y, 0, (uint32_t)r.w, (uint32_t)r.h, 1}; }
}

bool nx_present_active() { return g_active; }

bool nx_present_init()
{
    if (g_active) return true;
    if (!NxDeko::Init()) return false;
    dk::Device dev = NxDeko::Device;

    g_queue = dk::QueueMaker{dev}
        .setFlags(DkQueueFlags_Graphics | DkQueueFlags_Compute | DkQueueFlags_MediumPrio | DkQueueFlags_DisableZcull)
        .create();
    if (!g_queue) { std::fprintf(stderr, "[present] queue failed\n"); return false; }

    dk::ImageLayout fb_layout;
    dk::ImageLayoutMaker{dev}
        .setFlags(DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_Usage2DEngine)
        .setFormat(DkImageFormat_RGBA8_Unorm)
        .setDimensions(kFbW, kFbH)
        .initialize(fb_layout);
    const uint32_t fb_align = (uint32_t)fb_layout.getAlignment();
    const uint32_t fb_size = align_up((uint32_t)fb_layout.getSize(), fb_align);
    g_fb_mem = dk::MemBlockMaker{dev, align_up(fb_size * kSlots, DK_MEMBLOCK_ALIGNMENT)}
        .setFlags(DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image)
        .create();
    std::array<DkImage const*, kSlots> fbs;
    for (int i = 0; i < kSlots; ++i) {
        g_fb[i].initialize(fb_layout, g_fb_mem, i * fb_size);
        fbs[i] = &g_fb[i];
    }

    const uint32_t src_bytes = align_up(
        kSlots * (2 * (kDsW * kDsH * 4 + 0x1000) + kHudW * kHudH * 4 + 0x1000) + 16 * 16 * 4 + 0x1000,
        DK_MEMBLOCK_ALIGNMENT);
    g_src_mem = dk::MemBlockMaker{dev, src_bytes}
        .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image)
        .create();
    uint32_t off = 0;
    for (int i = 0; i < kSlots; ++i) {
        make_pitch_image(g_src[i][0], g_src_cpu[i][0], kDsW, kDsH, off);
        make_pitch_image(g_src[i][1], g_src_cpu[i][1], kDsW, kDsH, off, &g_src_off[i][1]);
        make_pitch_image(g_src[i][2], g_src_cpu[i][2], kHudW, kHudH, off);
    }
    uint32_t* black = nullptr;
    make_pitch_image(g_black, black, 16, 16, off);
    for (int i = 0; i < 16 * 16; ++i) black[i] = 0xFF000000u;
    if (off > src_bytes) { std::fprintf(stderr, "[present] staging overflow\n"); return false; }

    g_cmd_mem = dk::MemBlockMaker{dev, align_up(kCmdSlotSize * kSlots, DK_MEMBLOCK_ALIGNMENT)}
        .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
        .create();
    g_cmd = dk::CmdBufMaker{dev}.create();

    // Resolution scaler resources
    g_compose_ok = NxDeko::LoadShader("HdCompose", g_compose);
    {
        const uint32_t in_slot = align_up(2 * kHdLayerBytes + kHdParamsBytes, 0x1000);
        g_hd_in_mem = dk::MemBlockMaker{dev, align_up(in_slot * kSlots, DK_MEMBLOCK_ALIGNMENT)}
            .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
            .create();
        for (int i = 0; i < kSlots; ++i) g_hd_in_off[i] = i * in_slot;
        const uint32_t out_slot = align_up(kHiMaxW * kHiMaxH * 4, 0x10000);
        g_hd_out_mem = dk::MemBlockMaker{dev, align_up(out_slot * kSlots, DK_MEMBLOCK_ALIGNMENT)}
            .setFlags(DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image)
            .create();
        for (int i = 0; i < kSlots; ++i) {
            g_hd_out_off[i] = i * out_slot;
            const auto& dims = kHdDims;
            for (int v = 0; v < 4; ++v) {
                dk::ImageLayout layout;
                dk::ImageLayoutMaker{dev}
                    .setFlags(DkImageFlags_PitchLinear | DkImageFlags_Usage2DEngine)
                    .setFormat(DkImageFormat_BGRA8_Unorm)
                    .setDimensions(dims[v][0], dims[v][1])
                    .setPitchStride(dims[v][0] * 4)
                    .initialize(layout);
                g_hd_out_img[i][v].initialize(layout, g_hd_out_mem, g_hd_out_off[i]);
            }
        }
    }

    // native16 HUD panels
    g_panel_ok = NxDeko::LoadShader("PanelBlend", g_panel);
    if (g_panel_ok) {
        dk::ImageLayout layout;
        dk::ImageLayoutMaker{dev}
            .setFlags(DkImageFlags_PitchLinear | DkImageFlags_Usage2DEngine)
            .setFormat(DkImageFormat_RGBA8_Unorm)
            .setDimensions(kFbW, kFbH)
            .setPitchStride(kFbW * 4)
            .initialize(layout);
        g_comp_mem = dk::MemBlockMaker{dev, align_up((uint32_t)layout.getSize(), DK_MEMBLOCK_ALIGNMENT)}
            .setFlags(DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image)
            .create();
        g_comp.initialize(layout, g_comp_mem, 0);
    }

    g_swapchain = dk::SwapchainMaker{dev, nwindowGetDefault(), fbs}.create();
    if (!g_swapchain) { std::fprintf(stderr, "[present] swapchain failed\n"); return false; }
    g_swapchain.setSwapInterval(1);
    g_active = true;
    g_thread = std::thread(thread_main);
    g_thread.detach();
    std::fprintf(stderr, "[present] deko3d presenter ready (%dx%d, %d images)\n", kFbW, kFbH, kSlots);
    return true;
}

namespace {
void present_now(const NxPresentFrame& f)
{
    uint64_t t0 = armGetSystemTick();
    {   // native8g cadence
        static uint32_t last_serial = 0;
        static unsigned run = 0;
        g_stat_presents++;
        if (f.serial3d) {
            if (f.serial3d == last_serial) ++run;
            else {
                if (last_serial && run) g_stat_run[run < 4 ? run : 4]++;
                if (last_serial && f.serial3d < last_serial) g_stat_back++;
                last_serial = f.serial3d;
                run = 1;
            }
        }
    }
    const int slot = g_queue.acquireImage(g_swapchain);
    if (g_fence_used[slot]) g_fence[slot].wait();
    uint64_t t1 = armGetSystemTick();

    uint32_t* top = g_src_cpu[slot][0];
    uint32_t* bottom = g_src_cpu[slot][1];
    uint32_t* hud = g_src_cpu[slot][2];
    copy_screen(top, f.top, f.dim_top);
    const bool draw_bottom = f.bottom_rect.w > 0;
    const bool panels = g_panel_ok && f.npanels > 0 && f.bottom;
    if (draw_bottom || panels) {
        copy_screen(bottom, f.bottom, false);
        if (draw_bottom && f.cursor) draw_cursor(bottom, f.cursor_x, f.cursor_y);
        if (draw_bottom && f.notice && *f.notice) draw_notice(bottom, f.notice, f.glyph);
    }
    int hud_chars = 0;
    if (f.hud_text && *f.hud_text) {
        for (int i = 0; i < kHudW * kHudH; ++i) hud[i] = 0xFF000000u;
        hud_chars = draw_text(hud, kHudW, kHudH, 2, 2, f.hud_text, f.glyph);
        if (hud_chars > (kHudW - 4) / 6) hud_chars = (kHudW - 4) / 6;
    }
    uint64_t t2 = armGetSystemTick();

    g_cmd.clear();
    g_cmd.addMemory(g_cmd_mem, slot * kCmdSlotSize, kCmdSlotSize);
    dk::ImageView fb_view{panels ? g_comp : g_fb[slot]};
    dk::ImageView black_view{g_black};
    g_cmd.blitImage(black_view, {0, 0, 0, 16, 16, 1}, fb_view, {0, 0, 0, kFbW, kFbH, 1},
                    DkBlitFlag_FilterNearest);
    dk::ImageView top_view{g_src[slot][0]};
    dk::ImageView bottom_view{g_src[slot][1]};
    if (draw_bottom && !f.bottom_over_top)
        g_cmd.blitImage(bottom_view, {0, 0, 0, kDsW, kDsH, 1}, fb_view, rect(f.bottom_rect),
                        DkBlitFlag_FilterNearest);
    int hd_view_idx = -1;
    for (int v = 0; v < 4; ++v)
        if ((uint32_t)f.hi_w == kHdDims[v][0] && (uint32_t)f.hi_h == kHdDims[v][1]) hd_view_idx = v;
    const bool hd = f.hd && g_compose_ok && f.hd_top && f.hd_below && f.hi_addr && hd_view_idx >= 0;
    if (hd) g_stat_hd[hd_view_idx]++;
    else if (f.hd) g_stat_refused++;
    else g_stat_plain++;
    if (hd) {
        uint8_t* in = static_cast<uint8_t*>(g_hd_in_mem.getCpuAddr()) + g_hd_in_off[slot];
        std::memcpy(in, f.hd_top, kHdLayerBytes);
        std::memcpy(in + kHdLayerBytes, f.hd_below, kHdLayerBytes);
        const DkGpuAddr in_gpu = g_hd_in_mem.getGpuAddr() + g_hd_in_off[slot];
        const DkGpuAddr par_gpu = in_gpu + 2 * kHdLayerBytes;
        const DkGpuAddr out_gpu = g_hd_out_mem.getGpuAddr() + g_hd_out_off[slot];
        HdParams p{};
        p.out_w = (uint32_t)f.hi_w; p.out_h = (uint32_t)f.hi_h;
        p.bldcnt = f.bldcnt; p.eva = f.eva; p.evb = f.evb; p.evy = f.evy;
        p.master_bright = f.master_bright; p.prio3d = f.priority_3d; p.order3d = f.order_3d;
        p.render_xpos = f.render_xpos; p.output_width = 256;
        p.step_x = (uint32_t)((256u * 65536u + (uint32_t)f.hi_w - 1) / (uint32_t)f.hi_w);
        p.step_y = (uint32_t)((192u * 65536u + (uint32_t)f.hi_h - 1) / (uint32_t)f.hi_h);
        g_cmd.bindShaders(DkStageFlag_Compute, {&g_compose});
        g_cmd.bindUniformBuffer(DkStage_Compute, 0, par_gpu, kHdParamsBytes);
        g_cmd.pushConstants(par_gpu, kHdParamsBytes, 0, sizeof(p), &p);
        g_cmd.bindStorageBuffers(DkStage_Compute, 0, {
            {in_gpu, kHdLayerBytes},
            {in_gpu + kHdLayerBytes, kHdLayerBytes},
            {(DkGpuAddr)f.hi_addr, (uint32_t)(f.hi_w * f.hi_h * 4)},
            {out_gpu, (uint32_t)(f.hi_w * f.hi_h * 4)},
        });
        g_cmd.dispatchCompute((uint32_t)(f.hi_w + 7) / 8, (uint32_t)(f.hi_h + 7) / 8, 1);
        g_cmd.barrier(DkBarrier_Full, DkInvalidateFlags_Image | DkInvalidateFlags_L2Cache);
        dk::ImageView hd_view{g_hd_out_img[slot][hd_view_idx]};
        g_cmd.blitImage(hd_view, {0, 0, 0, (uint32_t)f.hi_w, (uint32_t)f.hi_h, 1}, fb_view,
                        rect(f.top_rect), DkBlitFlag_FilterLinear);

    } else {
        g_cmd.blitImage(top_view, {0, 0, 0, kDsW, kDsH, 1}, fb_view, rect(f.top_rect),
                        DkBlitFlag_FilterNearest);
    }
    if (draw_bottom && f.bottom_over_top)
        g_cmd.blitImage(bottom_view, {0, 0, 0, kDsW, kDsH, 1}, fb_view, rect(f.bottom_rect),
                        DkBlitFlag_FilterNearest);
    if (hud_chars > 0) {
        const uint32_t hw = (uint32_t)(hud_chars * 6 + 3);
        dk::ImageView hud_view{g_src[slot][2]};
        g_cmd.blitImage(hud_view, {0, 0, 0, hw, 11, 1}, fb_view, {6, 6, 0, hw * 3, 33, 1},
                        DkBlitFlag_FilterNearest);
    }
    if (panels) {
        g_cmd.barrier(DkBarrier_Full, DkInvalidateFlags_L2Cache);
        g_cmd.bindShaders(DkStageFlag_Compute, {&g_panel});
        const DkGpuAddr par_base = g_hd_in_mem.getGpuAddr() + g_hd_in_off[slot] + 2 * kHdLayerBytes + kHdParamsBytes;
        const DkGpuAddr src_gpu = g_src_mem.getGpuAddr() + g_src_off[slot][1];
        for (int k = 0; k < f.npanels && k < 3; ++k) {
            const NxPanel& pn = f.panels[k];
            NxRect d = pn.dst;
            if (d.x < 0) { d.w += d.x; d.x = 0; }
            if (d.y < 0) { d.h += d.y; d.y = 0; }
            if (d.x + d.w > kFbW) d.w = kFbW - d.x;
            if (d.y + d.h > kFbH) d.h = kFbH - d.y;
            if (d.w <= 0 || d.h <= 0 || pn.sw <= 0 || pn.sh <= 0) continue;
            PanelParams p{(uint32_t)d.x, (uint32_t)d.y, (uint32_t)d.w, (uint32_t)d.h,
                          (uint32_t)pn.sx, (uint32_t)pn.sy, (uint32_t)pn.sw, (uint32_t)pn.sh,
                          (uint32_t)pn.alpha_dark, (uint32_t)pn.alpha_bright, (uint32_t)kFbW};
            const DkGpuAddr par = par_base + 256u * k;
            g_cmd.bindUniformBuffer(DkStage_Compute, 0, par, 256);
            g_cmd.pushConstants(par, 256, 0, sizeof(p), &p);
            g_cmd.bindStorageBuffers(DkStage_Compute, 0, {
                {src_gpu, (uint32_t)(kDsW * kDsH * 4)},
                {g_comp_mem.getGpuAddr(), (uint32_t)(kFbW * kFbH * 4)},
            });
            g_cmd.dispatchCompute((uint32_t)(d.w + 7) / 8, (uint32_t)(d.h + 7) / 8, 1);
            g_cmd.barrier(DkBarrier_Full, DkInvalidateFlags_L2Cache);
        }
        g_cmd.barrier(DkBarrier_Full, DkInvalidateFlags_Image | DkInvalidateFlags_L2Cache);
        dk::ImageView comp_view{g_comp};
        dk::ImageView real_fb{g_fb[slot]};
        g_cmd.blitImage(comp_view, {0, 0, 0, kFbW, kFbH, 1}, real_fb, {0, 0, 0, kFbW, kFbH, 1},
                        DkBlitFlag_FilterNearest);
    }
    g_cmd.signalFence(g_fence[slot]);
    g_fence_used[slot] = true;
    g_queue.submitCommands(g_cmd.finishList());
    uint64_t t3 = armGetSystemTick();
    g_queue.presentImage(g_swapchain, slot);
    (void)t0; (void)t1; (void)t2; (void)t3;
}
}

void nx_present_frame(const NxPresentFrame& in, NxPresentTicks* ticks)
{
    if (!g_active) return;
    const uint64_t t0 = armGetSystemTick();
    int w;
    {
        std::unique_lock<std::mutex> lock(g_job_m);
        // native8g: display-locked pacing. Hand over a frame only once the
        // presenter has taken the previous one, which it does once per
        // display refresh, so frames reach the screen one per refresh
        // instead of in bursts set by the audio chunks.
        if (g_nx_display_lock && g_job_ready >= 0) {
            const uint64_t w0 = armGetSystemTick();
            g_taken_cv.wait_for(lock, std::chrono::milliseconds(50), [] { return g_job_ready < 0; });
            g_stat_wait_ticks += armGetSystemTick() - w0;
        }
        if (g_job_ready >= 0) g_stat_dropped++;
        // a slot that is neither being presented nor... the other one
        w = (g_job_busy == 0) ? 1 : (g_job_busy == 1) ? 0 : (g_job_ready == 0 ? 1 : 0);
        if (g_job_ready == w) g_job_ready = -1;   // overwrite an unshown frame
    }
    Job& j = g_job[w];
    j.f = in;
    auto take = [](std::vector<uint32_t>& v, const uint32_t* src, size_t n) -> const uint32_t* {
        if (!src) return nullptr;
        v.assign(src, src + n);
        return v.data();
    };
    j.f.top = take(j.top, in.top, kDsW * kDsH);
    j.f.bottom = take(j.bottom, in.bottom, kDsW * kDsH);
    if (in.hd) {
        j.f.hd_top = take(j.hd_top, in.hd_top, kDsW * kDsH * 2);
        j.f.hd_below = take(j.hd_below, in.hd_below, kDsW * kDsH * 2);
    }
    j.hud = in.hud_text ? in.hud_text : "";
    j.notice = in.notice ? in.notice : "";
    j.f.hud_text = in.hud_text ? j.hud.c_str() : nullptr;
    j.f.notice = in.notice ? j.notice.c_str() : nullptr;
    {
        std::lock_guard<std::mutex> lock(g_job_m);
        g_job_ready = w;
    }
    g_job_cv.notify_one();
    if (ticks) {
        // System ticks (19.2 MHz); the caller converts. Only the copy is on
        // the emulation thread now.
        ticks->upload = armGetSystemTick() - t0;
        ticks->draw = 0;
        ticks->swap = 0;
    }
}

void nx_present_stats(char* buf, int n)
{
    static const char* const kNames[4] = {"1.5x", "2x", "3x", "4x"};
    int o = std::snprintf(buf, n, "composed");
    for (int v = 0; v < 4 && o < n; ++v)
        o += std::snprintf(buf + o, n - o, " %s %u", kNames[v], g_stat_hd[v].exchange(0));
    if (o < n)
        o += std::snprintf(buf + o, n - o, "  refused %u  plain %u", g_stat_refused.exchange(0), g_stat_plain.exchange(0));
    if (o < n)
        std::snprintf(buf + o, n - o, " | cadence: presents %u dropped %u  3D frame held 1:%u 2:%u 3:%u 4+:%u  backwards %u  emu waited %.0f ms",
                      g_stat_presents.exchange(0), g_stat_dropped.exchange(0),
                      g_stat_run[1].exchange(0), g_stat_run[2].exchange(0), g_stat_run[3].exchange(0), g_stat_run[4].exchange(0),
                      g_stat_back.exchange(0), armTicksToNs(g_stat_wait_ticks.exchange(0)) / 1e6);
}
