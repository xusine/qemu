// This file is added by Cyan for the quantum mechanism.

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "gdbstub/helpers.h"
#include "exec/helper-proto.h"
#include "cpu.h"
#include "hw/core/cpu.h"
#include "sysemu/quantum.h"

void HELPER(deduce_quantum)(CPUArchState *env) {
    assert(coarse_grained_quantum_enabled());
    env->quantum_budget -= env->quantum_required;
    env->quantum_required = 0;
}

uint32_t HELPER(check_and_deduce_quantum)(CPUArchState *env) {
    assert(coarse_grained_quantum_enabled());

    if (!is_vcpu_affiliated_with_quantum(current_cpu->cpu_index)) {
        return false;
    }

    env->quantum_budget -= env->quantum_required;
    env->quantum_required = 0;
    if (env->quantum_budget <= 0) {
        env->quantum_budget_depleted = 1;
        return true;
    }
    return false;
}

void HELPER(deplete_quantum_budget)(CPUArchState *env) {
    assert(coarse_grained_quantum_enabled() || single_instruction_quantum_enabled());
    env->quantum_budget = 0;
    env->quantum_budget_depleted = 1;
}

void HELPER(set_quantum_requirement_example)(CPUArchState *env, uint32_t requirement) {
    assert(coarse_grained_quantum_enabled());
    env->quantum_required = requirement;
}