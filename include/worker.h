#ifndef WORKER_H
#define WORKER_H

/* =========================================================
 * worker.h  –  Protótipo da função do processo filho
 * ========================================================= */

/**
 * Função principal do processo filho.
 *
 * @param ficheiros        lista completa de ficheiros
 * @param total_ficheiros  número total de ficheiros
 * @param num_processos    número total de processos (workers)
 * @param worker_index     índice deste worker (0, 1, 2, ...)
 * @param verbose          1 se --verbose foi passado, 0 caso contrário
 *
 * O filho liga-se ao pai via Unix Domain Socket (SOCKET_PATH),
 * recebe a configuração (intervalo de linhas) e processa sua quota.
 * Envia ProgressUpdates + WorkerResult pelo mesmo socket.
 */
void run_worker(char **ficheiros, int total_ficheiros, int num_processos,
                int worker_index, int verbose);

#endif /* WORKER_H */