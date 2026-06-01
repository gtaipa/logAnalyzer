#ifndef WORKER_PRODCONS_H
#define WORKER_PRODCONS_H

#include <pthread.h>
#include <semaphore.h>
#include <sys/types.h>
#include "parser.h"

#define BUFFER_SIZE      30
#define ALERT_BUFFER_SIZE 64

typedef struct {
    LogEntry        queue[BUFFER_SIZE];
    int             count;
    int             in;
    int             out;
    pthread_mutex_t mutex;
    sem_t           empty;
    sem_t           full;
} LogQueue;

typedef struct {
    char  **ficheiros;
    int     total_ficheiros;
    off_t   byte_inicio;
    off_t   byte_fim;
    int     worker_index;
    int     verbose;
    long   *bytes_done;
    long   *bytes_total;
} ProducerArgs;

typedef struct {
    Metrics         *global_metrics;
    pthread_mutex_t *metrics_mutex;
    int              worker_index;
} ConsumerArgs;

extern LogQueue        buffer;
extern int             produtores_ativos;
extern pthread_mutex_t metrics_mutex;

void  init_bounded_buffer(void);
void  destroy_bounded_buffer(void);
void *run_producer(void *arg);
void *run_consumer(void *arg);

#endif /* WORKER_PRODCONS_H */