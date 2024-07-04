#include "qemu/osdep.h"
#include "sysemu/quantum.h"
#include "qemu/option.h"

uint64_t quantum_size = 0;
static uint64_t quantum_enabled_lower_bound = 0;
static uint64_t quantum_enabled_upper_bound = 0;
uint64_t deplete_threshold = 0;

void quantum_configure(QemuOpts *opts, Error **errp) {
    uint64_t quantum_size_tmp = qemu_opt_get_number(opts, "size", 0);
    deplete_threshold = qemu_opt_get_number(opts, "deplete_threshold", 0xffffffffffffffff);
    const char *range = qemu_opt_get(opts, "range");
 
    if (!range) {
        quantum_enabled_lower_bound = 0;
        quantum_enabled_upper_bound = 0xFFFFFFFFFFFFFFFF; // all cores are enabled.
    } else {
        // need to split the range.
        char *range_tmp = g_strdup(range);
        char *range_start = strtok(range_tmp, "-");
        char *range_end = strtok(NULL, "-");
        quantum_enabled_lower_bound = strtoull(range_start, NULL, 0);
        quantum_enabled_upper_bound = strtoull(range_end, NULL, 0);
        g_free(range_tmp);
    }


    // make it as a global value.
    quantum_size = quantum_size_tmp;
    assert(quantum_size < 0x7fffffff);
}

inline bool is_vcpu_affiliated_with_quantum(uint64_t cpu_idx) {
    if (quantum_size == 0) {
        return false;
    }

    if (quantum_enabled_lower_bound <= cpu_idx && cpu_idx <= quantum_enabled_upper_bound) {
        return true;
    }
    return false;
}