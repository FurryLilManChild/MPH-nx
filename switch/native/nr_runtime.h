/* MPH-nx native recompilation runtime interface, included by generated code.
 * Guest registers live in C locals inside each generated function and are
 * synced with g_cpu (the shared ARM state) only at calls, returns and
 * hardware-service points. */
#pragma once
#include <stdint.h>
#include <string.h>
#include "runtime_arm_types.h"

#ifdef __cplusplus
extern "C" {
#endif

extern ArmCpuState g_cpu;
extern unsigned long long g_runtime_cycles;   /* ARM9 time, in ARM9 cycles */
#ifdef NR_DIFFTEST
extern unsigned long long nr_deadline;        /* service when nr_now reaches this */
#else
extern unsigned long long g_nds_fast_limit;   /* the runtime's next poll point */
#define nr_deadline g_nds_fast_limit
#endif
extern uint32_t nr_redirect;                  /* 1: unwind to the top loop at g_cpu.R[15]; 2: no entry there */
extern uint32_t g_nr_q;                       /* sticky Q flag */
extern uintptr_t nr_rd_tab[1u << 20];         /* 4 KiB guest page -> host page pointer, 0 = slow path */
extern uintptr_t nr_wr_tab[1u << 20];

#define nr_now g_runtime_cycles

uint32_t nr_io_rd32(uint32_t a);
uint32_t nr_io_rd16(uint32_t a);
uint32_t nr_io_rd8(uint32_t a);
void nr_io_wr32(uint32_t a, uint32_t v);
void nr_io_wr16(uint32_t a, uint32_t v);
void nr_io_wr8(uint32_t a, uint32_t v);

void nr_service(void);
void nr_call_dyn(uint32_t target);           /* target | thumb bit; LR already set */
void nr_swi(uint32_t num);
void nr_cp15_write(uint32_t opc1, uint32_t crn, uint32_t crm, uint32_t opc2, uint32_t v);
uint32_t nr_cp15_read(uint32_t opc1, uint32_t crn, uint32_t crm, uint32_t opc2);
void nr_msr_cpsr(uint32_t v, uint32_t mask);
uint32_t nr_get_spsr(void);
void nr_set_spsr(uint32_t v, uint32_t mask);
void nr_exception_return(uint32_t pc);
void nr_fallback(void);                      /* run g_cpu.R[15] on the interpreter */

static inline uint32_t nr_ror(uint32_t v, uint32_t n) { n &= 31; return n ? (v >> n) | (v << (32 - n)) : v; }
static inline uint32_t nr_lsl(uint32_t v, uint32_t n) { return n >= 32 ? 0 : v << n; }
static inline uint32_t nr_lsr(uint32_t v, uint32_t n) { return n >= 32 ? 0 : v >> n; }
static inline uint32_t nr_asr(uint32_t v, uint32_t n) { return n >= 32 ? (uint32_t)((int32_t)v >> 31) : (uint32_t)((int32_t)v >> n); }
static inline uint32_t nr_rorr(uint32_t v, uint32_t n) { return nr_ror(v, n & 31); }
static inline uint32_t nr_lsl_c(uint32_t v, uint32_t n, uint32_t* c) {
    if (n == 0) return v; if (n < 32) { *c = (v >> (32 - n)) & 1; return v << n; }
    *c = n == 32 ? (v & 1) : 0; return 0; }
static inline uint32_t nr_lsr_c(uint32_t v, uint32_t n, uint32_t* c) {
    if (n == 0) return v; if (n < 32) { *c = (v >> (n - 1)) & 1; return v >> n; }
    *c = n == 32 ? (v >> 31) : 0; return 0; }
static inline uint32_t nr_asr_c(uint32_t v, uint32_t n, uint32_t* c) {
    if (n == 0) return v; if (n < 32) { *c = (v >> (n - 1)) & 1; return (uint32_t)((int32_t)v >> n); }
    *c = v >> 31; return (uint32_t)((int32_t)v >> 31); }
static inline uint32_t nr_rorr_c(uint32_t v, uint32_t n, uint32_t* c) {
    if (n == 0) return v; n &= 31; if (n == 0) { *c = v >> 31; return v; }
    uint32_t r = nr_ror(v, n); *c = r >> 31; return r; }
static inline uint32_t nr_qadd(uint32_t a, uint32_t b) {
    int64_t s = (int64_t)(int32_t)a + (int32_t)b;
    if (s > 0x7FFFFFFF) { g_nr_q = 1; return 0x7FFFFFFFu; }
    if (s < -0x80000000LL) { g_nr_q = 1; return 0x80000000u; }
    return (uint32_t)s; }
static inline uint32_t nr_qsub(uint32_t a, uint32_t b) {
    int64_t s = (int64_t)(int32_t)a - (int32_t)b;
    if (s > 0x7FFFFFFF) { g_nr_q = 1; return 0x7FFFFFFFu; }
    if (s < -0x80000000LL) { g_nr_q = 1; return 0x80000000u; }
    return (uint32_t)s; }

#define NR_LIKELY(x) __builtin_expect(!!(x), 1)

static inline uint32_t NR_RD32(uint32_t a) {
    uintptr_t b = nr_rd_tab[a >> 12];
    if (NR_LIKELY(b)) { uint32_t v; memcpy(&v, (const void*)(b + (a & 0xFFCu)), 4); return v; }
    return nr_io_rd32(a & ~3u); }
static inline uint32_t NR_LDR32(uint32_t a) {
    uint32_t v = NR_RD32(a & ~3u);
    return (a & 3) ? nr_ror(v, (a & 3) * 8) : v; }
static inline uint32_t NR_RD16(uint32_t a) {
    uintptr_t b = nr_rd_tab[a >> 12];
    if (NR_LIKELY(b)) { uint16_t v; memcpy(&v, (const void*)(b + (a & 0xFFEu)), 2); return v; }
    return nr_io_rd16(a & ~1u); }
static inline uint32_t NR_RD8(uint32_t a) {
    uintptr_t b = nr_rd_tab[a >> 12];
    if (NR_LIKELY(b)) return *(const uint8_t*)(b + (a & 0xFFFu));
    return nr_io_rd8(a); }
static inline void NR_WR32(uint32_t a, uint32_t v) {
    uintptr_t b = nr_wr_tab[a >> 12];
    if (NR_LIKELY(b)) { memcpy((void*)(b + (a & 0xFFCu)), &v, 4); return; }
    nr_io_wr32(a & ~3u, v); }
static inline void NR_WR16(uint32_t a, uint32_t v) {
    uintptr_t b = nr_wr_tab[a >> 12];
    if (NR_LIKELY(b)) { uint16_t h = (uint16_t)v; memcpy((void*)(b + (a & 0xFFEu)), &h, 2); return; }
    nr_io_wr16(a & ~1u, v); }
static inline void NR_WR8(uint32_t a, uint32_t v) {
    uintptr_t b = nr_wr_tab[a >> 12];
    if (NR_LIKELY(b)) { *(uint8_t*)(b + (a & 0xFFFu)) = (uint8_t)v; return; }
    nr_io_wr8(a, v); }

#define NR_LOCALS() \
    uint32_t r0 = g_cpu.R[0], r1 = g_cpu.R[1], r2 = g_cpu.R[2], r3 = g_cpu.R[3], \
             r4 = g_cpu.R[4], r5 = g_cpu.R[5], r6 = g_cpu.R[6], r7 = g_cpu.R[7], \
             r8 = g_cpu.R[8], r9 = g_cpu.R[9], r10 = g_cpu.R[10], r11 = g_cpu.R[11], \
             r12 = g_cpu.R[12], r13 = g_cpu.R[13], r14 = g_cpu.R[14]; \
    uint32_t fn = g_cpu.cpsr >> 31, fz = (g_cpu.cpsr >> 30) & 1, \
             fc = (g_cpu.cpsr >> 29) & 1, fv = (g_cpu.cpsr >> 28) & 1

#define NR_LOAD() do { \
    r0 = g_cpu.R[0]; r1 = g_cpu.R[1]; r2 = g_cpu.R[2]; r3 = g_cpu.R[3]; \
    r4 = g_cpu.R[4]; r5 = g_cpu.R[5]; r6 = g_cpu.R[6]; r7 = g_cpu.R[7]; \
    r8 = g_cpu.R[8]; r9 = g_cpu.R[9]; r10 = g_cpu.R[10]; r11 = g_cpu.R[11]; \
    r12 = g_cpu.R[12]; r13 = g_cpu.R[13]; r14 = g_cpu.R[14]; \
    fn = g_cpu.cpsr >> 31; fz = (g_cpu.cpsr >> 30) & 1; \
    fc = (g_cpu.cpsr >> 29) & 1; fv = (g_cpu.cpsr >> 28) & 1; } while (0)

