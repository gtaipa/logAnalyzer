#include "worker.h"
#include "parser.h"
#include "ipc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

/* =========================================================
 * Constantes internas
 * ========================================================= */
#define BUF_SIZE    4096   /* tamanho do buffer de leitura  */
#define LINE_MAX     512   /* tamanho máximo de uma linha   */

/* =========================================================
 * process_file
 *
 * Abre um ficheiro .log com open(), lê-o com read() em
 * blocos, parte o conteúdo em linhas e parseia cada uma.
 * Acumula os resultados em *m.
 * ========================================================= */
static void process_file(const char *path, Metrics *m, int verbose) {

    /* Abrir o ficheiro apenas para leitura */
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        perror("open");
        return;   /* continua para o próximo ficheiro */
    }

    if (verbose) {
        printf("[Filho %d] A abrir: %s\n", getpid(), path);
    }

    /* Buffer de leitura e linha acumulada */
    char buf[BUF_SIZE];
    char line[LINE_MAX];
    int  line_len = 0;

    /* Formato do ficheiro — detetado na primeira linha válida */
    LogFormat fmt = FORMAT_UNKNOWN;

    ssize_t bytes_read;

    /* Ler o ficheiro em blocos até EOF (read() devolve 0) */
    while ((bytes_read = read(fd, buf, BUF_SIZE)) > 0) {

        for (ssize_t b = 0; b < bytes_read; b++) {

            char c = buf[b];

            if (c == '\n' || c == '\r') {
                /* Fim de linha — processar o que acumulámos */
                if (line_len == 0) continue;   /* linha vazia */
                line[line_len] = '\0';

                /* Detetar formato na primeira linha válida do ficheiro */
                if (fmt == FORMAT_UNKNOWN) {
                    fmt = detect_format(line);
                }

                /* Parsear a linha e atualizar métricas */
                LogEntry entry;
                if (parse_line(line, fmt, &entry) == 0) {
                    update_metrics(m, &entry);
                }

                /* Reiniciar a linha */
                line_len = 0;

            } else {
                /* Acumular o carácter na linha atual */
                if (line_len < LINE_MAX - 1) {
                    line[line_len++] = c;
                }
                /* Se a linha for demasiado longa, ignoramos o excesso */
            }
        }
    }

    /* Processar última linha se o ficheiro não terminar com '\n' */
    if (line_len > 0) {
        line[line_len] = '\0';
        if (fmt == FORMAT_UNKNOWN) fmt = detect_format(line);
        LogEntry entry;
        if (parse_line(line, fmt, &entry) == 0) {
            update_metrics(m, &entry);
        }
    }

    if (bytes_read < 0) {
        perror("read");
    }

    close(fd);
}

static void get_top_ip(const Metrics *m, char top_ip[IP_LEN]) {
    strncpy(top_ip, "-", IP_LEN - 1);
    top_ip[IP_LEN - 1] = '\0';
    long top_count = 0;
    for (int i = 0; i < m->ip_num; i++) {
        if (m->ip_count[i] > top_count) {
            top_count = m->ip_count[i];
            strncpy(top_ip, m->ip_list[i], IP_LEN - 1);
            top_ip[IP_LEN - 1] = '\0';
        }
    }
}

static void send_results(int pipe_fd, const Metrics *m, int verbose) {
    WorkerResult r;
    r.pid = getpid();
    r.total_lines = m->total_lines;
    r.count_debug = m->count_debug;
    r.count_info = m->count_info;
    r.count_warn = m->count_warn;
    r.count_error = m->count_error;
    r.count_critical = m->count_critical;
    r.count_4xx = m->count_4xx;
    r.count_5xx = m->count_5xx;
    get_top_ip(m, r.top_ip);

    if (writen(pipe_fd, &r, sizeof(r)) != sizeof(r)) {
        perror("writen");
        if (verbose) {
            printf("[Filho %d] Erro a enviar resultados pelo pipe\n", getpid());
        }
        return;
    }

    if (verbose) {
        printf("[Filho %d] Resultados enviados ao pai via pipe\n", getpid());
    }
}

/* =========================================================
 * run_worker  –  função principal do processo filho
 *
 * Chamada pelo pai após o fork(). Processa todos os
 * ficheiros da sua fatia e envia os resultados ao pai.
 * ========================================================= */
void run_worker(char **ficheiros, int inicio, int fim, int pipe_fd, int verbose) {

    Metrics m;
    init_metrics(&m);

    printf("[Filho %d] Vou processar %d ficheiro(s)\n", getpid(), fim - inicio);

    for (int i = inicio; i < fim; i++) {
        process_file(ficheiros[i], &m, verbose);
    }

    send_results(pipe_fd, &m, verbose);

    printf("[Filho %d] Terminei. Total de linhas: %ld | Erros: %ld\n",
           getpid(), m.total_lines, m.count_error);
}