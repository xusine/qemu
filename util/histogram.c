// TODO: move the definition of the histogram struct to this file, from the quantum barrier.

#include "qemu/histogram.h"
#include <stdlib.h>

// Function to create a histogram
time_histogram_t* create_histogram(int bin_count, uint64_t min, uint64_t max) {
    time_histogram_t *histogram = (time_histogram_t*)malloc(sizeof(time_histogram_t));
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
void add_data_point(time_histogram_t *histogram, uint64_t data_point) {
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
void print_histogram(time_histogram_t *histogram, FILE *fp) {
    for (int i = 0; i < histogram->bin_count; i++) {
        int lower_bound = histogram->min + i * histogram->bin_width;
        int upper_bound = lower_bound + histogram->bin_width - 1;
        fprintf(fp, "Bin %d (%d - %d): %lu\n", i + 1, lower_bound, upper_bound, histogram->bins[i]);
    }
    fprintf(fp, "Underflow count: %lu\n", histogram->underflow_count);
    fprintf(fp, "Overflow count: %lu\n", histogram->overflow_count);
}


// Function to free the histogram
void free_histogram(time_histogram_t *histogram) {
    free(histogram->bins);
    free(histogram);
}