#define NR_CPSR() ((g_cpu.cpsr & 0x07FFFFDFu) | (fn << 31) | (fz << 30) | (fc << 29) | \
                   (fv << 28) | (g_nr_q << 27) | (_thumb << 5))

#define NR_STORE() do { \
    g_cpu.R[0] = r0; g_cpu.R[1] = r1; g_cpu.R[2] = r2; g_cpu.R[3] = r3; \
    g_cpu.R[4] = r4; g_cpu.R[5] = r5; g_cpu.R[6] = r6; g_cpu.R[7] = r7; \
    g_cpu.R[8] = r8; g_cpu.R[9] = r9; g_cpu.R[10] = r10; g_cpu.R[11] = r11; \
    g_cpu.R[12] = r12; g_cpu.R[13] = r13; g_cpu.R[14] = r14; \
    g_cpu.cpsr = NR_CPSR(); } while (0)

#define NR_RET() ((void)0)

/* Host stack guard: guest recursion runs on the host stack. */
extern char* nr_stack_limit;
void nr_stack_overflow(void);
#define NR_ENTER() do { if (__builtin_expect((char*)__builtin_frame_address(0) < nr_stack_limit, 0)) nr_stack_overflow(); } while (0)

/* Host debug: NR_CALLTRACE builds log every native function entry
   (nr_fentry decides whether to print). */
#ifdef NR_CALLTRACE
void nr_fentry(uint32_t pc);
#define NR_FENTRY(pc) nr_fentry(pc)
#else
#define NR_FENTRY(pc) ((void)0)
#endif
#ifdef NR_TRACE_ON
void nr_trace(uint32_t pc, const uint32_t* r, uint32_t flags);
#define NR_TRACE(pc) do { uint32_t _tr[15] = {r0,r1,r2,r3,r4,r5,r6,r7,r8,r9,r10,r11,r12,r13,r14}; \
    nr_trace(pc, _tr, (fn << 3) | (fz << 2) | (fc << 1) | fv); } while (0)
#else
#define NR_TRACE(pc) ((void)0)
#endif

typedef void (*NrFn)(uint32_t entry);
typedef struct NrEntry { uint32_t key; NrFn fn; } NrEntry;   /* key = addr | thumb */

#ifdef __cplusplus
}
#endif
