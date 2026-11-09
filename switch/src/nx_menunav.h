// MPH-nx: controller navigation for the game's touch-only screens.
//
// The port recognises a bottom-screen layout from coarse colour fingerprints
// (nx_menunav_data.h, made from Mark's L3 captures) and then:
//  - port screens and pop-ups: draws its own highlight; stick/D-pad move it,
//    Switch A taps the highlighted button, Switch B taps the screen's back
//    or "No" button;
//  - title-style screens the game already highlights: Switch A sends DS A;
//  - the star map: Switch A taps "Land Ship".
// Touch keeps working everywhere.
#pragma once
#include <cstdint>

struct NxNavPad {
    bool a, b;                      // physical Switch A / B held
    bool up, down, left, right;     // D-pad held
    float lx, ly;                   // left stick, -1..1, +y down
};

struct NxNavOut {
    uint16_t ds_keys;               // DS keys to hold (bit 0 = A, bit 1 = B)
    bool touch_override;            // apply touch below this frame
    bool touch_down;
    uint16_t touch_x, touch_y;
    int weapon_step;                // native16: D-pad left/right in play: -1 / +1 weapon
};

extern bool g_nx_menunav;           // "Menu buttons" toggle (default on)

// Detection, from the bottom screen about to be shown (ARGB8888).
void nx_menunav_detect(const uint32_t* bottom, int stride);
// Which controller input the current screen takes from the game.
bool nx_menunav_owns_pad();         // A, B, D-pad and left stick
bool nx_menunav_owns_a();           // Switch A only
// native16: what the bottom screen shows, for the Top only + HUD layout.
enum NxNavMode { kNxModeUnknown = 0, kNxModePlay = 1, kNxModeScan = 2, kNxModeMenu = 3 };
int nx_menunav_mode();
// In play: the third weapon slot holds a weapon (pixels of the slot).
bool nx_menunav_special_filled();
// native16: D-pad up held in play opens the game's weapon wheel; the left
// stick picks a box and letting go of up selects it. While open the stick
// belongs to the wheel and the full bottom screen should be shown.
bool nx_menunav_weapon_arc();
// D-pad left/right/up belong to the weapon controls (play screen or wheel).
bool nx_menunav_owns_weapon_dpad();
// Once per frame, with the raw pad state.
NxNavOut nx_menunav_input(const NxNavPad& pad);
// Draw the highlight into a copy of the bottom screen. Returns false when
// there is nothing to draw (the caller can keep the original buffer).
bool nx_menunav_draw(const uint32_t* bottom, uint32_t* out, int stride);
