/*
 * QEMU TCG Multi Threaded vCPUs implementation
 *
 * Copyright (c) 2003-2008 Fabrice Bellard
 * Copyright (c) 2014 Red Hat Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/histogram.h"
#include "qemu/osdep.h"
#include "sysemu/tcg.h"
#include "sysemu/replay.h"
#include "sysemu/cpu-timers.h"
#include "qemu/main-loop.h"
#include "qemu/notify.h"
#include "qemu/guest-random.h"
#include "exec/exec-all.h"
#include "hw/boards.h"
#include "tcg/tcg.h"
#include "tcg-accel-ops.h"
#include "tcg-accel-ops-mttcg.h"

#include "qemu/dynamic_barrier.h"
#include "sysemu/quantum.h"
#include <stdio.h>

// const uint64_t QUANTUM_SIZE = 1000000; // 1M
dynamic_barrier_polling_t quantum_barrier;

typedef struct MttcgForceRcuNotifier {
    Notifier notifier;
    CPUState *cpu;
} MttcgForceRcuNotifier;

static void do_nothing(CPUState *cpu, run_on_cpu_data d)
{
}

static void mttcg_force_rcu(Notifier *notify, void *data)
{
    CPUState *cpu = container_of(notify, MttcgForceRcuNotifier, notifier)->cpu;

    /*
     * Called with rcu_registry_lock held, using async_run_on_cpu() ensures
     * that there are no deadlocks.
     */
    async_run_on_cpu(cpu, do_nothing, RUN_ON_CPU_NULL);
}

/*
 * In the multi-threaded case each vCPU has its own thread. The TLS
 * variable current_cpu can be used deep in the code to find the
 * current CPUState for a given thread.
 */

static void *mttcg_cpu_thread_fn(void *arg)
{
    MttcgForceRcuNotifier force_rcu;
    CPUState *cpu = arg;

    assert(tcg_enabled());
    g_assert(!icount_enabled());

    rcu_register_thread();
    force_rcu.notifier.notify = mttcg_force_rcu;
    force_rcu.cpu = cpu;
    rcu_add_force_rcu_notifier(&force_rcu.notifier);
    tcg_register_thread();

    qemu_mutex_lock_iothread();
    qemu_thread_get_self(cpu->thread);

    // register the current thread to the barrier.
    if (is_vcpu_affiliated_with_quantum(cpu->cpu_index)) {
        dynamic_barrier_polling_increase_by_1(&quantum_barrier);
        printf("Quantum Count: %lu \n", quantum_size);
    }

    uint64_t cpu_index = cpu->cpu_index;

    cpu->thread_id = qemu_get_thread_id();
    cpu->can_do_io = 1;

    current_cpu = cpu;
    cpu_thread_signal_created(cpu);
    qemu_guest_random_seed_thread_part2(cpu->random_seed);

    // Now we want to fix the core affinity of the current thread for better experiments.
    // The thread is bind to the core 0.
    // cpu_set_t cpuset;
    // CPU_ZERO(&cpuset);
    // CPU_SET(cpu->cpu_index, &cpuset);
    // pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);

    /* process any pending work */
    cpu->exit_request = 1;

    char histogram_name[100];
    sprintf(histogram_name, "instruction_histogram_%lu.log", cpu_index);
    FILE *instruction_histogram = fopen(histogram_name, "w");

    do {
        if (cpu_can_run(cpu)) {
            int r;
            qemu_mutex_unlock_iothread();
cpu_resume_from_quantum:
            r = tcg_cpus_exec(cpu);
            // check the quantum budget and sync before doing I/O operation.
            if (cpu->env_ptr->quantum_budget_depleted) {
                if (is_vcpu_affiliated_with_quantum(cpu->cpu_index)) {
                    while (cpu->env_ptr->quantum_budget <= 0) {
                        // We need to wait for all the vCPUs to finish their quantum.
                        dynamic_barrier_polling_wait(&quantum_barrier); 
                        cpu->env_ptr->quantum_budget += quantum_size;
                    }
                }
                // We need to reset the quantum budget of the current vCPU.
                cpu->env_ptr->quantum_budget_depleted = false;
                if (r == EXCP_QUANTUM) {
                    goto cpu_resume_from_quantum;
                }
            }
            qemu_mutex_lock_iothread();
            switch (r) {
            case EXCP_DEBUG:
                cpu_handle_guest_debug(cpu);
                break;
            case EXCP_HALTED:
                /*
                 * Usually cpu->halted is set, but may have already been
                 * reset by another thread by the time we arrive here.
                 */
                break;
            case EXCP_ATOMIC:
                qemu_mutex_unlock_iothread();
                cpu_exec_step_atomic(cpu);
                qemu_mutex_lock_iothread();
            default:
                /* Ignore everything else? */
                break;
            }
        }

        qatomic_set_mb(&cpu->exit_request, 0);
        uint64_t idle_latency = qemu_wait_io_event(cpu);

        if (is_vcpu_affiliated_with_quantum(cpu->cpu_index)) {
            // We need to update the quantum budget of the current vCPU.
            // We assume that the idle thread update the quantum in the same way as other threads. 
            // This latency may also go across multiple quanta, but this part can be cancelled, because we assume they wait for the quantum barrier.
            // Now, the problem is that remaining. This part should be deducted from the quantum budget.

            uint64_t io_latency_after_deducing_quantum = idle_latency % quantum_size;
            cpu->env_ptr->quantum_required += io_latency_after_deducing_quantum;
        }
        
    } while (!cpu->unplug || cpu_can_run(cpu));

    print_histogram(cpu->env_ptr->instruction_histogram, instruction_histogram);
    fflush(instruction_histogram);
    fclose(instruction_histogram);
    
    tcg_cpus_destroy(cpu);
    qemu_mutex_unlock_iothread();
    rcu_remove_force_rcu_notifier(&force_rcu.notifier);
    rcu_unregister_thread();

    // resign the current thread from the barrier.
    if (is_vcpu_affiliated_with_quantum(cpu_index)) {
        dynamic_barrier_polling_decrease_by_1(&quantum_barrier);

        puts("Thread unregistered from the barrier.");

        // also, print the histogram.
        // open a log file.
        char log_name[100];
        snprintf(log_name, 100, "quantum_histogram_%lu.log", cpu_index);

        FILE *fp = fopen(log_name, "w");
        print_histogram(quantum_barrier.histogram[cpu_index], fp);
        fclose(fp);
    }

    return NULL;
}

void mttcg_kick_vcpu_thread(CPUState *cpu)
{
    cpu_exit(cpu);
}

void mttcg_start_vcpu_thread(CPUState *cpu)
{
    char thread_name[VCPU_THREAD_NAME_SIZE];

    g_assert(tcg_enabled());
    tcg_cpu_init_cflags(cpu, current_machine->smp.max_cpus > 1);

    cpu->thread = g_new0(QemuThread, 1);
    cpu->halt_cond = g_malloc0(sizeof(QemuCond));
    qemu_cond_init(cpu->halt_cond);

    /* create a thread per vCPU with TCG (MTTCG) */
    snprintf(thread_name, VCPU_THREAD_NAME_SIZE, "CPU %d/TCG",
             cpu->cpu_index);

    qemu_thread_create(cpu->thread, thread_name, mttcg_cpu_thread_fn,
                       cpu, QEMU_THREAD_JOINABLE);
}

void mttcg_initialize_barrier(void) {
    dynamic_barrier_polling_init(&quantum_barrier, 0);
}