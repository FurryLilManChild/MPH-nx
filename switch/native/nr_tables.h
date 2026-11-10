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
    uint64_t head_hash;        /* FNV-1a 64 of the overlay's first 64 bytes: identifies it in RAM
                                  (a hash, so the build carries no cartridge bytes) */
} NrOverlay;
extern const NrEntry nr_tab_static[];
extern const uint32_t nr_tab_static_count;
extern const NrOverlay nr_overlays[];
extern const uint32_t nr_overlay_count;
#ifdef __cplusplus
}
#endif
