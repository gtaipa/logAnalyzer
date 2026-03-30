#include "worker.h"
#include "parser.h"

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

/* =========================================================
 * write_results
 *
 * Escreve as métricas acumuladas num ficheiro
 * results_<pid>.txt usando write() (sem fprintf).
 * ========================================================= */
static void write_results(const Metrics *m, int verbose) {

    char path[64];
    snprintf(path, sizeof(path), "results_%d.txt", getpid());

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        perror("open results");
        return;
    }

    /* Encontrar o IP mais frequente (top 1) */
    char top_ip[IP_LEN] = "-";
    long top_count = 0;
    for (int i = 0; i < m->ip_num; i++) {
        if (m->ip_count[i] > top_count) {
            top_count = m->ip_count[i];
            strncpy(top_ip, m->ip_list[i], IP_LEN - 1);
            top_ip[IP_LEN - 1] = '\0';
        }
    }

    /* Formato definido no enunciado:
     * PID:1234;LINHAS:50000;ERRORS:234;WARNINGS:1205;TOP_IP:192.168.1.100 */
    char msg[512];
    int len = snprintf(msg, sizeof(msg),
        "PID:%d;LINHAS:%ld;DEBUG:%ld;INFO:%ld;WARNINGS:%ld;"
        "ERRORS:%ld;CRITICAL:%ld;4xx:%ld;5xx:%ld;TOP_IP:%s\n",
        getpid(),
        m->total_lines,
        m->count_debug,
        m->count_info,
        m->count_warn,
        m->count_error,
        m->count_critical,
        m->count_4xx,
        m->count_5xx,
        top_ip
    );

    write(fd, msg, len);
    close(fd);

    if (verbose) {
        printf("[Filho %d] Resultados escritos em %s\n", getpid(), path);
    }
}

/* =========================================================
 * run_worker  –  função principal do processo filho
 *
 * Chamada pelo pai após o fork(). Processa todos os
 * ficheiros da sua fatia e escreve os resultados.
 * ========================================================= */
void run_worker(char **ficheiros, int inicio, int fim, int verbose) {

    Metrics m;
    init_metrics(&m);

    printf("[Filho %d] Vou processar %d ficheiro(s)\n", getpid(), fim - inicio);

    for (int i = inicio; i < fim; i++) {
        process_file(ficheiros[i], &m, verbose);
    }

    write_results(&m, verbose);

    printf("[Filho %d] Terminei. Total de linhas: %ld | Erros: %ld\n",
           getpid(), m.total_lines, m.count_error);
}