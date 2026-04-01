#ifndef WORKER_H
#define WORKER_H

/* =========================================================
 * worker.h  –  Protótipo da função do processo filho
 * ========================================================= */

/**
 * Função principal do processo filho.
 *
 * @param ficheiros     lista completa de ficheiros
 * @param inicio        índice do primeiro ficheiro a processar
 * @param fim           índice após o último ficheiro a processar
 * @param pipe_fd       fd de escrita do pipe de resultados finais
 * @param progress_fd   fd de escrita do pipe de progresso
 * @param worker_index  índice do worker (0, 1, 2, ...)
 * @param verbose       1 se --verbose foi passado, 0 caso contrário
 */
void run_worker(char **ficheiros, int inicio, int fim,
                int pipe_fd, int progress_fd, int worker_index, int verbose);

#endif /* WORKER_H */