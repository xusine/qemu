#include "qemu/dynamic_barrier.h"
#include <stdio.h>
#include <time.h>
#include <unistd.h>

static uint64_t get_current_timestamp_ns(void) {
    struct timespec ts;
    // Get the current time
    clock_gettime(CLOCK_REALTIME, &ts);

    // Convert to nanoseconds
    // tv_sec is seconds, tv_nsec is nanoseconds
    uint64_t timestamp_ns = (uint64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;

    return timestamp_ns;
}

static void *report_time_peridically(void *arg) {
    dynamic_barrier_polling_t *barrier = arg;
    while (1) {
        sleep(10);
        uint64_t total_diff = barrier->total_diff;
        uint64_t generation = barrier->generation;
        printf("Total time spent in the barrier: %lu ns, generation: %lu, normalized_diff: %lf\n", total_diff, generation, (double)total_diff / generation);
    }
    return NULL;
}

// Initialize the dynamic barrier
int dynamic_barrier_init(dynamic_barrier_t *barrier, int initial_threshold) {
    int status;

    status = pthread_mutex_init(&barrier->mutex, NULL);
    if (status != 0) return status;

    status = pthread_cond_init(&barrier->cond, NULL);
    if (status != 0) {
        pthread_mutex_destroy(&barrier->mutex);
        return status;
    }

    barrier->threshold = initial_threshold;
    barrier->count = 0;
    barrier->generation = 0;

    // start another thread to call report_time_peridically.

    return 0;
}

// Destroy the dynamic barrier
int dynamic_barrier_destroy(dynamic_barrier_t *barrier) {
    pthread_mutex_destroy(&barrier->mutex);
    pthread_cond_destroy(&barrier->cond);
    return 0;
}

// Wait on the barrier
int dynamic_barrier_wait(dynamic_barrier_t *barrier) {
    pthread_mutex_lock(&barrier->mutex);

    int gen = barrier->generation;
    barrier->count++;

    if (barrier->count >= barrier->threshold) {
        barrier->generation++;
        barrier->count = 0;
        pthread_cond_broadcast(&barrier->cond);
    } else {
        while (gen == barrier->generation) {
            pthread_cond_wait(&barrier->cond, &barrier->mutex);
        }
    }

    pthread_mutex_unlock(&barrier->mutex);
    return 0;
}



int dynamic_barrier_increase_by_1(dynamic_barrier_t *barrier) {
    pthread_mutex_lock(&barrier->mutex);
    barrier->threshold = barrier->threshold + 1;
    if (barrier->count >= barrier->threshold) {
        barrier->generation++;
        barrier->count = 0;
        pthread_cond_broadcast(&barrier->cond);
    }
    pthread_mutex_unlock(&barrier->mutex);
    return 0;
}

int dynamic_barrier_decrease_by_1(dynamic_barrier_t *barrier) {
    pthread_mutex_lock(&barrier->mutex);
    if (barrier->threshold <= 0) {
        pthread_mutex_unlock(&barrier->mutex);
        return -1;
    }
    barrier->threshold = barrier->threshold - 1;
    if (barrier->count >= barrier->threshold) {
        barrier->generation++;
        barrier->count = 0;
        pthread_cond_broadcast(&barrier->cond);
    }
    pthread_mutex_unlock(&barrier->mutex);
    return 0;
}


int dynamic_barrier_polling_init(dynamic_barrier_polling_t *barrier, int initial_threshold) {
    barrier->threshold = initial_threshold;
    barrier->count = 0;
    barrier->generation = 0;

    pthread_t tid;
    pthread_create(&tid, NULL, report_time_peridically, barrier);

    return 0;
}

int dynamic_barrier_polling_destroy(dynamic_barrier_polling_t *barrier) {
    return 0;
}

int dynamic_barrier_polling_wait(dynamic_barrier_polling_t *barrier) {
    uint64_t gen = atomic_load(&barrier->generation);
    uint64_t idx = atomic_fetch_add(&barrier->count, 1);

    if (idx == 0) {
        barrier->last_timestamp = get_current_timestamp_ns();
    } 
    
    if (idx == atomic_load(&barrier->threshold) - 1) {
        // This is the last thread entering the barrier.
        barrier->total_diff += get_current_timestamp_ns() - barrier->last_timestamp;

        // reset the waiting count.
        atomic_store(&barrier->count, 0);

        // increase the generation.
        atomic_fetch_add(&barrier->generation, 1);
    } else {
        while (atomic_load(&barrier->generation) == gen) {
            // do nothing, because the current generation is not changed.
        }
    }

    return 0;
}

int dynamic_barrier_polling_increase_by_1(dynamic_barrier_polling_t *barrier) {
    atomic_fetch_add(&barrier->threshold, 1);
    return 0;
}

int dynamic_barrier_polling_decrease_by_1(dynamic_barrier_polling_t *barrier) {
    if (atomic_load(&barrier->threshold) <= 0) {
        return -1;
    }
    if (atomic_fetch_sub(&barrier->threshold, 1) == atomic_load(&barrier->count) + 1) {
        // This thread actually makes the threshold smaller than the count.
        // In case it triggers the threshold, it should release all the waiting threads.

        // reset the waiting count.
        atomic_store(&barrier->count, 0);

        // increase the generation.
        atomic_fetch_add(&barrier->generation, 1);

    }
    return 0;
}