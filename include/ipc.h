#ifndef IPC_H
#define IPC_H

#include <unistd.h>
#include <stdint.h>

#include "parser.h"  /* IP_LEN */

/* Estrutura enviada do filho para o pai via pipe */
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

ssize_t readn(int fd, void *buf, size_t count);
ssize_t writen(int fd, const void *buf, size_t count);

#endif /* IPC_H */
