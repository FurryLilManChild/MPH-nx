/* MPH-nx minimal stack-switching coroutine (one per side, same OS thread). */
#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct NrCoro { void* sp; } NrCoro;
/* Save the current context into `from`, resume `to`. */
void nr_coro_switch(NrCoro* from, NrCoro* to);
/* Prepare `c` to start fn(arg) on the given stack at its first resume. */
void nr_coro_init(NrCoro* c, void* stack, size_t size, void (*fn)(void*), void* arg);
#ifdef __cplusplus
}
#endif
