/**
 * @file include/qemu/plugin-cyan.h
 * 
 * This file contains the definitions for the Cyan's plugin system.
 */

#ifndef QEMU_PLUGIN_CYAN_H
#define QEMU_PLUGIN_CYAN_H

#include "qemu/qemu-plugin.h"

// This file keeps the following callbacks for the plugin system:
// - Callback for virtual time calculation.
// - Callback for branch resolution.
// - Callback for savevm (after the VM state is saved).

// The callback for virtual time calculation.
extern qemu_plugin_virtual_time_callback_t cyan_vclock_cb;

// The callback for branch resolution.
extern qemu_plugin_vcpu_branch_resolved_cb_t cyan_br_cb;

// The callback for savevm (after the VM state is saved).
extern qemu_plugin_savevm_cb_t cyan_savevm_cb;

#endif
