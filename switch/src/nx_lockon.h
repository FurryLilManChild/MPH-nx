// MPH-nx native18: lock-on targeting (hold ZL), as in Metroid Prime 1 to 3.
//
// The port reads the room's entity list and the local player's camera from
// game memory (layout from NoneGiven's MphRead, MIT; US Rev 1 addresses are
// MphRead's European Rev 1 ones shifted by the same 0xA0 the port's existing
// weapon addresses show) and turns the aim toward the locked enemy through the
// game's own per-frame aim fields, the same path the right stick uses. The
// game's code is not changed.
#pragma once
#include <cstdint>

struct NxLockOut {
    bool active;          // a target is locked; dx/dy are aim counts for this frame
    int32_t dx, dy;
};

extern bool g_nx_lockon;  // "Lock-on" toggle (default on)
extern bool g_nx_lockon_debug;  // per-press and per-frame trace logs (developer.on only)

// Once per frame. held: ZL is held and the game is in play (adventure only).
NxLockOut nx_lockon_frame(bool held);
// For the 10 s log line.
void nx_lockon_stats(char* buf, int n);
