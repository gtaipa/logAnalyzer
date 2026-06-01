/**
 * @file worker_threads.h
 * @brief Estrutura de argumentos e protótipo da thread worker para processamento
 *        paralelo de logs com pthreads.
 *
 * Em vez de processos filho separados, esta variante usa threads POSIX.
 * Cada thread recebe um ThreadArgs com a sua fatia de ficheiro e um ponteiro
 * para as métricas globais partilhadas, protegidas por um mutex.
 */

#ifndef WORKER_THREADS_H
#define WORKER_THREADS_H

#include <pthread.h>
#include <sys/types.h>
#include "parser.h"

/**
 * @brief Argumentos passados a cada thread worker no modelo de threads POSIX.
 *
 * Contém a fatia do ficheiro de log a processar, informação de controlo
 * e ponteiros para as estruturas partilhadas (métricas globais, mutex e
 * contadores de progresso) que são atualizadas de forma sincronizada.
 */
typedef struct {
    char  **ficheiros;          /**< @brief Lista de caminhos dos ficheiros de log a processar. */
    int     total_ficheiros;    /**< @brief Número de entradas em @p ficheiros. */
    off_t   byte_inicio;        /**< @brief Offset de byte (inclusivo) onde esta thread deve começar a ler. */
    off_t   byte_fim;           /**< @brief Offset de byte (exclusivo) onde esta thread deve parar de ler. */
    int     worker_index;       /**< @brief Índice desta thread no conjunto de workers (0-based). */
    int     verbose;            /**< @brief 1 se o modo verboso está ativo; 0 caso contrário. */

    Metrics         *global_metrics; /**< @brief Ponteiro para o acumulador de métricas global partilhado entre todas as threads. */
    pthread_mutex_t *mutex;          /**< @brief Mutex que protege o acesso concorrente a @p global_metrics. */

    long *bytes_done;  /**< @brief Ponteiro para o contador partilhado de bytes já processados por todas as threads. */
    long *bytes_total; /**< @brief Ponteiro para o total global de bytes a processar (usado no cálculo de progresso). */
} ThreadArgs;

/**
 * @brief Função executada por cada thread worker no modelo de threads POSIX.
 *
 * Processa a fatia de log definida em ThreadArgs::byte_inicio /
 * ThreadArgs::byte_fim, acumula as métricas localmente e, no final,
 * funde-as nas métricas globais sob proteção do mutex.
 * Atualiza também o contador de progresso partilhado.
 *
 * @param arg Ponteiro para um ThreadArgs com os argumentos desta thread
 *            (passado como void* de acordo com a interface pthread_create).
 * @return Sempre NULL (os resultados são escritos via ponteiros partilhados).
 */
void *run_worker_thread(void *arg);

#endif /* WORKER_THREADS_H */
