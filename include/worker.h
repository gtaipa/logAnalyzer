/**
 * @file worker.h
 * @brief Protótipo da função principal do processo filho (worker) que comunica
 *        via Unix Domain Socket.
 *
 * Cada processo filho criado pelo pai invoca run_worker(), que estabelece
 * ligação ao servidor (processo pai), recebe a sua WorkerConfig, processa
 * a fatia de log que lhe foi atribuída e devolve ProgressUpdates periódicos
 * seguidos de um WorkerResult final, tudo pelo mesmo socket.
 */

#ifndef WORKER_H
#define WORKER_H

/* =========================================================
 * worker.h  –  Protótipo da função do processo filho
 * ========================================================= */

/**
 * @brief Função principal do processo filho que usa Unix Domain Socket.
 *
 * O filho liga-se ao pai via Unix Domain Socket (SOCKET_PATH),
 * recebe a configuração (intervalo de bytes a processar) e processa a sua
 * quota do(s) ficheiro(s) de log. Durante o processamento envia ProgressUpdates
 * periódicos ao pai e, no final, envia um WorkerResult com as métricas
 * acumuladas. A função termina (e o processo filho deve chamar exit()) após
 * o envio do resultado.
 *
 * @param ficheiros        Lista completa de caminhos dos ficheiros de log.
 * @param total_ficheiros  Número de entradas em @p ficheiros.
 * @param num_processos    Número total de processos worker criados pelo pai.
 * @param worker_index     Índice deste worker no conjunto de processos filhos (0-based).
 * @param verbose          1 se o modo verboso (--verbose) está ativo; 0 caso contrário.
 */
void run_worker(char **ficheiros, int total_ficheiros, int num_processos,
                int worker_index, int verbose);

#endif /* WORKER_H */
