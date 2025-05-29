#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>

void* compile_simulation(void* arg) {
    int duration_ms = *(int*)arg;
    struct timespec start, now;

    int busy_time = duration_ms;
    int idle_time = 100 - duration_ms;
    while (1) {
        clock_gettime(CLOCK_MONOTONIC, &start);

        // Run for 100ms
        for (int i = 0; i < busy_time; ++i) {
            // Busy work for 1ms
            struct timespec t1, t2;
            clock_gettime(CLOCK_MONOTONIC, &t1);
            do {
                volatile int x = 0;
                for (int j = 0; j < 10000; ++j) x += j;
                clock_gettime(CLOCK_MONOTONIC, &t2);
            } while (((t2.tv_sec - t1.tv_sec) * 1000 + (t2.tv_nsec - t1.tv_nsec) / 1000000) < 1);
        }

        // Sleep for the remaining ms
        if (idle_time > 0) {
            struct timespec ts;
            ts.tv_sec = 0;
            ts.tv_nsec = idle_time * 1000000;
            nanosleep(&ts, NULL);
        }
    }

    return NULL;
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <num_threads> <duration_ms>\n", argv[0]);
        return 1;
    }

    int num_threads = atoi(argv[1]);
    int duration_ms = atoi(argv[2]) % 100; // arg% ms of 100ms

    if (num_threads <= 0 || duration_ms <= 0) {
        fprintf(stderr, "Invalid arguments.\n");
        return 1;
    }

    pthread_t* threads = malloc(num_threads * sizeof(pthread_t));
    if (!threads) {
        perror("malloc");
        return 1;
    }

    for (int i = 0; i < num_threads; ++i) {
        if (pthread_create(&threads[i], NULL, compile_simulation, &duration_ms) != 0) {
            perror("pthread_create");
            free(threads);
            return 1;
        }
    }

    for (int i = 0; i < num_threads; ++i) {
        pthread_join(threads[i], NULL);
    }

    free(threads);
    printf("Stress test completed with %d threads for %d ms each.\n", num_threads, duration_ms);
    return 0;
}