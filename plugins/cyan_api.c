/**
 * @file cyan_api.c
 *
 * @brief This file defines Cyan's API for QEMU.
 *
 *
 * Some plugins defined in this file can be removed in the future if QEMU
 * officially supports them.
 *
 * Currently, all plugins are only for ARM processor.
 */

#include <stdint.h>
#ifndef CONFIG_USER_ONLY

#include "qemu/osdep.h"
#include "qemu/plugin.h"
#include "qemu/log.h"
#include "qemu/qemu-plugin.h"
#include "sysemu/cpu-timers.h"
#include "sysemu/cpus.h"
#include "tcg/tcg.h"
#include "exec/exec-all.h"
#include "exec/ram_addr.h"
#include "disas/disas.h"
#include "plugin.h"
#include "qemu/plugin-memory.h"
#include "hw/boards.h"
#include "softmmu/timers-state.h"
#include "exec/cpu-common.h"



void register_virtual_clock_cb(int64_t (*callback)(void));

void qemu_plugin_set_running_flag(bool is_running) {
  CPUState *cpu = current_cpu;
  if (is_running) {
    // set the running flag to true, and also toggle the pending_list.
    cpu_exec_start(cpu);
  } else {
    // set the running flag to false, and remove this core self from the pending
    // list.
    cpu_exec_end(cpu);
  }
}

bool qemu_plugin_is_current_cpu_can_run(void) {
  return cpu_can_run(current_cpu);
}

void qemu_plugin_register_virtual_time_cb(int64_t (*callback)(void)) {
  register_virtual_clock_cb(callback);
}

int64_t qemu_plugin_get_cpu_clock(void) { return cpu_get_clock(); }

int64_t qemu_plugin_get_snapshoted_vm_clock(void) {
  return cpu_get_snapshoted_vm_clock();
}

bool qemu_plugin_cpu_is_tick_enabled(void) { return cpu_is_tick_enabled(); }

uint64_t qemu_plugin_read_cpu_integer_register(int reg_index) {
  g_assert_cmpstr(TARGET_NAME, ==, "aarch64");

  g_assert(reg_index >= 0 && reg_index < 32);

  CPUState *cpu = current_cpu;
  g_assert(cpu != NULL);

  return (uint64_t)cpu->env_ptr->xregs[reg_index];
}

uint64_t qemu_plugin_read_ttbr_el1(int which_tbbr) {
  g_assert_cmpstr(TARGET_NAME, ==, "aarch64");

  CPUState *cpu = current_cpu;
  g_assert(cpu != NULL);

  g_assert(which_tbbr == 0 || which_tbbr == 1);

  if (which_tbbr == 0) {
    return (uint64_t)cpu->env_ptr->cp15.ttbr0_el[1];
  } else {
    return (uint64_t)cpu->env_ptr->cp15.ttbr1_el[1];
  }
}

uint64_t qemu_plugin_read_tcr_el1(void) {
  g_assert_cmpstr(TARGET_NAME, ==, "aarch64");

  CPUState *cpu = current_cpu;
  g_assert(cpu != NULL);

  return (uint64_t)cpu->env_ptr->cp15.tcr_el[1];
}

const uint64_t *qemu_plugin_hwaddr_translate_walk_trace(
    const struct qemu_plugin_hwaddr *hwaddr) {
  g_assert_cmpstr(TARGET_NAME, ==, "aarch64");
  if (hwaddr) {
    if (!hwaddr->is_io) {
      return &hwaddr->v.ram.walk_trace[0];
    }
  }
  return NULL;
}

void qemu_plugin_read_physical_memory(uint64_t physical_address, uint64_t size,
                                      void *buf) {
  cpu_physical_memory_rw(physical_address, buf, size, false);
}

void qemu_plugin_write_physical_memory(uint64_t physical_address, uint64_t size,
                                       const void *buf) {
  cpu_physical_memory_rw(physical_address, (void *)buf, size, true);
}

bool qemu_plugin_register_vcpu_branch_resolved_cb(qemu_plugin_vcpu_branch_resolved_cb_t cb) {
  return arm_cpu_register_cyan_branch_cb(cb);
}

// uint8_t qemu_plugin_get_cvnz(void) {
//   g_assert_cmpstr(TARGET_NAME, ==, "aarch64");

//   CPUState *cpu = current_cpu;
//   g_assert(cpu != NULL);

//   uint8_t cvnz = 0;

//   // C
//   cvnz |= (cpu->env_ptr->CF == 1 ? 1 : 0) << 0;

//   // V
//   cvnz |= ((cpu->env_ptr->VF & 0x80000000) != 0 ? 1 : 0) << 1;

//   // N
//   cvnz |= ((cpu->env_ptr->NF & 0x80000000) != 0 ? 1 : 0) << 2;

//   // Z
//   cvnz |= (cpu->env_ptr->ZF == 0 ? 1 : 0) << 3;

//   return cvnz;
// }

// uint64_t helper_autia(CPUARMState *env, uint64_t pointer, uint64_t modifier);
// uint64_t helper_autib(CPUARMState *env, uint64_t pointer, uint64_t modifier);

// uint64_t qemu_plugin_resolve_pointer_authentication(uint64_t pointer, uint64_t key, uint64_t modifier){
//   g_assert_cmpstr(TARGET_NAME, ==, "aarch64");

//   CPUState *cpu = current_cpu;
//   g_assert(cpu != NULL);

//   if (key == 0) {
//     return helper_autia(cpu->env_ptr, pointer, modifier);
//   }

//   return helper_autib(cpu->env_ptr, pointer, modifier);
// }

uint64_t qemu_plugin_read_pc_vpn(void) {

  CPUState *cpu = current_cpu;
  g_assert(cpu != NULL);

  return (uint64_t)cpu->env_ptr->pc >> 12;
}

#endif