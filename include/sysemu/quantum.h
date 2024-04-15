/**
 * @file include/sysemu/quantum.h
 * 
 * Quantum API, for configuring and using the quantum counter.
 */

#ifndef SYSEMU_QUANTUM_H
#define SYSEMU_QUANTUM_H


void quantum_configure(QemuOpts *opts, Error **errp);

bool is_vcpu_affiliated_with_quantum(uint64_t cpu_idx);

#ifdef CONFIG_TCG
extern uint64_t quantum_size;
#define quantum_enabled() (quantum_size != 0)
#else
#define quantum_enabled() 0
#endif



#endif