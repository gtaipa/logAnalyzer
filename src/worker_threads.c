#include "worker_threads.h"
#include "parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>

#define BUF_SIZE 4096
#define LINE_MAX 512

/* Função para estimar o total de linhas rapidamente */
static long count_lines(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    char buf[BUF_SIZE];
    long count = 0;
    ssize_t n;
    while ((n = read(fd, buf, BUF_SIZE)) > 0) {
        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] == '\n') count++;
        }
    }
    close(fd);
    return count;
}

static void process_file_thread(const char *path, Metrics *local_m, int verbose, int worker_index, long *lines_done) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror("open"); return; }

    if (verbose) fprintf(stderr, "[Thread %d] A abrir: %s\n", worker_index, path);

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

                if (fmt == FORMAT_UNKNOWN) fmt = detect_format(line);

                LogEntry entry;
                if (parse_line(line, fmt, &entry) == 0) update_metrics(local_m, &entry);
                line_len = 0;
                
                // Atualiza o progresso no dashboard
                (*lines_done)++;
            } else {
                if (line_len < LINE_MAX - 1) line[line_len++] = c;
            }
        }
    }

    if (line_len > 0) {
        line[line_len] = '\0';
        if (fmt == FORMAT_UNKNOWN) fmt = detect_format(line);
        LogEntry entry;
        if (parse_line(line, fmt, &entry) == 0) update_metrics(local_m, &entry);
        (*lines_done)++;
    }
    close(fd);
}

void *run_worker_thread(void *arg) {
    ThreadArgs *t_args = (ThreadArgs *)arg;
    
    // 1. Estimar o total de linhas para o Dashboard
    long total = 0;
    for (int i = t_args->inicio; i < t_args->fim; i++) {
        total += count_lines(t_args->ficheiros[i]);
    }
    *(t_args->lines_total) = total;
    *(t_args->lines_done) = 0;

    // 2. Criar métricas locais
    Metrics local_metrics;
    init_metrics(&local_metrics);

    // 3. Processar os ficheiros
    for (int i = t_args->inicio; i < t_args->fim; i++) {
        process_file_thread(t_args->ficheiros[i], &local_metrics, t_args->verbose, t_args->worker_index, t_args->lines_done);
    }

    // Forçar os 100% no final para garantir que arredondamentos não falham
    *(t_args->lines_done) = *(t_args->lines_total);

    // 4. Atualizar a variável GLOBAL partilhada por todos, usando o Mutex
    pthread_mutex_lock(t_args->mutex);
    
    t_args->global_metrics->total_lines    += local_metrics.total_lines;
    t_args->global_metrics->count_debug    += local_metrics.count_debug;
    t_args->global_metrics->count_info     += local_metrics.count_info;
    t_args->global_metrics->count_warn     += local_metrics.count_warn;
    t_args->global_metrics->count_error    += local_metrics.count_error;
    t_args->global_metrics->count_critical += local_metrics.count_critical;
    t_args->global_metrics->count_4xx      += local_metrics.count_4xx;
    t_args->global_metrics->count_5xx      += local_metrics.count_5xx;
    
    for (int i = 0; i < local_metrics.ip_num; i++) {
        if (t_args->global_metrics->ip_num < MAX_IPS) {
            strncpy(t_args->global_metrics->ip_list[t_args->global_metrics->ip_num], local_metrics.ip_list[i], IP_LEN);
            t_args->global_metrics->ip_count[t_args->global_metrics->ip_num] = local_metrics.ip_count[i];
            t_args->global_metrics->ip_num++;
        }
    }

    pthread_mutex_unlock(t_args->mutex);

    pthread_exit(NULL);
}
