#include "qemu/osdep.h"
#include "qemu/units.h"
#include "gdbstub/helpers.h"
#include "exec/helper-proto.h"
#include "cpu.h"

#include <x86intrin.h>
#include "qemu/histogram.h"


void HELPER(note_current_time)(CPUArchState *env) {
    assert(false);

#if defined(__x86_64__)
    // env->current_instruction_rdtsc = __rdtsc();
#endif
    
}

void HELPER(calculate_time_difference)(CPUArchState *env) {
#if defined(__x86_64__)
    // uint64_t time_difference = __rdtsc() - env->current_instruction_rdtsc;

    // Please note that the unit of the time_difference is the cycle. 
    // On these platform, the speed can vary from 2MIPS to 400MIPS,
    // Which means the range should be 2.5ns to 500ns.
    // We make 50 bins, each bin is 10ns.
    // add_data_point(env->instruction_histogram, time_difference);
#endif
}