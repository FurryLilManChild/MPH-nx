/* MPH-nx native entry tables (generated nr_tables.c fills them). */
#pragma once
#include "nr_runtime.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct NrOverlay {
    uint32_t id, ram, size;
    const NrEntry* tab;
    uint32_t count;
    uint8_t head[64];          /* first 64 bytes of the overlay: identifies it in RAM */
} NrOverlay;
extern const NrEntry nr_tab_static[];
extern const uint32_t nr_tab_static_count;
extern const NrOverlay nr_overlays[];
extern const uint32_t nr_overlay_count;
#ifdef __cplusplus
}
#endif
