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
 * Tipos de mensagens (protocol)
 * ========================================================= */
#define MSG_CONFIG     0  /* Pai envia configuração ao worker */
#define MSG_PROGRESSO  1  /* Worker envia progresso ao pai */
#define MSG_RESULTADO  2  /* Worker envia resultado final ao pai */

/* =========================================================
 * Estrutura enviada do pai para o filho — configuração
 * ========================================================= */
typedef struct {
    long linha_inicio;
    long linha_fim;
    long total_linhas_globais;
    int worker_index;
} WorkerConfig;

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
    char top_ips[10][IP_LEN];
    long top_ips_counts[10];
    char alerts[MAX_ALERTS][ALERT_LEN];
    int num_alerts;
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
 * Liga ao socket servidor (usado pelos filhos).
 * Retorna o fd do socket ligado, ou -1 em erro.
 */
int connect_to_server(void);

/* =========================================================
 * Funções auxiliares seguras para Pipes/Sockets
 *
 * readn() tenta ler exatamente nbytes, repetindo read()
 * quando ha leituras parciais ou interrupcao por sinal.
 *
 * writen() tenta escrever exatamente nbytes, repetindo write()
 * quando ha escritas parciais ou interrupcao por sinal.
 * ========================================================= */
ssize_t readn(int fd, void *ptr, size_t nbytes);
ssize_t writen(int fd, void *ptr, size_t nbytes);
void run_worker_pipe(char **ficheiros, int total_ficheiros, int pipe_fd_write, 
                     int worker_index, long linha_inicio, long linha_fim, int verbose);

#endif /* IPC_H */
