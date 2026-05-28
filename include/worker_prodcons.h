#ifndef WORKER_PRODCONS_H
#define WORKER_PRODCONS_H

#include <pthread.h>
#include <semaphore.h>
#include "parser.h"

#define BUFFER_SIZE      10
#define ALERT_BUFFER_SIZE 64

/* ---- Buffer circular ---- */
typedef struct {
    LogEntry        queue[BUFFER_SIZE];
    int             count;
    int             in;
    int             out;
    pthread_mutex_t mutex;
    sem_t           empty;
    sem_t           full;
} LogQueue;

/* ---- Argumentos do produtor ---- */
typedef struct {
    char **ficheiros;
    int    inicio;
    int    fim;
    int    worker_index;
    int    verbose;
    long  *lines_done;   /* para o dashboard */
    long  *lines_total;  /* para o dashboard */
} ProducerArgs;

/* ---- Argumentos do consumidor ---- */
typedef struct {
    Metrics         *global_metrics;
    pthread_mutex_t *metrics_mutex;
    int              worker_index;
} ConsumerArgs;

/* ---- Variáveis globais (definidas em worker_prodcons.c) ---- */
extern LogQueue        buffer;
extern int             produtores_ativos;
extern pthread_mutex_t metrics_mutex;

/**
 * @brief Inicializa o buffer circular, mutex e semáforos.
 */
void  init_bounded_buffer(void);

/**
 * @brief Destrói o mutex e os semáforos do buffer circular.
 */
void  destroy_bounded_buffer(void);

/**
 * @brief Função executada por cada thread produtora.
 * @param arg Apontador para ProducerArgs com o subconjunto de ficheiros.
 * @return NULL.
 *
 * Lê os ficheiros linha a linha, parseia cada linha e insere LogEntry
 * válidas no buffer circular. Ao terminar decrementa produtores_ativos
 * e acorda os consumidores bloqueados.
 */
void *run_producer(void *arg);

/**
 * @brief Função executada por cada thread consumidora.
 * @param arg Apontador para ConsumerArgs com as métricas globais e o mutex.
 * @return NULL.
 *
 * Retira LogEntry do buffer circular e chama update_metrics() enquanto
 * houver produtores activos ou entradas por processar.
 */
void *run_consumer(void *arg);

#endif /* WORKER_PRODCONS_H */