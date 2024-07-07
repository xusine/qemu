#ifndef QEMU_DYNAMIC_BARRIER_H
#define QEMU_DYNAMIC_BARRIER_H

#include <stdint.h>
#include <pthread.h>
#include <stdatomic.h>


#include "histogram.h"



typedef struct {
    pthread_mutex_t mutex;      // Mutex for locking
    pthread_cond_t cond;        // Condition variable for waiting
    int threshold;              // The number of threads required to proceed
    int count;                  // The current count of waiting threads
    int generation;             // Generation count to handle spurious wakeups
} dynamic_barrier_t;

// These functions are implemented in dynamic_barrier.c. We will use them in the cpus.c and mttcg-ops.c.
// int dynamic_barrier_init(dynamic_barrier_t *barrier, int initial_threshold);
// int dynamic_barrier_destroy(dynamic_barrier_t *barrier);
// int dynamic_barrier_wait(dynamic_barrier_t *barrier);
// // int dynamic_barrier_wait_with_periodical_wakeup(dynamic_barrier_t *barrier, int sleep_time);
// // int dynamic_barrier_wait_using_polling(dynamic_barrier_t *barrier);
// int dynamic_barrier_increase_by_1(dynamic_barrier_t *barrier);
// int dynamic_barrier_decrease_by_1(dynamic_barrier_t *barrier);


typedef struct {
    struct {
        atomic_uint_fast64_t next_ticket;
        atomic_uint_fast64_t now_serving;
    } lock;
    uint64_t __padding1__[6];
    uint64_t threshold;
    uint64_t __padding2__[7];
    uint64_t count;
    uint64_t __padding3__[7];
    atomic_uint_fast32_t generation;


    uint64_t current_system_target_time;
    uint64_t current_generation_budget;


    uint64_t last_timestamp;
    uint64_t total_diff;
    time_histogram_t *histogram[128]; // each core has its own histogram.
} dynamic_barrier_polling_t;

int dynamic_barrier_polling_init(dynamic_barrier_polling_t *barrier, int initial_threshold);
int dynamic_barrier_polling_destroy(dynamic_barrier_polling_t *barrier);
void dynamic_barrier_polling_wait(dynamic_barrier_polling_t *barrier); // return the target time of the last quantum boundary.
uint64_t dynamic_barrier_polling_increase_by_1(dynamic_barrier_polling_t *barrier); // return the current global target time while this thread is added. 
int dynamic_barrier_polling_decrease_by_1(dynamic_barrier_polling_t *barrier);
void dynamic_barrier_polling_reset(dynamic_barrier_polling_t *barrier);
int64_t dynamic_barrier_polling_evaluate_host_time(dynamic_barrier_polling_t *barrier); 


#endif