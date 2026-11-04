// MPH-nx: Horizon platform helpers shared by the Switch entry point.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// CPU clock boost while the game runs. Restored on exit and on crash.
void nx_clocks_apply(void);
void nx_clocks_restore(void);
const char* nx_clocks_status(void);

#ifdef __cplusplus
}
#endif
