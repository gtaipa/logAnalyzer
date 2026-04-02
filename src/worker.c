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
#define BUF_SIZE         4096
#define LINE_MAX          512
#define PROGRESS_INTERVAL 100

/* Tipos de mensagem — têm de coincidir com o main.c */
#define MSG_PROGRESS 1
#define MSG_RESULT   2

typedef struct { int type; } MsgHeader;

/* =========================================================
 * count_lines
 * ========================================================= */
static long count_lines(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    char buf[BUF_SIZE];
    long count = 0;
    ssize_t n;
    while ((n = read(fd, buf, BUF_SIZE)) > 0)
        for (ssize_t i = 0; i < n; i++)
            if (buf[i] == '\n') count++;
    close(fd);
    return count;
}

/* =========================================================
 * send_progress — envia ProgressUpdate ao pai
 * ========================================================= */
static void send_progress(int sock_fd, int worker_index,
                           long lines_done, long lines_total) {
    MsgHeader hdr = { MSG_PROGRESS };
    writen(sock_fd, &hdr, sizeof(hdr));

    ProgressUpdate pu;
    pu.pid          = getpid();
    pu.worker_index = worker_index;
    pu.lines_done   = lines_done;
    pu.lines_total  = lines_total;
    writen(sock_fd, &pu, sizeof(pu));
}

/* =========================================================
 * process_file
 * ========================================================= */
static void process_file(const char *path, Metrics *m, int verbose,
                         int sock_fd, int worker_index,
                         long *lines_done, long lines_total) {

    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror("open"); return; }

    if (verbose)
        printf("[Filho %d] A abrir: %s\n", getpid(), path);

    char buf[BUF_SIZE];
    char line[LINE_MAX];
    int  line_len = 0;
    LogFormat fmt = FORMAT_UNKNOWN;
    ssize_t bytes_read;

    while ((bytes_read = read(fd, buf, BUF_SIZE)) > 0) {
        for (ssize_t b = 0; b < bytes_read; b++) {
            char c = buf[b];

            if (c == '\n' || c == '\r') {
                if (line_len == 0) continue;
                line[line_len] = '\0';

                if (fmt == FORMAT_UNKNOWN)
                    fmt = detect_format(line);

                LogEntry entry;
                if (parse_line(line, fmt, &entry) == 0)
                    update_metrics(m, &entry);

                line_len = 0;
                (*lines_done)++;

                if (sock_fd >= 0 && (*lines_done % PROGRESS_INTERVAL) == 0)
                    send_progress(sock_fd, worker_index, *lines_done, lines_total);

            } else {
                if (line_len < LINE_MAX - 1)
                    line[line_len++] = c;
            }
        }
    }

    if (line_len > 0) {
        line[line_len] = '\0';
        if (fmt == FORMAT_UNKNOWN) fmt = detect_format(line);
        LogEntry entry;
        if (parse_line(line, fmt, &entry) == 0)
            update_metrics(m, &entry);
        (*lines_done)++;
    }

    if (bytes_read < 0) perror("read");
    close(fd);
}

/* =========================================================
 * get_top_ip
 * ========================================================= */
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

/* =========================================================
 * run_worker
 * ========================================================= */
void run_worker(char **ficheiros, int inicio, int fim,
                int worker_index, int verbose) {

    Metrics m;
    init_metrics(&m);

    printf("[Filho %d] Vou processar %d ficheiro(s)\n", getpid(), fim - inicio);

    /* Ligar ao servidor (pai) */
    int sock_fd = connect_to_server();
    if (sock_fd < 0) {
        fprintf(stderr, "[Filho %d] Erro ao ligar ao servidor\n", getpid());
        exit(1);
    }

    /* Estimar total de linhas */
    long lines_total = 0;
    for (int i = inicio; i < fim; i++)
        lines_total += count_lines(ficheiros[i]);

    long lines_done = 0;

    /* Processar ficheiros */
    for (int i = inicio; i < fim; i++)
        process_file(ficheiros[i], &m, verbose,
                     sock_fd, worker_index, &lines_done, lines_total);

    /* Enviar update final de progresso (100%) */
    send_progress(sock_fd, worker_index, lines_total, lines_total);

    /* Enviar resultado final */
    MsgHeader hdr = { MSG_RESULT };
    writen(sock_fd, &hdr, sizeof(hdr));

    WorkerResult r;
    r.pid            = getpid();
    r.total_lines    = m.total_lines;
    r.count_debug    = m.count_debug;
    r.count_info     = m.count_info;
    r.count_warn     = m.count_warn;
    r.count_error    = m.count_error;
    r.count_critical = m.count_critical;
    r.count_4xx      = m.count_4xx;
    r.count_5xx      = m.count_5xx;
    get_top_ip(&m, r.top_ip);
    writen(sock_fd, &r, sizeof(r));

    if (verbose)
        printf("[Filho %d] Resultados enviados ao pai via socket\n", getpid());

    close(sock_fd);

    printf("[Filho %d] Terminei. Total de linhas: %ld | Erros: %ld\n",
           getpid(), m.total_lines, m.count_error);
}