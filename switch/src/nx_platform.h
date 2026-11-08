// MPH-nx: Horizon platform helpers shared by the Switch entry point.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// CPU clock boost while the game runs. Restored on exit and on crash.
void nx_clocks_apply(void);
void nx_clocks_restore(void);
void nx_clocks_set_boost(int on);
const char* nx_clocks_status(void);

// Direct Horizon input read for the frontend (touchscreen plus the buttons the
// MPH-nx touch cursor uses). Declared again in the frontend patch; keep the
// two in step.
typedef struct MphnxInput {
    int touch_count;     // fingers on the screen (0 when none)
    int touch_x;         // first finger, 0..1279
    int touch_y;         // first finger, 0..719
    int zl;              // ZL held
    int a;               // A held
    int minus;           // minus held
    float lx, ly;        // left stick, -1..1, +y is down
} MphnxInput;
void mphnx_input_poll(MphnxInput* out);

#ifdef __cplusplus
}
#endif
