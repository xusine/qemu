// This file is added by Cyan for the quantum mechanism.

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "gdbstub/helpers.h"
#include "exec/helper-proto.h"
#include "cpu.h"
#include "hw/core/cpu.h"
#include "sysemu/quantum.h"

void HELPER(deduce_quantum)(CPUArchState *env) {
    assert(quantum_enabled());

    assert(current_cpu->env_ptr == env);

    // deduction.
    env->quantum_budget -= env->quantum_required;
}

uint32_t HELPER(check_and_deduce_quantum)(CPUArchState *env) {
    assert(quantum_enabled());
    assert(current_cpu->env_ptr == env);

    if (current_cpu->ipc == 0) {
        return false;
    }

    current_cpu->target_cycle_on_instruction += env->quantum_required;

    // deduction.
    env->quantum_budget -= env->quantum_required;
    
    if (env->quantum_budget <= 0) {
        env->quantum_budget_depleted = 1;
        return true;
    }
    return false;
}

void HELPER(deplete_quantum_budget)(CPUArchState *env) {
    assert(quantum_enabled());
    env->quantum_budget = 0;
    env->quantum_budget_depleted = 1;
}

void HELPER(set_quantum_requirement_example)(CPUArchState *env, uint32_t requirement) {
    assert(quantum_enabled());
    env->quantum_required = requirement;
}