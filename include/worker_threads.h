#ifndef WORKER_THREADS_H
#define WORKER_THREADS_H

#include <pthread.h>
#include <sys/types.h>
#include "parser.h"

typedef struct {
    char  **ficheiros;
    int     total_ficheiros;
    off_t   byte_inicio;
    off_t   byte_fim;
    int     worker_index;
    int     verbose;

    Metrics         *global_metrics;
    pthread_mutex_t *mutex;

    long *bytes_done;
    long *bytes_total;
} ThreadArgs;

void *run_worker_thread(void *arg);

#endif /* WORKER_THREADS_H */