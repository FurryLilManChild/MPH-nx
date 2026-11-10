// MPH-nx native18: lock-on targeting. See nx_lockon.h.
#include "nx_lockon.h"

#include <cmath>
#include <cstdio>
#include <cstring>

// From the runner (title_patches.cpp).
bool nds_title_patches_mph_mainram(const uint8_t** ptr, uint32_t* len);
bool nds_title_patches_mph_local_player_index(uint8_t* pos);
bool nds_title_patches_mph_adventure();

bool g_nx_lockon = true;

namespace {

// US Rev 1 (MphRead amhp1 minus 0xA0; the port's SelectedWeapon 0x020DB463 is
// player + 0x4CF and SpecialWeaponSlot 0x020DB0EE is player + 0x15A, exactly
// MphRead's CPlayer offsets).
constexpr uint32_t kPlayers = 0x020DAF94;
constexpr uint32_t kPlayerStride = 0xF30;
constexpr uint32_t kListHeads[2] = {0x020E3E40, 0x020E3EE0};   // shifted, unshifted
constexpr uint32_t kCamInfo = 0x55C;      // CPlayer.CameraInfo
constexpr uint32_t kGunVec = 0x9C;        // aim direction the inputs turn at once (native22 log: it leads,
                                          // +0xB4 follows halfway, the camera last; +0x64 points up)
constexpr uint32_t kCamPos = 0x0, kCamFacing = 0x3C;
// CEntity / CEnemyBase
constexpr uint32_t kEntType = 0x0, kEntNext = 0x14;
constexpr uint32_t kEnemyPos = 0x4C, kEnemyHealth = 0x110;
constexpr uint16_t kTypeListHead = 27, kTypeEnemy = 23;

constexpr float kRange = 48.0f;           // game units (fx32 / 4096)
constexpr float kConeCos = 0.70f;         // about 45 degrees off the view
constexpr int kMaxWalk = 600;
constexpr float kGain = 100.0f;           // aim counts per radian (about one count per 0.6 degree)
constexpr float kMaxStep = 6.0f;          // counts per frame: caps the turn speed
constexpr float kLead = 8.0f;             // frames of view momentum to allow for

const uint8_t* g_ram = nullptr;
uint32_t g_len = 0;

bool ok_addr(uint32_t a, uint32_t n) {
    return a >= 0x02000000u && a - 0x02000000u + n <= g_len;
}
uint16_t rd16(uint32_t a) { uint16_t v; std::memcpy(&v, g_ram + (a - 0x02000000u), 2); return v; }
uint32_t rd32(uint32_t a) { uint32_t v; std::memcpy(&v, g_ram + (a - 0x02000000u), 4); return v; }
void rdvec(uint32_t a, float v[3]) {
    for (int i = 0; i < 3; ++i) v[i] = (float)(int32_t)rd32(a + 4u * i) / 4096.0f;
}

struct State {
    bool was_held = false;
    uint32_t head = 0;          // verified list head, 0 unknown
    int head_logged = 0;
    uint32_t target = 0;        // locked entity address
    bool spent = false;         // target lost while held: wait for a new press
    // adaptive aim gain (counts per radian), per axis, with sign
    float sign_acc[2] = {0.0f, 0.0f};
    float last_cmd[2] = {0.0f, 0.0f};
    float last_ang[2] = {0.0f, 0.0f};
    bool have_last = false;
    float rem[2] = {0.0f, 0.0f};
    // stats
    unsigned locks = 0, lost = 0, frames = 0, enemies_seen = 0;
    float last_err = 0.0f;
    int trace = 0;               // frames of per-lock trace left
    float omega[2] = {0.0f, 0.0f};   // view turn rate, radians per frame
    bool using_gun = false;
    bool lock_gun = false;
    float cam_yaw = 0.0f;
    uint32_t cand[8] = {};
    int ncand = 0;
    uint32_t cand_base = 0;
} g;

bool find_head() {
    for (uint32_t h : kListHeads)
        if (ok_addr(h, 0x18) && rd16(h + kEntType) == kTypeListHead) {
            if (g.head != h) {
                g.head = h;
                std::fprintf(stderr, "[mph-nx] lockon: entity list at %08X\n", (unsigned)h);
            }
            return true;
        }
    if (g.head_logged < 3) {
        ++g.head_logged;
        std::fprintf(stderr, "[mph-nx] lockon: no entity list head (types %u %u)\n",
                     ok_addr(kListHeads[0], 2) ? rd16(kListHeads[0]) : 0xFFFFu,
                     ok_addr(kListHeads[1], 2) ? rd16(kListHeads[1]) : 0xFFFFu);
    }
    g.head = 0;
    return false;
}

bool enemy_alive(uint32_t e, float pos[3]) {
    if (!ok_addr(e, 0x114) || rd16(e + kEntType) != kTypeEnemy) return false;
    if (rd16(e + kEnemyHealth) == 0) return false;
    rdvec(e + kEnemyPos, pos);
    return true;
}

// The enemy closest to the centre of the view, in range and in the cone.
uint32_t pick(const float cam[3], const float face[3]) {
    uint32_t best = 0;
    float best_score = 1e9f;
    unsigned seen = 0;
    uint32_t e = rd32(g.head + kEntNext);
    for (int i = 0; i < kMaxWalk && e != g.head && ok_addr(e, 0x18); ++i) {
        float p[3];
        if (enemy_alive(e, p)) {
            ++seen;
            const float v[3] = {p[0] - cam[0], p[1] - cam[1], p[2] - cam[2]};
            const float d = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            if (d > 0.5f && d < kRange) {
                const float c = (v[0] * face[0] + v[1] * face[1] + v[2] * face[2]) / d;
                if (c > kConeCos) {
                    const float score = (1.0f - c) * 4.0f + d / kRange;
                    if (score < best_score) { best_score = score; best = e; }
                }
            }
        }
        e = rd32(e + kEntNext);
    }
    g.enemies_seen = seen;
    return best;
}

float wrap(float a) {
    while (a > 3.14159265f) a -= 6.28318531f;
    while (a < -3.14159265f) a += 6.28318531f;
    return a;
}

}  // namespace

