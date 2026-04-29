#ifndef WORKER_PRODCONS_H
#define WORKER_PRODCONS_H

#include <pthread.h>
#include <semaphore.h>
#include "parser.h"

#define BUFFER_SIZE 100
#define ALERT_BUFFER_SIZE 32

extern char *ficheiros[];
extern int total_ficheiros;
extern int num_produtores;
extern int verbose;

extern LogEntry buffer[BUFFER_SIZE];
extern int prodptr;
extern int consptr;
extern int buffer_count;
extern int produtores_ativos;

extern Metrics global_metrics;
extern pthread_mutex_t trinco;
extern pthread_mutex_t metrics_mutex;
extern sem_t vagas;
extern sem_t itens;

extern LogEntry alert_buffer[ALERT_BUFFER_SIZE];
extern int alert_prodptr;
extern int alert_consptr;
extern int alert_count;
extern pthread_mutex_t alert_trinco;
extern sem_t alert_vagas;
extern sem_t alert_itens;
extern volatile int consumidores_ativos;

void init_bounded_buffer(void);
void destroy_bounded_buffer(void);
void init_alert_buffer(void);
void destroy_alert_buffer(void);
void send_alert(LogEntry *entry);

void *run_producer(void *arg);
void *run_consumer(void *arg);
void *run_alert_monitor(void *arg);

#endif /* WORKER_PRODCONS_H */
