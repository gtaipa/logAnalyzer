#ifndef WORKER_PRODCONS_H
#define WORKER_PRODCONS_H

#include <pthread.h>
#include <semaphore.h>
#include "parser.h"

/* ALTERAÇÃO: Removidas declarações de funções que não existem:
 *   - prodcons_configure()       → nunca foi implementada
 *   - init_alert_buffer()        → nunca foi implementada
 *   - destroy_alert_buffer()     → nunca foi implementada
 *   - prodcons_stop_alert_monitor() → nunca foi implementada
 *   - run_alert_monitor()        → nunca foi implementada
 * Manter declarações de funções que não existem causa erros de linker
 * e confunde quem lê o código na defesa. */

#define BUFFER_SIZE      10

/* ---- Estrutura do buffer circular ---- */
typedef struct {
    LogEntry        queue[BUFFER_SIZE];  /* slots do buffer */
    int             count;               /* quantos slots estão ocupados */
    int             in;                  /* próximo slot a escrever (produtor) */
    int             out;                 /* próximo slot a ler (consumidor) */
    pthread_mutex_t mutex;               /* protege in, out, count */
    sem_t           empty;              /* conta slots VAZIOS (começa em BUFFER_SIZE) */
    sem_t           full;               /* conta slots CHEIOS (começa em 0) */
} LogQueue;

/* ---- Argumentos passados a cada thread produtora ---- */
/* ALTERAÇÃO: Struct nova. O main precisa de passar informação a cada produtor:
 * que ficheiros lhe cabem, e onde escrever as métricas no final. */
typedef struct {
    char          **ficheiros;        /* array completo de ficheiros */
    int             inicio;           /* índice do primeiro ficheiro deste produtor */
    int             fim;              /* índice do último + 1 (exclusivo) */
    int             worker_index;     /* ID para mensagens de debug */
    int             verbose;
} ProducerArgs;

/* ---- Argumentos passados a cada thread consumidora ---- */
/* ALTERAÇÃO: Struct nova. O consumidor precisa de saber onde acumular métricas
 * e qual o mutex que protege essa zona de memória partilhada. */
typedef struct {
    Metrics        *global_metrics;   /* métricas globais partilhadas por todos os consumidores */
    pthread_mutex_t *metrics_mutex;   /* mutex que protege global_metrics */
    int             worker_index;     /* ID para mensagens de debug */
} ConsumerArgs;

/* ---- Buffer principal partilhado (definido em worker_prodcons.c) ---- */
extern LogQueue buffer;

/* ---- Contador de produtores ainda activos (definido em worker_prodcons.c) ----
 * ALTERAÇÃO: Este valor começa em N (número de produtores) e cada produtor
 * decrementa-o quando termina. Quando chega a 0, os consumidores sabem
 * que não vão chegar mais entradas. Protegido pelo mutex do buffer. */
extern int             produtores_ativos;
extern pthread_mutex_t metrics_mutex;

/* ---- Funções de inicialização e destruição ---- */
void init_bounded_buffer(void);
void destroy_bounded_buffer(void);

/* ---- Funções das threads ---- */
void *run_producer(void *arg);   /* lê ficheiros e insere LogEntries no buffer */
void *run_consumer(void *arg);   /* retira LogEntries do buffer e acumula métricas */

#endif /* WORKER_PRODCONS_H */