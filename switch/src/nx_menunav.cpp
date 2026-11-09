// MPH-nx: controller navigation for the game's touch-only screens.
// See nx_menunav.h.
#include "nx_menunav.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

enum NavKind { kNavPort, kNavPopup, kNavGame, kNavStarmap };

struct NavRect {
    int x0, y0, x1, y1;   // inclusive, DS bottom-screen pixels
    int fp;               // first fingerprint cell
    int gw, gh;           // fingerprint grid
    bool need;            // target must be visible to be used
    const char* name;
};

struct NavScreen {
    const char* name;
    NavKind kind;
    int key0, nkeys;      // fingerprints that must all match
    int t0, nt;           // targets
    int back;             // target index for Switch B, or -1
    int def;              // default target, or -1
};

#include "nx_menunav_data.h"

constexpr int kScreens = sizeof(kNavScreens) / sizeof(kNavScreens[0]);
constexpr int kMaxTargets = 8;
constexpr float kKeyMatch = 0.90f;
constexpr float kTargetMatch = 0.85f;
constexpr int kTol = 24;

float match_rect(const uint32_t* px, int stride, const NavRect& r) {
    const int w = r.x1 - r.x0 + 1, h = r.y1 - r.y0 + 1;
    int ok = 0;
    for (int j = 0; j < r.gh; ++j) {
        for (int i = 0; i < r.gw; ++i) {
            const int cx0 = r.x0 + i * w / r.gw, cx1 = r.x0 + (i + 1) * w / r.gw;
            const int cy0 = r.y0 + j * h / r.gh, cy1 = r.y0 + (j + 1) * h / r.gh;
            int s[3] = {0, 0, 0}, n = 0;
            for (int y = cy0; y < cy1; ++y) {
                const uint32_t* row = px + y * stride;
                for (int x = cx0; x < cx1; ++x) {
                    const uint32_t p = row[x];
                    s[0] += (p >> 16) & 0xFF; s[1] += (p >> 8) & 0xFF; s[2] += p & 0xFF;
                    ++n;
                }
            }
            if (n == 0) continue;
            const uint8_t* f = kNavFp[r.fp + j * r.gw + i];
            if (std::abs(s[0] / n - f[0]) <= kTol && std::abs(s[1] / n - f[1]) <= kTol &&
                std::abs(s[2] / n - f[2]) <= kTol)
                ++ok;
        }
    }
    return static_cast<float>(ok) / static_cast<float>(r.gw * r.gh);
}

struct State {
    int screen = -1;          // active screen
    int candidate = -1, candidate_frames = 0, miss_frames = 0;
    bool visible[kMaxTargets] = {};
    int sel = -1;
    // Touch sequence.
    struct Step { uint16_t x, y; bool down; int frames; };
    Step steps[6] = {};
    int nsteps = 0, step = 0, step_left = 0;
    int cooldown = 0;
    // Edge detection.
    bool prev_a = false, prev_b = false;
    int dir_held = -1, dir_frames = 0;
    // Slider value read at detection time (options screen).
    int slider_top = -1;
} g;

const NavScreen* cur() { return g.screen >= 0 ? &kNavScreens[g.screen] : nullptr; }

bool screen_matches(const uint32_t* px, int stride, int s, bool vis[kMaxTargets]) {
    const NavScreen& sc = kNavScreens[s];
    for (int k = 0; k < sc.nkeys; ++k)
        if (match_rect(px, stride, kNavRects[sc.key0 + k]) < kKeyMatch) return false;
    bool any = sc.nt == 0;
    for (int t = 0; t < sc.nt && t < kMaxTargets; ++t) {
        const NavRect& r = kNavRects[sc.t0 + t];
        vis[t] = !r.need || match_rect(px, stride, r) >= kTargetMatch;
        any = any || vis[t];
    }
    return any;
}

void pick_default() {
    const NavScreen* sc = cur();
    g.sel = -1;
    if (!sc || sc->nt == 0) return;
    if (sc->def >= 0 && g.visible[sc->def]) { g.sel = sc->def; return; }
    for (int t = 0; t < sc->nt; ++t)
        if (g.visible[t]) { g.sel = t; return; }
}

void start_tap(int x, int y) {
    g.steps[0] = {0, 0, false, 2};
    g.steps[1] = {static_cast<uint16_t>(x), static_cast<uint16_t>(y), true, 3};
    g.steps[2] = {0, 0, false, 2};
    g.nsteps = 3; g.step = 0; g.step_left = g.steps[0].frames;
    g.cooldown = 12;
    std::fprintf(stderr, "[mph-nx] menunav: tap %d,%d on %s\n", x, y, cur() ? cur()->name : "?");
}

