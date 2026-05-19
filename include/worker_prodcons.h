#ifndef WORKER_PRODCONS_H
#define WORKER_PRODCONS_H

#include <pthread.h>
#include <semaphore.h>
#include "parser.h"

#define BUFFER_SIZE      10
#define ALERT_BUFFER_SIZE 64

/* ---- Estrutura do buffer circular ---- */
typedef struct {
    LogEntry        queue[BUFFER_SIZE];
    int             count;
    int             in;
    int             out;
    pthread_mutex_t mutex;
    sem_t           empty;
    sem_t           full;
} LogQueue;

/* ---- Buffer principal partilhado ---- */
extern LogQueue buffer;

/* ---- Funções de configuração ---- */
void prodcons_configure(char **ficheiros,
                        int total_ficheiros,
                        int num_produtores,
                        int num_consumidores,
                        int verbose,
                        Metrics *global_metrics,
                        pthread_mutex_t *metrics_mutex);

/* ---- Funções de inicialização e destruição ---- */
void init_bounded_buffer(void);
void destroy_bounded_buffer(void);
void init_alert_buffer(void);
void destroy_alert_buffer(void);
void prodcons_stop_alert_monitor(void);

/* ---- Funções das threads ---- */
void *run_producer(void *arg);
void *run_consumer(void *arg);
void *run_alert_monitor(void *arg);

extern int             produtores_ativos;
extern pthread_mutex_t metrics_mutex;

#endif /* WORKER_PRODCONS_H */