NxLockOut nx_lockon_frame(bool held) {
    NxLockOut out{};
    const bool press = held && !g.was_held;
    g.was_held = held;
    if (!held || !g_nx_lockon) {
        if (g.target) { g.target = 0; }
        g.spent = false;
        g.have_last = false;
        return out;
    }
    if (!nds_title_patches_mph_adventure()) return out;
    if (!nds_title_patches_mph_mainram(&g_ram, &g_len) || !g_ram) return out;
    uint8_t idx = 0;
    if (!nds_title_patches_mph_local_player_index(&idx)) return out;
    if (press || !g.head) find_head();
    if (!g.head) return out;

    const uint32_t cam_base = kPlayers + idx * kPlayerStride + kCamInfo;
    if (!ok_addr(cam_base, 0x48)) return out;
    float cam[3], face[3];
    rdvec(cam_base + kCamPos, cam);
    rdvec(cam_base + kCamFacing, face);
    // native21: steer the gun's aim vector (CPlayer + 0x64, MphRead "Vec1",
    // the player's _gunVec1), which an aim input turns at once. The camera
    // follows it smoothly a few frames later, which is the momentum native19
    // and native20 fought. Fall back to the camera if it does not look like
    // a direction.
    float gun[3];
    rdvec(kPlayers + idx * kPlayerStride + kGunVec, gun);
    const float gl = std::sqrt(gun[0] * gun[0] + gun[1] * gun[1] + gun[2] * gun[2]);
    // The camera picks the target (it is what you see); the gun steers only
    // when it points the same way as the camera (native21 also picked with
    // the gun and found nothing).
    const float cl = std::sqrt(face[0] * face[0] + face[1] * face[1] + face[2] * face[2]);
    if (cl < 0.5f) return out;
    for (float& f : face) f /= cl;
    g.cam_yaw = std::atan2(face[0], face[2]);
    const float gdot = gl > 0.5f ? (gun[0] * face[0] + gun[1] * face[1] + gun[2] * face[2]) / gl : -2.0f;
    // Decided at the press (they agree then); while locked the aim may run
    // ahead of the camera, which is fine.
    if (press) g.lock_gun = gl > 0.9f && gl < 1.1f && gdot > 0.8f;
    const bool use_gun = g.lock_gun && gl > 0.9f && gl < 1.1f;
    if (press)
        std::fprintf(stderr, "[mph-nx] lockon: camera %.2f %.2f %.2f  gun %.2f %.2f %.2f (length %.2f, agreement %.2f) -> steer %s\n",
                     face[0], face[1], face[2], gun[0], gun[1], gun[2], gl, gdot, use_gun ? "gun" : "camera");
    if (press) {
        // native22 diagnostic: every direction-shaped value in the player's
        // data that points where the camera looks. The trace then shows how
        // each one moves, to find the gun aim (it turns before the camera).
        g.ncand = 0;
        const uint32_t pb = kPlayers + idx * kPlayerStride;
        for (uint32_t off = 0; off + 12 <= kPlayerStride && g.ncand < 8; off += 4) {
            if (off >= kCamInfo && off < kCamInfo + 0x48) continue;   // the camera itself
            float v[3];
            rdvec(pb + off, v);
            const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            if (l < 0.95f || l > 1.05f) continue;
            const float d = (v[0] * face[0] + v[1] * face[1] + v[2] * face[2]) / l;
            if (d < 0.9f) continue;
            g.cand[g.ncand++] = off;
            std::fprintf(stderr, "[mph-nx] lockon: candidate +%03X  %.3f %.3f %.3f  agreement %.3f\n",
                         (unsigned)off, v[0], v[1], v[2], d);
            off += 8;   // skip the rest of this vector
        }
        g.cand_base = pb;
    }
    float pick_face[3] = {face[0], face[1], face[2]};
    if (use_gun) for (int i = 0; i < 3; ++i) face[i] = gun[i] / gl;
    g.using_gun = use_gun;

    float tp[3];
    if (g.target && !enemy_alive(g.target, tp)) {
        g.target = 0;
        g.spent = true;
        ++g.lost;
        std::fprintf(stderr, "[mph-nx] lockon: target lost\n");
    }
    if (!g.target && press && !g.spent) {
        g.target = pick(cam, pick_face);
        if (g.target) {
            ++g.locks;
            g.have_last = false;
            g.trace = 120;
            std::fprintf(stderr, "[mph-nx] lockon: locked %08X (%u enemies in room)\n",
                         (unsigned)g.target, g.enemies_seen);
        } else {
            g.spent = true;
            std::fprintf(stderr, "[mph-nx] lockon: nothing in front (%u enemies in room)\n", g.enemies_seen);
        }
    }
    if (!g.target || !enemy_alive(g.target, tp)) return out;
    const float v[3] = {tp[0] - cam[0], tp[1] - cam[1], tp[2] - cam[2]};
    const float d = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (d < 0.3f || d > kRange * 1.5f) {
        g.target = 0; g.spent = true; ++g.lost;
        std::fprintf(stderr, "[mph-nx] lockon: target out of range\n");
        return out;
    }
    // Angles of the view and of the target (y up).
    const float ang_face[2] = {std::atan2(face[0], face[2]), std::asin(face[1])};
    const float ang_want[2] = {std::atan2(v[0], v[2]), std::asin(v[1] / d)};
    const float err[2] = {wrap(ang_want[0] - ang_face[0]), ang_want[1] - ang_face[1]};
    g.last_err = std::sqrt(err[0] * err[0] + err[1] * err[1]);
    // native19 safety: a lock that ends up far off target (wrong direction
    // learned, or a camera field that is not the view) lets go instead of
    // spinning the view around.
    if (g.last_err > 1.2f) {
        g.target = 0; g.spent = true; ++g.lost;
        std::fprintf(stderr, "[mph-nx] lockon: released, %.0f degrees off target\n", g.last_err * 57.2958f);
        return out;
    }

    // native19: fixed, gentle speed. Only the direction of each axis is
    // learned (from how the view moved after a command), and it freezes once
    // it is clear, so a lagging camera cannot make the lock push harder and
    // harder (native18 swung around).
    for (int a = 0; a < 2; ++a) {
        const float moved = g.have_last ? wrap(ang_face[a] - g.last_ang[a]) : 0.0f;
        if (g.have_last && std::fabs(g.last_cmd[a]) >= 1.0f && std::fabs(g.sign_acc[a]) < 4.0f) {
            if (std::fabs(moved) > 0.002f)
                g.sign_acc[a] += ((g.last_cmd[a] / moved) > 0 ? 1.0f : -1.0f);
        }
        // native20: the game's aim keeps turning for a while after an input
        // (it has momentum), so steer on where the view will be in kLead
        // frames rather than where it is now; native19 orbited the target.
        g.omega[a] = g.have_last ? g.omega[a] * 0.5f + moved * 0.5f : 0.0f;
        g.last_ang[a] = ang_face[a];
    }
    g.have_last = true;
    float cmd[2];
    for (int a = 0; a < 2; ++a) {
        const float sgn = g.sign_acc[a] < 0.0f ? -1.0f : 1.0f;
        // The gun turns at once: no lead, and a one-degree box around the
        // target where nothing is corrected (Mark's idea), so it holds still.
        const float lead = g.using_gun ? 0.0f : kLead;
        const float ahead = err[a] - g.omega[a] * lead;
        // Hold box (Mark's idea): one degree when steering the gun, three on
        // the camera fallback, where corrections are also gentler.
        const float box = g.using_gun ? 0.017f : 0.052f;
        const float e = std::fabs(ahead) < box ? 0.0f : ahead - (ahead > 0 ? box : -box);
        float c = e * kGain * sgn * (g.using_gun ? 0.35f : 0.2f) + g.rem[a];
        if (c > kMaxStep) c = kMaxStep;
        if (c < -kMaxStep) c = -kMaxStep;
        const float ci = std::trunc(c);
        g.rem[a] = c - ci;
        cmd[a] = ci;
        g.last_cmd[a] = ci;
    }
    if (g.trace > 0 && (g.trace % 3) == 0)
        std::fprintf(stderr, "[mph-nx] lockon trace (%s): err yaw %.1f pitch %.1f deg  turn %.2f %.2f deg/frame  cmd %d %d  view yaw %.1f pitch %.1f camera yaw %.1f  dir %s%s\n",
                     g.using_gun ? "gun" : "camera", err[0] * 57.2958f, err[1] * 57.2958f, g.omega[0] * 57.2958f, g.omega[1] * 57.2958f,
                     (int)cmd[0], (int)cmd[1],
                     ang_face[0] * 57.2958f, ang_face[1] * 57.2958f, g.cam_yaw * 57.2958f,
                     g.sign_acc[0] < 0 ? "-" : "+", g.sign_acc[1] < 0 ? "-" : "+");
    if (g.trace > 0 && (g.trace % 3) == 0 && g.ncand > 0) {
        char line[256];
        int n = std::snprintf(line, sizeof line, "[mph-nx] lockon trace yaw: camera %.1f", g.cam_yaw * 57.2958f);
        for (int i = 0; i < g.ncand && n < 220; ++i) {
            float v[3];
            rdvec(g.cand_base + g.cand[i], v);
            n += std::snprintf(line + n, sizeof line - n, "  +%03X %.1f", (unsigned)g.cand[i],
                               std::atan2(v[0], v[2]) * 57.2958f);
        }
        std::fprintf(stderr, "%s\n", line);
    }
    if (g.trace > 0) --g.trace;
    ++g.frames;
    out.active = true;
    out.dx = (int32_t)cmd[0];
    out.dy = (int32_t)cmd[1];
    return out;
}

void nx_lockon_stats(char* buf, int n) {
    std::snprintf(buf, n, "lockon: locks %u lost %u frames %u  direction x %+.0f y %+.0f  last error %.1f deg  list %08X",
                  g.locks, g.lost, g.frames, g.sign_acc[0], g.sign_acc[1],
                  g.last_err * 57.2958f, (unsigned)g.head);
    g.locks = g.lost = g.frames = 0;
}
