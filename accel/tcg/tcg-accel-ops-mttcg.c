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

#include "qemu/osdep.h"
#include "sysemu/tcg.h"
#include "qemu/typedefs.h"
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
#include "qemu/plugin-cyan.h"

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

static uint64_t get_current_timestamp_ns(void) {
    struct timespec ts;
    // Get the current time
    clock_gettime(CLOCK_REALTIME, &ts);

    // Convert to nanoseconds
    // tv_sec is seconds, tv_nsec is nanoseconds
    uint64_t timestamp_ns = (uint64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;

    return timestamp_ns;
}

typedef struct {
    uint64_t total_time;
    uint64_t execution_time;
    uint64_t waiting_time;
    uint64_t idle_time;
    uint64_t peeking_other_time;
} per_cpu_host_time_breakdown_t;

const uint64_t RECORD_SIZE = 4096;
const uint64_t SKIP_SIZE = 1000;

static void dump_log(CPUState *cpu, per_cpu_host_time_breakdown_t *statistics) {
    // open a log file.
    char log_name[100];
    snprintf(log_name, 100, "qlog/statisitcs_%u.log", cpu->cpu_index);

    FILE *fp = fopen(log_name, "w");

    // push the enter_idle_time, target_cycle_on_idle, and target_cycle_on_instruction to the statistics.
    fprintf(
        fp, 
        "%lu,%lu,%lu\n", 
        cpu->enter_idle_time, 
        cpu->target_cycle_on_idle, 
        cpu->target_cycle_on_instruction
    );

    for(uint64_t i = 0; i < RECORD_SIZE; ++i) {
        fprintf(
            fp, 
            "%lu,%lu,%lu,%lu,%lu\n", 
            statistics[i].total_time, 
            statistics[i].execution_time, 
            statistics[i].waiting_time, 
            statistics[i].idle_time, 
            statistics[i].peeking_other_time
        );
    }

    fclose(fp);
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

    cpu->ipc = 1;

    // MachineState *ms = MACHINE(qdev_get_machine());

    // if (ms->smp.cpus % 2 == 0 && cpu->cpu_index >= ms->smp.cpus / 2) {
    //     cpu->ipc = 1;
    //     printf("CPU %d is in the high IPC mode. Its IPC is %ld\n", cpu->cpu_index, cpu->ipc);
    // }

    assert(tcg_enabled());
    g_assert(!icount_enabled());

    rcu_register_thread();
    force_rcu.notifier.notify = mttcg_force_rcu;
    force_rcu.cpu = cpu;
    rcu_add_force_rcu_notifier(&force_rcu.notifier);
    tcg_register_thread();

    qemu_mutex_lock_iothread();
    qemu_thread_get_self(cpu->thread);

    // per_cpu_host_time_breakdown_t statistics[RECORD_SIZE];
    // bzero(statistics, sizeof(per_cpu_host_time_breakdown_t) * RECORD_SIZE);
    // uint64_t statistic_head_counter = 0;

    cpu->thread_id = qemu_get_thread_id();
    cpu->can_do_io = 1;


    current_cpu = cpu;
    cpu_thread_signal_created(cpu);
    qemu_guest_random_seed_thread_part2(cpu->random_seed);

    // Now we want to fix the core affinity of the current thread for better experiments.
    // On this machine, the affinity setting is very strange.
    // Core 0-15, 32-47 are on socket 0
    // Core 16-31, 48-63 are on socket 1
    // Here is the mapping
    assert(cpu->cpu_index < 64);
    uint64_t affinity_list = 0;
    if (cpu->cpu_index >= 0 && cpu->cpu_index < 16) {
        affinity_list = cpu->cpu_index;
    } else if (cpu->cpu_index >= 16 && cpu->cpu_index < 32) {
        affinity_list = cpu->cpu_index + 16;
    } else if (cpu->cpu_index >= 32 && cpu->cpu_index < 48) {
        affinity_list = cpu->cpu_index - 16;
    } else {
        affinity_list = cpu->cpu_index;
    }


    cpu_set_t cpuset;

    CPU_ZERO(&cpuset);
    CPU_SET(affinity_list, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);

    /* process any pending work */
    cpu->exit_request = 1;

    bool not_running_yet = true;

    // uint64_t dumping_threshold = 300 * 1000 * 1000; // 300M

    // uint64_t ts0 = get_current_timestamp_ns();
    do {
        if (cpu_can_run(cpu)) {
            if (not_running_yet) {
                // initialize the field that are related to the time. 
                // cpu->unknown_time = 0;
                // cpu->enter_idle_time = 0;
                // cpu->target_cycle_on_idle = 0;
                // cpu->target_cycle_on_instruction = 0;

                // // register the current thread to the barrier.
                // if (is_vcpu_affiliated_with_quantum(cpu->cpu_index)) {
                //     dynamic_barrier_polling_increase_by_1(&quantum_barrier);
                //     printf("Quantum Count: %lu \n", quantum_size);
                // }

                not_running_yet = false;
            }

            int r;
            // uint64_t ts1;
            qemu_mutex_unlock_iothread();
// cpu_resume_from_quantum:
            // ts1 = get_current_timestamp_ns();
            r = tcg_cpus_exec(cpu);
            // if (statistic_head_counter < RECORD_SIZE + SKIP_SIZE && statistic_head_counter > SKIP_SIZE) {
            //     statistics[statistic_head_counter - SKIP_SIZE].execution_time += (get_current_timestamp_ns() - ts1); // record the execution time.
            // }
            // check the quantum budget and sync before doing I/O operation.
            // if (cpu->env_ptr->quantum_budget_depleted) {
            //     cpu->env_ptr->quantum_budget_depleted = false;
            //     if (is_vcpu_affiliated_with_quantum(cpu->cpu_index)) {
            //         while (cpu->env_ptr->quantum_budget_and_generation.separated.quantum_budget <= 0) {
            //             // We need to wait for all the vCPUs to finish their quantum.
            //             uint32_t old_generation = cpu->env_ptr->quantum_budget_and_generation.separated.quantum_generation;
            //             // assert(old_generation == statistic_head_counter);
            //             // uint64_t start_waiting = get_current_timestamp_ns();
            //             uint64_t new_generation = dynamic_barrier_polling_wait(&quantum_barrier, cpu->env_ptr->quantum_budget_and_generation.separated.quantum_generation);
                        
            //             // check whether there is a overflow from the new generation to the old generation.
            //             bool overflow = (new_generation < old_generation);

            //             // We the need to increase the upper bound generation number when there is a overflow.
            //             if (overflow) {
            //                 cpu->env_ptr->quantum_generation_upper32 += 1;
            //             }
                        
            //             assert(new_generation == old_generation + 1);
            //             // if (statistic_head_counter < RECORD_SIZE + SKIP_SIZE && statistic_head_counter > SKIP_SIZE) {
            //             //     statistics[statistic_head_counter-SKIP_SIZE].waiting_time += (get_current_timestamp_ns() - start_waiting); // increase the idle time.
            //             // }

            //             uint64_t new_budget_and_generation = cpu->env_ptr->quantum_budget_and_generation.separated.quantum_budget + quantum_size;
            //             new_budget_and_generation = new_budget_and_generation << 32 | new_generation;
            //             cpu->env_ptr->quantum_budget_and_generation.combined = new_budget_and_generation;

            //             // if (statistic_head_counter < RECORD_SIZE + SKIP_SIZE && statistic_head_counter > SKIP_SIZE) {
            //             //     statistics[statistic_head_counter - SKIP_SIZE].total_time = (get_current_timestamp_ns() - ts0);
            //             // }

            //             // statistic_head_counter += 1;
            //             // assert(statistic_head_counter == new_generation);

            //             // if (statistic_head_counter > SKIP_SIZE && (statistic_head_counter - SKIP_SIZE) * quantum_size > dumping_threshold) {
            //             //     dump_log(cpu, statistics);
            //             //     dumping_threshold += 300 * 1000 * 1000;
            //             // }


            //             // ts0 = get_current_timestamp_ns(); // reset the starting time of the next quantum. 
            //         }
                    
            //         // We need to reset the quantum budget of the current vCPU.
            //         if (r == EXCP_QUANTUM) {
            //             goto cpu_resume_from_quantum;
            //         }
            //     } else {
            //         assert(false);
            //     }
            // }
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
                // Well, it is possible that this atomic step may deplete the quantum budget.
                // What we have to do now is to give enough quantum budget to this CPU, and remove it afterwards. 
                // int64_t quantum_for_deduction = cpu->env_ptr->quantum_required / cpu->ipc;
                // We need to sync immediately to get the quantum budget. 
                // if (is_vcpu_affiliated_with_quantum(cpu->cpu_index)) {
                //     while (cpu->env_ptr->quantum_budget_and_generation.separated.quantum_budget <= quantum_for_deduction) {
                //         uint32_t old_generation = cpu->env_ptr->quantum_budget_and_generation.separated.quantum_generation;
                //         // assert(old_generation == statistic_head_counter);
                //         uint64_t start_waiting = get_current_timestamp_ns();
                //         uint64_t new_generation = dynamic_barrier_polling_wait(&quantum_barrier, cpu->env_ptr->quantum_budget_and_generation.separated.quantum_generation);
                        
                //         bool overflow = (new_generation < old_generation);

                //         // We the need to increase the upper bound generation number when there is a overflow.
                //         if (overflow) {
                //             cpu->env_ptr->quantum_generation_upper32 += 1;
                //         }
                //         assert(new_generation == old_generation + 1);
                        
                //         // if (statistic_head_counter < RECORD_SIZE + SKIP_SIZE && statistic_head_counter > SKIP_SIZE) {
                //         //     statistics[statistic_head_counter-SKIP_SIZE].waiting_time += (get_current_timestamp_ns() - start_waiting); // increase the idle time.
                //         // }

                //         uint64_t new_budget_and_generation = cpu->env_ptr->quantum_budget_and_generation.separated.quantum_budget + quantum_size;
                //         new_budget_and_generation = new_budget_and_generation << 32 | new_generation;
                //         cpu->env_ptr->quantum_budget_and_generation.combined = new_budget_and_generation;

                //         // last quantum ends here. 
                //         // if (statistic_head_counter < RECORD_SIZE + SKIP_SIZE && statistic_head_counter > SKIP_SIZE) {
                //         //     statistics[statistic_head_counter - SKIP_SIZE].total_time = (get_current_timestamp_ns() - ts0);
                //         // }

                //         // statistic_head_counter += 1;
                //         // assert(statistic_head_counter == new_generation);

                //         // if (statistic_head_counter > SKIP_SIZE && (statistic_head_counter - SKIP_SIZE) * quantum_size > dumping_threshold) {
                //         //     dump_log(cpu, statistics);
                //         //     dumping_threshold += 300 * 1000 * 1000;
                //         // }

                //         ts0 = get_current_timestamp_ns(); // reset the starting time of the next quantum.
                //     }
                // }
                // assert(cpu->env_ptr->quantum_budget_depleted == false);
                // // Now it is safe to execute the next instruction. It will not trigger quantum depleting and rollback.
                // ts1 = get_current_timestamp_ns();
                cpu_exec_step_atomic(cpu);
                // if (statistic_head_counter < RECORD_SIZE + SKIP_SIZE && statistic_head_counter > SKIP_SIZE) {
                //     statistics[statistic_head_counter - SKIP_SIZE].execution_time += (get_current_timestamp_ns() - ts1); // record the execution time.
                // }
                // It is impossible to see the quantum is depleted here, because we leave the budget for the exclusive instruction.
                // assert(cpu->env_ptr->quantum_budget_depleted == false);
                // exclusive_icount += 1;
                qemu_mutex_lock_iothread();
            default:
                /* Ignore everything else? */
                break;
            }
        }

        qatomic_set_mb(&cpu->exit_request, 0);
        uint32_t current_quantum_generation = 0;
        // uint64_t wait_start_ts = get_current_timestamp_ns();
        qemu_wait_io_event(cpu, not_running_yet, &current_quantum_generation);

        // Activate the plugin and see if it can run any delayed tasks.
        if (cyan_el_pool_cb) {
            cyan_el_pool_cb();
        }
        // if (statistic_head_counter < RECORD_SIZE + SKIP_SIZE && statistic_head_counter > SKIP_SIZE) {
        //     statistics[statistic_head_counter - SKIP_SIZE].idle_time += (get_current_timestamp_ns() - wait_start_ts); // increase the idle time.
        // }
        // uint32_t old_generation = cpu->env_ptr->quantum_budget_and_generation.separated.quantum_generation;

        // assert(statistic_head_counter == old_generation);
        // if (statistic_head_counter != old_generation) {
        //     printf("statistic_head_counter: %lu, old_generation: %u\n", statistic_head_counter, old_generation);
        // }
        
        // uint64_t time_calculation_ts = get_current_timestamp_ns();

        // if (!not_running_yet && has_slept && is_vcpu_affiliated_with_quantum(cpu->cpu_index)) {
        //     // We need to update the quantum budget of the current vCPU.
        //     // We assume that the idle thread update the quantum in the same way as other threads. 
        //     // This latency may also go across multiple quanta, but this part can be cancelled, because we assume they wait for the quantum barrier.
        //     // Now, the problem is that remaining. This part should be deducted from the quantum budget.

        //     // We can look at what other CPUs are doing right now.
        //     assert(current_quantum_generation >= old_generation); 
        //     assert(cpu->unknown_time == 1);

        //     // so the statistics between [old_generation,  new_generation) is done. 
        //     for(uint64_t generation_idx = old_generation; generation_idx < current_quantum_generation; ++generation_idx) {
        //         // End the current one.
        //         // if (statistic_head_counter < RECORD_SIZE + SKIP_SIZE && statistic_head_counter > SKIP_SIZE) {
        //         //     statistics[statistic_head_counter - SKIP_SIZE].total_time = (get_current_timestamp_ns() - ts0);
        //         // }
        //         // statistic_head_counter += 1;
        //         // if (statistic_head_counter > SKIP_SIZE && (statistic_head_counter - SKIP_SIZE) * quantum_size > dumping_threshold) {
        //         //     dump_log(cpu, statistics);
        //         //     dumping_threshold += 300 * 1000 * 1000;
        //         // }
        //         // ts0 = get_current_timestamp_ns(); // reset the starting time of the next quantum. 
        //     }

        //     assert(statistic_head_counter == current_quantum_generation);

        //     cpu->enter_idle_time += 1;

        //     int32_t old_budget = cpu->env_ptr->quantum_budget_and_generation.separated.quantum_budget;

        //     CPUState *iter_cpu;
        //     uint64_t current_generation_count = 0;
        //     uint64_t current_generation_budget = 0;

        //     CPU_FOREACH(iter_cpu) {
        //         if (iter_cpu->unknown_time == 0 && iter_cpu != cpu && is_vcpu_affiliated_with_quantum(iter_cpu->cpu_index)) {
        //             uint64_t other_cpu_budget_and_generation = iter_cpu->env_ptr->quantum_budget_and_generation.combined;
        //             int32_t other_cpu_budget = other_cpu_budget_and_generation >> 32;
        //             uint32_t other_cpu_generation = other_cpu_budget_and_generation & 0xFFFFFFFF;
        //             if (other_cpu_generation == current_quantum_generation) {
        //                 if (other_cpu_budget < 0) other_cpu_budget = 0;
        //                 current_generation_budget += other_cpu_budget;
        //                 current_generation_count += 1;
        //             } else {
        //                 assert(other_cpu_budget <= 0);
        //             }
        //         }
        //     }

        //     uint64_t old_generation = cpu->env_ptr->quantum_budget_and_generation.separated.quantum_generation;
        //     bool overflow = (current_quantum_generation < old_generation);

        //     if (overflow) {
        //         cpu->env_ptr->quantum_generation_upper32 += 1;
        //     }

        //     if (current_generation_count) {
        //         int32_t budget = current_generation_budget / current_generation_count;
        //         if (budget < old_budget || old_generation != current_quantum_generation) { // we only apply the new budget when it is smaller than the consumption, or we are in a new generation.
        //             uint64_t new_budget_and_generation = (((uint64_t)budget) << 32) | current_quantum_generation;
        //             cpu->env_ptr->quantum_budget_and_generation.combined = new_budget_and_generation;

        //             if (current_quantum_generation > old_generation) {
        //                 // crossing multiple generations.
        //                 cpu->target_cycle_on_idle += (current_quantum_generation - old_generation - 1) * quantum_size;

        //                 if (old_budget >= 0) {
        //                     cpu->target_cycle_on_idle += (quantum_size - budget) + old_budget;
        //                 } else {
        //                     old_budget = -old_budget;
        //                     cpu->target_cycle_on_idle -= old_budget;
        //                 }
        //             } else {
        //                 assert(old_budget >= 0);
        //                 cpu->target_cycle_on_idle += (old_budget - budget);
        //             }
        //         }
        //     } else {
        //         // this must be the beginning of the quantum. 
        //         if (current_quantum_generation > old_generation) {
        //             uint64_t new_budget_and_generation = (((uint64_t)quantum_size) << 32) | current_quantum_generation;
        //             cpu->env_ptr->quantum_budget_and_generation.combined = new_budget_and_generation;

        //             if (old_budget >= 0) {
        //                 cpu->target_cycle_on_idle += (current_quantum_generation - old_generation - 1) * quantum_size + old_budget;
        //             } else {
        //                 old_budget = -old_budget;
        //                 cpu->target_cycle_on_idle += (current_quantum_generation - old_generation - 1) * quantum_size - old_budget;
        //             }

        //         } else {
        //             // This means there is no way to know the time, because all other cores are in the unknown state. 
        //             // We just assume that yout time is not changed.  You are still in the current quantum and leave everything unchanged. 
        //         }
        //     }

        //     cpu->unknown_time = 0;
        // }

        // if (statistic_head_counter < RECORD_SIZE + SKIP_SIZE && statistic_head_counter > SKIP_SIZE) {
        //     statistics[statistic_head_counter - SKIP_SIZE].peeking_other_time += (get_current_timestamp_ns() - time_calculation_ts); // increase the idle time.
        // }
        
    } while (!cpu->unplug || cpu_can_run(cpu));

    tcg_cpus_destroy(cpu);
    qemu_mutex_unlock_iothread();
    rcu_remove_force_rcu_notifier(&force_rcu.notifier);
    rcu_unregister_thread();

    // resign the current thread from the barrier.
    if (is_vcpu_affiliated_with_quantum(cpu->cpu_index)) {
        dynamic_barrier_polling_decrease_by_1(&quantum_barrier);

        // also, print the histogram.
        // open a log file.
        char log_name[100];
        snprintf(log_name, 100, "quantum_histogram_%d.log", cpu->cpu_index);

        FILE *fp = fopen(log_name, "w");
        print_histogram(quantum_barrier.histogram[cpu->cpu_index], fp);
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
