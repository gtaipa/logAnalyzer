#ifndef IPC_H
#define IPC_H

#include <unistd.h>
#include <stdint.h>

#include "parser.h"  /* IP_LEN */

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
 * Enviada periodicamente via pipe de progresso
 * ========================================================= */
typedef struct {
    pid_t pid;
    int   worker_index;   /* índice do worker (0, 1, 2, ...) */
    long  lines_done;     /* linhas processadas até agora     */
    long  lines_total;    /* total estimado de linhas         */
} ProgressUpdate;

/* =========================================================
 * Funções auxiliares de I/O
 * ========================================================= */
ssize_t readn(int fd, void *buf, size_t count);
ssize_t writen(int fd, const void *buf, size_t count);

#endif /* IPC_H */