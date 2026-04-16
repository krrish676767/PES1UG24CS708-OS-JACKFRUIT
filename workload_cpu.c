/* workload_cpu.c — CPU-bound workload for Task 5 (scheduler experiments)
 *
 * Runs a tight arithmetic loop and prints elapsed time.
 * Usage:  ./workload_cpu [iterations]
 *
 * Scheduler experiment idea:
 *   Terminal 1:  nice -n 0  ./workload_cpu 500000000
 *   Terminal 2:  nice -n 19 ./workload_cpu 500000000
 * Observe difference in wall-clock completion times.
 */

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

int main(int argc, char *argv[])
{
    long iterations = (argc > 1) ? atol(argv[1]) : 200000000L;

    printf("[cpu_workload] starting: %ld iterations\n", iterations);
    fflush(stdout);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    /* Tight loop — keeps CPU fully busy */
    volatile long sum = 0;
    for (long i = 0; i < iterations; i++)
        sum += i * i % 1000003;   /* modulo avoids overflow; keeps it busy */

    clock_gettime(CLOCK_MONOTONIC, &t1);

    double elapsed = (t1.tv_sec  - t0.tv_sec) +
                     (t1.tv_nsec - t0.tv_nsec) / 1e9;

    printf("[cpu_workload] done: sum=%ld  time=%.3fs\n", sum, elapsed);
    return 0;
}
