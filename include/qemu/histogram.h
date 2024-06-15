#ifndef QEMU_HISTOGRAM_H
#define QEMU_HISTOGRAM_H

// We also define a bin and a histogram for the profiling of the quantum time. This should be helpful for debugging the performance problem.

#include <stdint.h>
#include <stdio.h>

typedef struct {
    uint64_t *bins;
    int bin_count;
    uint64_t overflow_count;
    uint64_t underflow_count;
    uint64_t min;
    uint64_t max;
    uint64_t bin_width;
} time_histogram_t;


time_histogram_t* create_histogram(int bin_count, uint64_t min, uint64_t max);
void add_data_point(time_histogram_t *histogram, uint64_t data_point);
void print_histogram(time_histogram_t *histogram, FILE *fp);
void free_histogram(time_histogram_t *histogram);


#endif