void start_drag(int x, int y0, int y1) {
    g.steps[0] = {0, 0, false, 2};
    g.steps[1] = {static_cast<uint16_t>(x), static_cast<uint16_t>(y0), true, 3};
    g.steps[2] = {static_cast<uint16_t>(x), static_cast<uint16_t>((y0 + y1) / 2), true, 2};
    g.steps[3] = {static_cast<uint16_t>(x), static_cast<uint16_t>(y1), true, 3};
    g.steps[4] = {0, 0, false, 2};
    g.nsteps = 5; g.step = 0; g.step_left = g.steps[0].frames;
    g.cooldown = 8;
    std::fprintf(stderr, "[mph-nx] menunav: slider %d -> %d\n", y0, y1);
}

void tap_target(int t) {
    const NavScreen* sc = cur();
    if (!sc || t < 0 || t >= sc->nt) return;
    const NavRect& r = kNavRects[sc->t0 + t];
    start_tap((r.x0 + r.x1) / 2, (r.y0 + r.y1) / 2);
}

void move_sel(int dir) {   // 0 up, 1 down, 2 left, 3 right
    const NavScreen* sc = cur();
    if (!sc || g.sel < 0) return;
    const NavRect& a = kNavRects[sc->t0 + g.sel];
    const int ax = (a.x0 + a.x1) / 2, ay = (a.y0 + a.y1) / 2;
    int best = -1, best_score = 1 << 30;
    for (int t = 0; t < sc->nt; ++t) {
        if (t == g.sel || !g.visible[t]) continue;
        const NavRect& b = kNavRects[sc->t0 + t];
        const int dx = (b.x0 + b.x1) / 2 - ax, dy = (b.y0 + b.y1) / 2 - ay;
        int along, across;
        switch (dir) {
            case 0: along = -dy; across = dx; break;
            case 1: along = dy; across = dx; break;
            case 2: along = -dx; across = dy; break;
            default: along = dx; across = dy; break;
        }
        if (along <= 4) continue;
        const int score = along + 2 * std::abs(across);
        if (score < best_score) { best_score = score; best = t; }
    }
    if (best >= 0) g.sel = best;
}

bool on_slider() {
    const NavScreen* sc = cur();
    return sc && g.sel >= 0 && std::strcmp(kNavRects[sc->t0 + g.sel].name, "slider") == 0;
}

}  // namespace

bool g_nx_menunav = true;

void nx_menunav_detect(const uint32_t* bottom, int stride) {
    if (!g_nx_menunav || !bottom) {
        g.screen = -1;
        return;
    }
    bool vis[kMaxTargets] = {};
    int found = -1;
    // The current screen first (keeps it stable), then the list in order:
    // pop-ups come before the screens they cover.
    if (g.screen >= 0 && screen_matches(bottom, stride, g.screen, vis)) found = g.screen;
    for (int s = 0; found < 0 && s < kScreens; ++s) {
        bool v[kMaxTargets] = {};
        if (screen_matches(bottom, stride, s, v)) { found = s; std::memcpy(vis, v, sizeof vis); }
    }
    if (found >= 0 && found == g.screen) {
        g.miss_frames = 0;
        std::memcpy(g.visible, vis, sizeof vis);
        if (g.sel < 0 || !g.visible[g.sel]) pick_default();
    } else if (found >= 0) {
        g.candidate_frames = (found == g.candidate) ? g.candidate_frames + 1 : 1;
        g.candidate = found;
        if (g.candidate_frames >= 2) {
            g.screen = found;
            g.miss_frames = 0;
            std::memcpy(g.visible, vis, sizeof vis);
            pick_default();
            std::fprintf(stderr, "[mph-nx] menunav: screen %s\n", kNavScreens[found].name);
        }
    } else if (g.screen >= 0 && ++g.miss_frames >= 4) {
        std::fprintf(stderr, "[mph-nx] menunav: screen %s left\n", kNavScreens[g.screen].name);
        g.screen = -1;
        g.candidate = -1;
        g.sel = -1;
    }
    // Options: where the sensitivity fill starts (bright cyan at x 131).
    g.slider_top = -1;
    if (const NavScreen* sc = cur(); sc && std::strcmp(sc->name, "options") == 0) {
        for (int y = 59; y <= 123; ++y) {
            const uint32_t p = bottom[y * stride + 131];
            const int r = (p >> 16) & 0xFF, gg = (p >> 8) & 0xFF, b = p & 0xFF;
            if (b > 200 && gg > 150 && r < 100) { g.slider_top = y; break; }
        }
    }
}

