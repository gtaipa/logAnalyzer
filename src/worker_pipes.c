#include "ipc.h"
#include "parser.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BUF_SIZE 4096
#define LINE_MAX 512

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

static void process_file(const char *path, Metrics *m) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        perror("open");
        return;
    }

    char buf[BUF_SIZE];
    char line[LINE_MAX];
    int line_len = 0;
    LogFormat fmt = FORMAT_UNKNOWN;
    ssize_t bytes_read;

    while ((bytes_read = read(fd, buf, sizeof(buf))) > 0) {
        for (ssize_t i = 0; i < bytes_read; i++) {
            char c = buf[i];
            if (c == '\n' || c == '\r') {
                if (line_len == 0) continue;
                line[line_len] = '\0';
                if (fmt == FORMAT_UNKNOWN) fmt = detect_format(line);
                LogEntry entry;
                if (parse_line(line, fmt, &entry) == 0)
                    update_metrics(m, &entry);
                line_len = 0;
            } else if (line_len < LINE_MAX - 1) {
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
    }

    if (bytes_read < 0) perror("read");
    close(fd);
}

void run_worker_pipe(char **ficheiros, int inicio, int fim, int pipe_fd_write, int verbose) {
    Metrics m;
    init_metrics(&m);

    if (verbose) {
        printf("[Filho %d] Vou processar %d ficheiro(s) (%d..%d)\n",
               getpid(), fim - inicio, inicio, fim - 1);
    }

    for (int i = inicio; i < fim; i++) {
        if (verbose) {
            printf("[Filho %d] A processar: %s\n", getpid(), ficheiros[i]);
        }
        process_file(ficheiros[i], &m);
    }

    WorkerResult result;
    result.pid = getpid();
    result.total_lines = m.total_lines;
    result.count_debug = m.count_debug;
    result.count_info = m.count_info;
    result.count_warn = m.count_warn;
    result.count_error = m.count_error;
    result.count_critical = m.count_critical;
    result.count_4xx = m.count_4xx;
    result.count_5xx = m.count_5xx;
    get_top_ip(&m, result.top_ip);

    ssize_t written = write(pipe_fd_write, &result, sizeof(result));
    if (written < 0) {
        perror("Erro no write do worker");
    } else if (written != sizeof(result)) {
        fprintf(stderr, "Erro: write incompleto (%zd de %zu bytes)\n", written, sizeof(result));
    }

    close(pipe_fd_write);
    exit(0);
}
