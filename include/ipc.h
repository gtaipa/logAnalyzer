#ifndef IPC_H
#define IPC_H

#include <unistd.h>
#include <stdint.h>

#include "parser.h"  /* IP_LEN */

/* =========================================================
 * Path do socket Unix Domain
 * ========================================================= */
#define SOCKET_PATH "/tmp/loganalyzer.sock"

/* =========================================================
 * Estrutura enviada do filho para o pai — resultados finais
 * ========================================================= */
typedef struct {
    pid_t pid;
    long total_lines;
    long count_debug;
    long count_info;
    long count_warn;
    long count_error;
    long count_critical;
    long count_4xx;
    long count_5xx;
    char top_ip[IP_LEN];
} WorkerResult;

/* =========================================================
 * Estrutura enviada do filho para o pai — progresso
 * ========================================================= */
typedef struct {
    pid_t pid;
    int   worker_index;
    long  lines_done;
    long  lines_total;
} ProgressUpdate;

/* =========================================================

 * Funções de Unix Domain Sockets
 * ========================================================= */

/**
 * Cria o socket servidor, faz bind() e listen().
 * Retorna o fd do socket servidor, ou -1 em erro.
 */
int create_server_socket(void);

/**
 * Aceita uma ligação de um cliente.
 * Retorna o fd do cliente aceite, ou -1 em erro.
 */
int accept_client(int server_fd);

/**
 * Liga ao socket servidor (usado pelos filhos).
 * Retorna o fd do socket ligado, ou -1 em erro.
 */
int connect_to_server(void);

#endif /* IPC_H */