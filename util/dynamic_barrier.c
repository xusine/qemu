#include "qemu/dynamic_barrier.h"
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include "qemu/osdep.h"
#include "sysemu/quantum.h"

#include "hw/core/cpu.h"


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

// create a timestamp of each thread. 
static __thread uint64_t thread_start_quantum_timestamp = 0;

// // Initialize the dynamic barrier
// int dynamic_barrier_init(dynamic_barrier_t *barrier, int initial_threshold) {
//     int status;

//     status = pthread_mutex_init(&barrier->mutex, NULL);
//     if (status != 0) return status;

//     status = pthread_cond_init(&barrier->cond, NULL);
//     if (status != 0) {
//         pthread_mutex_destroy(&barrier->mutex);
//         return status;
//     }

//     barrier->threshold = initial_threshold;
//     barrier->count = 0;
//     barrier->generation = 0;

//     // start another thread to call report_time_peridically.

//     return 0;
// }

// // Destroy the dynamic barrier
// int dynamic_barrier_destroy(dynamic_barrier_t *barrier) {
//     pthread_mutex_destroy(&barrier->mutex);
//     pthread_cond_destroy(&barrier->cond);
//     return 0;
// }

// // Wait on the barrier
// int dynamic_barrier_wait(dynamic_barrier_t *barrier) {
//     pthread_mutex_lock(&barrier->mutex);

//     int gen = barrier->generation;
//     barrier->count++;

//     if (barrier->count >= barrier->threshold) {
//         barrier->generation++;
//         barrier->count = 0;
//         pthread_cond_broadcast(&barrier->cond);
//     } else {
//         while (gen == barrier->generation) {
//             pthread_cond_wait(&barrier->cond, &barrier->mutex);
//         }
//     }

//     pthread_mutex_unlock(&barrier->mutex);
//     return 0;
// }



// int dynamic_barrier_increase_by_1(dynamic_barrier_t *barrier) {
//     pthread_mutex_lock(&barrier->mutex);
//     barrier->threshold = barrier->threshold + 1;
//     if (barrier->count >= barrier->threshold) {
//         barrier->generation++;
//         barrier->count = 0;
//         pthread_cond_broadcast(&barrier->cond);
//     }
//     pthread_mutex_unlock(&barrier->mutex);
//     return 0;
// }

// int dynamic_barrier_decrease_by_1(dynamic_barrier_t *barrier) {
//     pthread_mutex_lock(&barrier->mutex);
//     if (barrier->threshold <= 0) {
//         pthread_mutex_unlock(&barrier->mutex);
//         return -1;
//     }
//     barrier->threshold = barrier->threshold - 1;
//     if (barrier->count >= barrier->threshold) {
//         barrier->generation++;
//         barrier->count = 0;
//         pthread_cond_broadcast(&barrier->cond);
//     }
//     pthread_mutex_unlock(&barrier->mutex);
//     return 0;
// }


int dynamic_barrier_polling_init(dynamic_barrier_polling_t *barrier, int initial_threshold) {
    barrier->threshold = initial_threshold;
    barrier->count = 0;
    barrier->generation = 0;

    if (quantum_enabled()) {
        // pthread_t tid;
        // pthread_create(&tid, NULL, report_time_peridically, barrier);
    }

    for (int i = 0; i < 128; i++) {
        barrier->histogram[i] = create_histogram(100, 1e5, 101e5);
    }
    
    return 0;
}

int dynamic_barrier_polling_destroy(dynamic_barrier_polling_t *barrier) {
    for (int i = 0; i < 128; i++) {
        free_histogram(barrier->histogram[i]);
    }
    
    return 0;
}

int dynamic_barrier_polling_wait(dynamic_barrier_polling_t *barrier) {
    uint64_t gen = atomic_load(&barrier->generation);
    uint64_t idx = atomic_fetch_add(&barrier->count, 1);

    // now, the time of the quantum is calculated.
    uint64_t difference = get_current_timestamp_ns() - thread_start_quantum_timestamp;

    
    if (idx == atomic_load(&barrier->threshold) - 1) {
        // This is the last thread entering the barrier.
        // barrier->total_diff += get_current_timestamp_ns() - barrier->last_timestamp;

        // reset the waiting count.
        atomic_store(&barrier->count, 0);

        // increase the generation.
        atomic_fetch_add(&barrier->generation, 1);
    } else {
        while (atomic_load(&barrier->generation) == gen) {
            // do nothing, because the current generation is not changed.
        }
    }

    if (thread_start_quantum_timestamp != 0) {
        // get the CPU index.
        uint64_t cpu_idx = current_cpu->cpu_index;

        // add the data point to the histogram.

        add_data_point(barrier->histogram[cpu_idx], difference);
    } 

    thread_start_quantum_timestamp = get_current_timestamp_ns();

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

        if (!quantum_enabled()) {
            assert(atomic_load(&barrier->count) == 0);
        }

        // reset the waiting count.
        atomic_store(&barrier->count, 0);

        // increase the generation.
        atomic_fetch_add(&barrier->generation, 1);

    }
    return 0;
}

// Function to create a histogram
quantum_time_histogram_t* create_histogram(int bin_count, uint64_t min, uint64_t max) {
    quantum_time_histogram_t *histogram = (quantum_time_histogram_t*)malloc(sizeof(quantum_time_histogram_t));
    histogram->bins = (uint64_t*)malloc(bin_count * sizeof(uint64_t));
    histogram->bin_count = bin_count;
    histogram->overflow_count = 0;
    histogram->underflow_count = 0;
    histogram->min = min;
    histogram->max = max;
    histogram->bin_width = (max - min) / bin_count;

    for (int i = 0; i < bin_count; i++) {
        histogram->bins[i] = 0;
    }

    return histogram;
}

// Function to add a data point to the histogram
void add_data_point(quantum_time_histogram_t *histogram, uint64_t data_point) {
    if (data_point < histogram->min) {
        histogram->underflow_count++;
    } else if (data_point >= histogram->max) {
        histogram->overflow_count++;
    } else {
        int bin_index = (data_point - histogram->min) / histogram->bin_width;
        histogram->bins[bin_index]++;
    }
}

// Function to print the histogram
void print_histogram(quantum_time_histogram_t *histogram, FILE *fp) {
    for (int i = 0; i < histogram->bin_count; i++) {
        int lower_bound = histogram->min + i * histogram->bin_width;
        int upper_bound = lower_bound + histogram->bin_width - 1;
        fprintf(fp, "Bin %d (%d - %d): %lu\n", i + 1, lower_bound, upper_bound, histogram->bins[i]);
    }
    fprintf(fp, "Underflow count: %lu\n", histogram->underflow_count);
    fprintf(fp, "Overflow count: %lu\n", histogram->overflow_count);
}


// Function to free the histogram
void free_histogram(quantum_time_histogram_t *histogram) {
    free(histogram->bins);
    free(histogram);
}
