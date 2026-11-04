/* MPH-nx stub: the retail Nintendo DS BIOS bank is not built (no retail dump).
   The Switch build always boots FreeBIOS (--freebios), so this bank is never
   registered. It exists only to satisfy the runner's link. */
#include "runtime_arm.h"
int mphnx_arm9_bios_stub_unused;
