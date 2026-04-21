#ifndef WORKER_THREADS_H
#define WORKER_THREADS_H

#include <pthread.h>
#include "parser.h"

/* Estrutura para passar argumentos para cada thread */
typedef struct {
    char **ficheiros;         // Lista de todos os ficheiros
    int inicio;               // Índice onde esta thread começa
    int fim;                  // Índice onde esta thread acaba
    int worker_index;         // ID do worker (0, 1, 2...)
    int verbose;              // Modo verbose (0 ou 1)
    
    Metrics *global_metrics;  // Apontador para a variável global de resultados
    pthread_mutex_t *mutex;   // Apontador para o cadeado (mutex)
    
    // NOVOS CAMPOS PARA O DASHBOARD:
    long *lines_done;         // Apontador para partilhar linhas lidas
    long *lines_total;        // Apontador para partilhar o total de linhas a ler
} ThreadArgs;

/* Protótipo da função que a thread vai executar */
void *run_worker_thread(void *arg);

#endif /* WORKER_THREADS_H */