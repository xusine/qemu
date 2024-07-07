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

static void atomic_update_cpu_quantum_info(CPUState *cpu, uint32_t generation, int32_t budget) {
    uint64_t imm = ((uint64_t) budget << 32) | generation;
    atomic_store((uint64_t *)&cpu->quantum_info, imm);
}

static void atomic_read_cpu_quantum_info(CPUState *cpu, uint32_t *generation, int32_t *budget) {
    uint64_t imm = atomic_load((uint64_t *)&cpu->quantum_info);
    *generation = imm & 0xFFFFFFFF;
    *budget = (imm >> 32) & 0xFFFFFFFF;
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

    per_cpu_host_time_breakdown_t statistics[RECORD_SIZE];
    bzero(statistics, sizeof(per_cpu_host_time_breakdown_t) * RECORD_SIZE);
    uint64_t statistic_head_counter = 0;

    cpu->thread_id = qemu_get_thread_id();
    cpu->can_do_io = 1;


    current_cpu = cpu;
    cpu_thread_signal_created(cpu);
    qemu_guest_random_seed_thread_part2(cpu->random_seed);

    // Now we want to fix the core affinity of the current thread for better experiments.
    // The thread is bind to the core 0.
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu->cpu_index, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);

    /* process any pending work */
    cpu->exit_request = 1;

    bool not_running_yet = true;

    // uint64_t dumping_threshold = 300 * 1000 * 1000; // 300M

    uint64_t ts0 = get_current_timestamp_ns();
    do {
        if (cpu_can_run(cpu)) {
            if (not_running_yet) {
                // initialize the field that are related to the time. 
                cpu->unknown_time = 0;
                cpu->enter_idle_time = 0;
                cpu->target_cycle_on_idle = 0;
                cpu->target_cycle_on_instruction = 0;
                cpu->last_synced_target_time = 0;
                atomic_update_cpu_quantum_info(cpu, 0, quantum_size);
                cpu->current_quantum_size = quantum_size; // this should be set properly as well. 
                cpu->quantum_budget_depleted = false;
                cpu->kicker_time = 0;

                // register the current thread to the barrier.
                if (is_vcpu_affiliated_with_quantum(cpu->cpu_index)) {
                    dynamic_barrier_polling_increase_by_1(&quantum_barrier);
                    printf("CPU %u is managed by the quantum \n", cpu->cpu_index);
                }

                not_running_yet = false;
            }

            int r;
            uint64_t ts1;
            qemu_mutex_unlock_iothread();
cpu_resume_from_quantum:
            ts1 = get_current_timestamp_ns();
            r = tcg_cpus_exec(cpu);
            if (statistic_head_counter < RECORD_SIZE + SKIP_SIZE && statistic_head_counter > SKIP_SIZE) {
                statistics[statistic_head_counter - SKIP_SIZE].execution_time += (get_current_timestamp_ns() - ts1); // record the execution time.
            }
            // check the quantum budget and sync before doing I/O operation.
            if (cpu->quantum_budget_depleted) {
                cpu->quantum_budget_depleted = false;
                if (is_vcpu_affiliated_with_quantum(cpu->cpu_index)) {
                    while (cpu->quantum_info.budget <= 0) {
                        // We need to wait for all the vCPUs to finish their quantum.
                        uint64_t start_waiting = get_current_timestamp_ns();
                        dynamic_barrier_polling_wait(&quantum_barrier);
                        int32_t budget = quantum_barrier.current_generation_budget + cpu->quantum_info.budget;
                        atomic_update_cpu_quantum_info(cpu, quantum_barrier.generation, budget);
                        cpu->current_quantum_size = quantum_barrier.current_generation_budget;
                        cpu->last_synced_target_time = quantum_barrier.current_system_target_time;
                        assert(quantum_barrier.current_system_target_time - cpu->last_synced_target_time <= quantum_size);
                        
                        if (statistic_head_counter < RECORD_SIZE + SKIP_SIZE && statistic_head_counter > SKIP_SIZE) {
                            statistics[statistic_head_counter-SKIP_SIZE].waiting_time += (get_current_timestamp_ns() - start_waiting); // increase the idle time.
                        }

                        if (statistic_head_counter < RECORD_SIZE + SKIP_SIZE && statistic_head_counter > SKIP_SIZE) {
                            statistics[statistic_head_counter - SKIP_SIZE].total_time = (get_current_timestamp_ns() - ts0);
                        }

                        statistic_head_counter += 1;

                        ts0 = get_current_timestamp_ns(); // reset the starting time of the next quantum. 
                    }
                    
                    // We need to reset the quantum budget of the current vCPU.
                    if (r == EXCP_QUANTUM) {
                        goto cpu_resume_from_quantum;
                    }
                } else {
                    assert(false);
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
                // Well, it is possible that this atomic step may deplete the quantum budget.
                // What we have to do now is to give enough quantum budget to this CPU, and remove it afterwards. 
                int64_t quantum_for_deduction = cpu->env_ptr->quantum_required;
                // We need to sync immediately to get the quantum budget. 
                if (is_vcpu_affiliated_with_quantum(cpu->cpu_index)) {
                    while (cpu->quantum_info.budget <= quantum_for_deduction) {
                        uint64_t start_waiting = get_current_timestamp_ns();
                        dynamic_barrier_polling_wait(&quantum_barrier);
                        int32_t budget = quantum_barrier.current_generation_budget + cpu->quantum_info.budget;
                        atomic_update_cpu_quantum_info(cpu, quantum_barrier.generation, budget);
                        cpu->current_quantum_size = quantum_barrier.current_generation_budget;
                        cpu->last_synced_target_time = quantum_barrier.current_system_target_time;
                        assert(quantum_barrier.current_system_target_time - cpu->last_synced_target_time <= quantum_size);
                        
                        if (statistic_head_counter < RECORD_SIZE + SKIP_SIZE && statistic_head_counter > SKIP_SIZE) {
                            statistics[statistic_head_counter-SKIP_SIZE].waiting_time += (get_current_timestamp_ns() - start_waiting); // increase the idle time.
                        }

                        // last quantum ends here. 
                        if (statistic_head_counter < RECORD_SIZE + SKIP_SIZE && statistic_head_counter > SKIP_SIZE) {
                            statistics[statistic_head_counter - SKIP_SIZE].total_time = (get_current_timestamp_ns() - ts0);
                        }

                        statistic_head_counter += 1;
                        ts0 = get_current_timestamp_ns(); // reset the starting time of the next quantum.
                    }
                }
                assert(cpu->quantum_budget_depleted == false);
                // Now it is safe to execute the next instruction. It will not trigger quantum depleting and rollback.
                ts1 = get_current_timestamp_ns();
                cpu_exec_step_atomic(cpu);
                if (statistic_head_counter < RECORD_SIZE + SKIP_SIZE && statistic_head_counter > SKIP_SIZE) {
                    statistics[statistic_head_counter - SKIP_SIZE].execution_time += (get_current_timestamp_ns() - ts1); // record the execution time.
                }
                // It is impossible to see the quantum is depleted here, because we leave the budget for the exclusive instruction.
                assert(cpu->quantum_budget_depleted == false);
                // exclusive_icount += 1;
                qemu_mutex_lock_iothread();
            default:
                /* Ignore everything else? */
                break;
            }
        }

        qatomic_set_mb(&cpu->exit_request, 0);
        uint64_t wait_start_ts = get_current_timestamp_ns();
        bool has_slept = qemu_wait_io_event(cpu, not_running_yet);
        uint64_t current_target_time = quantum_barrier.current_system_target_time;
        uint32_t current_generation = quantum_barrier.generation;
        if (statistic_head_counter < RECORD_SIZE + SKIP_SIZE && statistic_head_counter > SKIP_SIZE) {
            statistics[statistic_head_counter - SKIP_SIZE].idle_time += (get_current_timestamp_ns() - wait_start_ts); // increase the idle time.
        }
        
        uint64_t time_calculation_ts = get_current_timestamp_ns();

        if (!not_running_yet && has_slept && is_vcpu_affiliated_with_quantum(cpu->cpu_index)) {
            // We need to update the quantum budget of the current vCPU.
            // We assume that the idle thread update the quantum in the same way as other threads. 
            // This latency may also go across multiple quanta, but this part can be cancelled, because we assume they wait for the quantum barrier.
            // Now, the problem is that remaining. This part should be deducted from the quantum budget.

            // We can look at what other CPUs are doing right now.
            assert(cpu->unknown_time == 1);

            // so the statistics between [old_generation,  new_generation) is done. 
            // TODO: This part requires a rework, because we don't return the target time.
            // for(uint64_t generation_idx = old_generation; generation_idx < current_quantum_generation; ++generation_idx) {
            //     // End the current one.
            //     if (statistic_head_counter < RECORD_SIZE + SKIP_SIZE && statistic_head_counter > SKIP_SIZE) {
            //         statistics[statistic_head_counter - SKIP_SIZE].total_time = (get_current_timestamp_ns() - ts0);
            //     }
            //     statistic_head_counter += 1;
            //     // if (statistic_head_counter > SKIP_SIZE && (statistic_head_counter - SKIP_SIZE) * quantum_size > dumping_threshold) {
            //     //     dump_log(cpu, statistics);
            //     //     dumping_threshold += 300 * 1000 * 1000;
            //     // }
            //     ts0 = get_current_timestamp_ns(); // reset the starting time of the next quantum. 
            // }


            cpu->enter_idle_time += 1;

            // uint32_t old_generation = cpu->quantum_info.generation;
            int32_t old_budget = cpu->quantum_info.budget;

            CPUState *iter_cpu;
            uint64_t current_generation_count = 0;
            uint64_t current_generation_budgets = 0;

            CPU_FOREACH(iter_cpu) {
                if (iter_cpu->unknown_time == 0 && iter_cpu != cpu && is_vcpu_affiliated_with_quantum(iter_cpu->cpu_index)) {
                    int32_t its_budget;
                    uint32_t its_generation;
                    atomic_read_cpu_quantum_info(iter_cpu, &its_generation, &its_budget);

                    if (its_generation == current_generation) {
                        int64_t other_cpu_budget = its_budget;
                        if (other_cpu_budget < 0) other_cpu_budget = 0;
                        current_generation_budgets += other_cpu_budget;
                        current_generation_count += 1;
                    } else {
                        assert(its_budget <= 0);
                    }
                }
            }

            uint64_t old_target_time = cpu->last_synced_target_time;

            if (current_generation_count) {
                int64_t budget = current_generation_budgets / current_generation_count;
                if (budget < old_budget || old_target_time != current_target_time) { // we only apply the new budget when it is smaller than the consumption, or we are in a new generation.
                    atomic_update_cpu_quantum_info(cpu, current_generation, budget);
                    cpu->current_quantum_size = quantum_barrier.current_generation_budget;
                    cpu->last_synced_target_time = quantum_barrier.current_system_target_time;                    

                    if (current_target_time > old_target_time) {
                        // crossing multiple generations.
                        cpu->target_cycle_on_idle += current_target_time - old_target_time;

                        if (old_budget >= 0) {
                            cpu->target_cycle_on_idle += (quantum_barrier.current_generation_budget - budget) + old_budget;
                        } else {
                            old_budget = -old_budget;
                            cpu->target_cycle_on_idle -= old_budget;
                        }
                    } else {
                        assert(old_budget >= 0);
                        cpu->target_cycle_on_idle += (old_budget - budget);
                    }
                }
            } else {
                // this must be the beginning of the quantum. 
                if (current_target_time > old_target_time) {
                    atomic_update_cpu_quantum_info(cpu, current_generation, (int32_t)quantum_barrier.current_generation_budget);
                    cpu->current_quantum_size = quantum_barrier.current_generation_budget;
                    cpu->last_synced_target_time = quantum_barrier.current_system_target_time;     


                    if (old_budget >= 0) {
                        cpu->target_cycle_on_idle += current_target_time - old_target_time + old_budget;
                    } else {
                        old_budget = -old_budget;
                        cpu->target_cycle_on_idle += current_target_time - old_target_time - old_budget;
                    }

                } else {
                    // This means there is no way to know the time, because all other cores are in the unknown state. 
                    // We just assume that yout time is not changed.  You are still in the current quantum and leave everything unchanged. 
                }
            }

            cpu->unknown_time = 0;
        }

        if (statistic_head_counter < RECORD_SIZE + SKIP_SIZE && statistic_head_counter > SKIP_SIZE) {
            statistics[statistic_head_counter - SKIP_SIZE].peeking_other_time += (get_current_timestamp_ns() - time_calculation_ts); // increase the idle time.
        }
        
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
