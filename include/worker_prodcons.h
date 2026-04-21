#ifndef WORKER_PRODCONS_H
#define WORKER_PRODCONS_H

#include <pthread.h>
#include "parser.h"

/* Tamanho do buffer circular (conforme o resumo do colega) */
#define BUFFER_SIZE 100

/* =========================================================
 * 📦 O Armazém (Bounded Buffer) - A passadeira rolante
 * ========================================================= */
typedef struct {
    LogEntry buffer[BUFFER_SIZE];    // Array circular para as linhas de log
    int in;                          // Índice para inserção (Produtor)
    int out;                         // Índice para remoção (Consumidor)
    int count;                       // Quantidade de items no buffer neste momento

    pthread_mutex_t mutex;           // Cadeado exclusivo
    pthread_cond_t cond_not_full;    // Sinal: buffer não está cheio
    pthread_cond_t cond_not_empty;   // Sinal: buffer não está vazio
} BoundedBuffer;

/* =========================================================
 * Estrutura para passar argumentos para as threads
 * ========================================================= */
typedef struct {
    char **ficheiros;                // Lista de todos os ficheiros
    int inicio;                      // Índice onde este worker começa
    int fim;                         // Índice onde este worker acaba
    int worker_index;                // ID do worker (1, 2, 3...)
    int verbose;                     // Modo verbose (0 ou 1)
    
    BoundedBuffer *buffer;           // Apontador para o buffer partilhado
    Metrics *global_metrics;         // Apontador para a variável global de resultados (para o Consumidor)
    pthread_mutex_t *metrics_mutex;  // Apontador para o cadeado das métricas globais
} ProducerConsumerArgs;

/* =========================================================
 * Variáveis Globais Partilhadas
 * ========================================================= */
/* Alarme de fim de turno (declarado no main_prodcons.c) */
extern int produtores_ativos;

/* =========================================================
 * Protótipos das funções
 * ========================================================= */
void init_bounded_buffer(BoundedBuffer *buffer);
void destroy_bounded_buffer(BoundedBuffer *buffer);

void *run_producer(void *arg);
void *run_consumer(void *arg);

#endif /* WORKER_PRODCONS_H */