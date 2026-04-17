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

/* Função idêntica à da Fase 1, mas não precisa de sockets */
static void process_file_thread(const char *path, Metrics *local_m, int verbose, int worker_index) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror("open"); return; }

    if (verbose) printf("[Thread %d] A abrir: %s\n", worker_index, path);

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
    }
    close(fd);
}

void *run_worker_thread(void *arg) {
    ThreadArgs *t_args = (ThreadArgs *)arg;
    
    // 1. Criar métricas locais só para esta thread (para ser rápido e não bloquear as outras)
    Metrics local_metrics;
    init_metrics(&local_metrics);

    // 2. Processar os ficheiros que calharam a esta thread
    for (int i = t_args->inicio; i < t_args->fim; i++) {
        process_file_thread(t_args->ficheiros[i], &local_metrics, t_args->verbose, t_args->worker_index);
    }

    // 3. Atualizar a variável GLOBAL partilhada por todos, usando o Mutex!
    pthread_mutex_lock(t_args->mutex);
    
    t_args->global_metrics->total_lines    += local_metrics.total_lines;
    t_args->global_metrics->count_debug    += local_metrics.count_debug;
    t_args->global_metrics->count_info     += local_metrics.count_info;
    t_args->global_metrics->count_warn     += local_metrics.count_warn;
    t_args->global_metrics->count_error    += local_metrics.count_error;
    t_args->global_metrics->count_critical += local_metrics.count_critical;
    t_args->global_metrics->count_4xx      += local_metrics.count_4xx;
    t_args->global_metrics->count_5xx      += local_metrics.count_5xx;
    
    // Simplificação para o Top IP: se o atual tiver mais acessos, sobrepõe
    for (int i = 0; i < local_metrics.ip_num; i++) {
        // Lógica simples de merge de IPs (para o requisito básico serve bem)
        if (t_args->global_metrics->ip_num < MAX_IPS) {
            strncpy(t_args->global_metrics->ip_list[t_args->global_metrics->ip_num], local_metrics.ip_list[i], IP_LEN);
            t_args->global_metrics->ip_count[t_args->global_metrics->ip_num] = local_metrics.ip_count[i];
            t_args->global_metrics->ip_num++;
        }
    }

    pthread_mutex_unlock(t_args->mutex);

    if (t_args->verbose) {
        printf("[Thread %d] Terminei e guardei os resultados globais.\n", t_args->worker_index);
    }

    pthread_exit(NULL);
}