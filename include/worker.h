#ifndef WORKER_H
#define WORKER_H

/* =========================================================
 * worker.h  –  Protótipo da função do processo filho
 * ========================================================= */

/**
 * @brief Função principal do processo filho (variante sockets).
 * @param ficheiros        Lista completa de ficheiros a processar.
 * @param total_ficheiros  Número total de ficheiros.
 * @param num_processos    Número total de workers lançados pelo pai.
 * @param worker_index     Índice deste worker (usado internamente; o índice real vem via MSG_CONFIG).
 * @param verbose          1 se --verbose foi passado, 0 caso contrário.
 *
 * O filho liga-se ao servidor (pai) via Unix Domain Socket, recebe a
 * configuração (byte_inicio, byte_fim), processa a sua fatia e envia
 * o WorkerResult final pelo socket.
 */
void run_worker(char **ficheiros, int total_ficheiros, int num_processos,
                int worker_index, int verbose);

#endif /* WORKER_H */