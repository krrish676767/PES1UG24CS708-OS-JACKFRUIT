/* workload_io.c — I/O-bound workload for Task 5 (scheduler experiments)
 *
 * Repeatedly writes and reads a 4 KB block to a temp file with fsync,
 * forcing the process to block on I/O and yield the CPU frequently.
 * Usage:  ./workload_io [rounds]
 *
 * Scheduler experiment idea:
 *   Run workload_cpu and workload_io simultaneously and compare how much
 *   CPU time each gets — the I/O-bound process voluntarily yields the CPU,
 *   so the CPU-bound process should get more time when they compete.
 *   Observe with:  top -p <pid1>,<pid2>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

int main(int argc, char *argv[])
{
    int rounds = (argc > 1) ? atoi(argv[1]) : 2000;

    char tmpfile[] = "/tmp/jackfruit_io_XXXXXX";
    int fd = mkstemp(tmpfile);
    if (fd < 0) { perror("mkstemp"); return 1; }

    printf("[io_workload] starting: %d rounds on %s\n", rounds, tmpfile);
    fflush(stdout);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    char buf[4096];
    memset(buf, 'A', sizeof(buf));

    for (int i = 0; i < rounds; i++) {
        if (write(fd, buf, sizeof(buf)) < 0) { perror("write"); break; }
        if (lseek(fd, 0, SEEK_SET)  < 0) { perror("lseek"); break; }
        if (read(fd,  buf, sizeof(buf)) < 0) { perror("read"); break; }
        fsync(fd);   /* force flush to disk — ensures we actually block */
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double elapsed = (t1.tv_sec  - t0.tv_sec) +
                     (t1.tv_nsec - t0.tv_nsec) / 1e9;

    close(fd);
    unlink(tmpfile);

    printf("[io_workload] done: rounds=%d  time=%.3fs\n", rounds, elapsed);
    return 0;
}
