/* MPH-nx coroutine setup: lay out a first frame that nr_coro_switch pops
 * into nr_coro_trampoline, which calls fn(arg). */
#include "nr_coro.h"
#include <stdint.h>
#include <string.h>
void nr_coro_trampoline(void);
void nr_coro_init(NrCoro* c, void* stack, size_t size, void (*fn)(void*), void* arg) {
    uintptr_t top = ((uintptr_t)stack + size) & ~(uintptr_t)15;
#if defined(__aarch64__)
    uint64_t* f = (uint64_t*)(top - 176);
    memset(f, 0, 176);
    f[0] = (uint64_t)(uintptr_t)fn;     /* x19 */
    f[1] = (uint64_t)(uintptr_t)arg;    /* x20 */
    f[11] = (uint64_t)(uintptr_t)nr_coro_trampoline;  /* x30 */
    c->sp = f;
#elif defined(__x86_64__)
    uint64_t* f = (uint64_t*)(top - 8 * 8);
    memset(f, 0, 8 * 8);
    /* pop order: r15 r14 r13 r12 rbx rbp, then ret */
    f[2] = (uint64_t)(uintptr_t)arg;    /* r13 */
    f[3] = (uint64_t)(uintptr_t)fn;     /* r12 */
    f[6] = (uint64_t)(uintptr_t)nr_coro_trampoline;
    c->sp = f;
#endif
}