bool nx_menunav_owns_pad() {
    const NavScreen* sc = cur();
    return sc && (sc->kind == kNavPort || sc->kind == kNavPopup);
}

bool nx_menunav_owns_a() {
    return cur() != nullptr;
}

NxNavOut nx_menunav_input(const NxNavPad& pad) {
    NxNavOut out{};
    const NavScreen* sc = cur();

    // A running touch sequence finishes even if the screen changes.
    if (g.nsteps > 0) {
        const auto& s = g.steps[g.step];
        out.touch_override = true;
        out.touch_down = s.down;
        out.touch_x = s.x;
        out.touch_y = s.y;
        if (--g.step_left <= 0) {
            if (++g.step >= g.nsteps) g.nsteps = 0;
            else g.step_left = g.steps[g.step].frames;
        }
    }
    if (g.cooldown > 0) --g.cooldown;

    const bool a_edge = pad.a && !g.prev_a, b_edge = pad.b && !g.prev_b;
    g.prev_a = pad.a;
    g.prev_b = pad.b;
    if (!sc) { g.dir_held = -1; return out; }

    if (sc->kind == kNavGame) {
        if (pad.a) out.ds_keys |= 1u;   // DS A while Switch A is held
        return out;
    }
    if (sc->kind == kNavStarmap) {
        if (a_edge && g.nsteps == 0 && g.cooldown == 0) tap_target(0);
        return out;
    }

    // Port highlight: direction with repeat (first move at once, then after
    // 20 frames, then every 7).
    int dir = -1;
    if (pad.up || pad.ly < -0.5f) dir = 0;
    else if (pad.down || pad.ly > 0.5f) dir = 1;
    else if (pad.left || pad.lx < -0.5f) dir = 2;
    else if (pad.right || pad.lx > 0.5f) dir = 3;
    if (dir != g.dir_held) { g.dir_held = dir; g.dir_frames = 0; }
    bool fire = false;
    if (dir >= 0) {
        fire = g.dir_frames == 0 || (g.dir_frames >= 20 && (g.dir_frames - 20) % 7 == 0);
        ++g.dir_frames;
    }
    if (fire && g.nsteps == 0) {
        if (on_slider() && (dir == 2 || dir == 3)) {
            if (g.slider_top >= 0 && g.cooldown == 0) {
                const int ny = g.slider_top + (dir == 3 ? -5 : 5);
                if (ny >= 59 && ny <= 123) start_drag(131, g.slider_top, ny);
            }
        } else {
            move_sel(dir);
        }
    }
    if (a_edge && g.nsteps == 0 && g.cooldown == 0 && g.sel >= 0 && !on_slider())
        tap_target(g.sel);
    if (b_edge && g.nsteps == 0 && g.cooldown == 0 && sc->back >= 0 && g.visible[sc->back])
        tap_target(sc->back);
    return out;
}

bool nx_menunav_draw(const uint32_t* bottom, uint32_t* out, int stride) {
    const NavScreen* sc = cur();
    if (!sc || g.sel < 0 || (sc->kind != kNavPort && sc->kind != kNavPopup)) return false;
    std::memcpy(out, bottom, sizeof(uint32_t) * stride * 192);
    const NavRect& r = kNavRects[sc->t0 + g.sel];
    const int x0 = r.x0 - 2 < 0 ? 0 : r.x0 - 2, y0 = r.y0 - 2 < 0 ? 0 : r.y0 - 2;
    const int x1 = r.x1 + 2 > 255 ? 255 : r.x1 + 2, y1 = r.y1 + 2 > 191 ? 191 : r.y1 + 2;
    const uint32_t c = 0xFFFFE000u;   // bright yellow
    for (int k = 0; k < 2; ++k) {
        for (int x = x0; x <= x1; ++x) {
            if (y0 + k <= y1) out[(y0 + k) * stride + x] = c;
            if (y1 - k >= y0) out[(y1 - k) * stride + x] = c;
        }
        for (int y = y0; y <= y1; ++y) {
            if (x0 + k <= x1) out[y * stride + x0 + k] = c;
            if (x1 - k >= x0) out[y * stride + x1 - k] = c;
        }
    }
    return true;
}
