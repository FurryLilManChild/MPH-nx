/* MPH-nx native ARM7: the generated ARM7 code uses the same helpers as the
 * ARM9 code (nr_runtime.h), bound to the ARM7's own page tables, I/O, calls,
 * BIOS calls and service point. Included by generated ARM7 code and by
 * nr_runtime7.cpp only. */
#pragma once
#define nr_rd_tab nr7_rd_tab
#define nr_wr_tab nr7_wr_tab
#define nr_io_rd32 nr7_io_rd32
#define nr_io_rd16 nr7_io_rd16
#define nr_io_rd8 nr7_io_rd8
#define nr_io_wr32 nr7_io_wr32
#define nr_io_wr16 nr7_io_wr16
#define nr_io_wr8 nr7_io_wr8
#define nr_service nr7_service
#define nr_call_dyn nr7_call_dyn
#define nr_swi nr7_swi
#define nr_cp15_write nr7_cp15_write
#define nr_cp15_read nr7_cp15_read
#define nr_msr_cpsr nr7_msr_cpsr
#define nr_exception_return nr7_exception_return
#define nr_fallback nr7_fallback
#define nr_stack_limit nr7_stack_limit
#define nr_stack_overflow nr7_stack_overflow
#define nr_fentry nr7_fentry
#define nr_trace nr7_trace
#include "nr_runtime.h"
