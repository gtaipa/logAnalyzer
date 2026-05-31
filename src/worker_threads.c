#include "worker_threads.h"
#include "parser.h"
#include "posix_io.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>

#define BUF_SIZE 4096
#define LINE_MAX 512

/* Conta rapidamente o número de linhas em um ficheiro (para dashboard) */
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

/* Processa um ficheiro de logs, parseia linhas e atualiza métricas locais */
static void process_file_thread(const char *path, Metrics *local_m, int verbose, int worker_index, long *lines_done) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror("open"); return; }

    if (verbose) posix_writef(STDERR_FILENO, "[Thread %d] A abrir: %s\n", worker_index, path);

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

/* Função principal de cada thread worker: processa ficheiros atribuídos e agrega métricas */
void *run_worker_thread(void *arg) {
    ThreadArgs *t_args = (ThreadArgs *)arg;
    
    /* 1. Estimar o total de linhas para o Dashboard */
    long total = 0;
    for (int i = t_args->inicio; i < t_args->fim; i++) {
        total += count_lines(t_args->ficheiros[i]);
    }
    *(t_args->lines_total) = total;
    *(t_args->lines_done) = 0;

    /* 2. Criar métricas locais */
    Metrics local_metrics;
    init_metrics(&local_metrics);

    /* 3. Processar os ficheiros */
    for (int i = t_args->inicio; i < t_args->fim; i++) {
        process_file_thread(t_args->ficheiros[i], &local_metrics, t_args->verbose, t_args->worker_index, t_args->lines_done);
    }

    /* Forçar os 100% no final para garantir que arredondamentos não falham */
    *(t_args->lines_done) = *(t_args->lines_total);

    /* 4. Atualizar a variável GLOBAL partilhada por todos, usando o Mutex */
    pthread_mutex_lock(t_args->mutex);
    
    t_args->global_metrics->total_lines    += local_metrics.total_lines;
    t_args->global_metrics->count_debug    += local_metrics.count_debug;
    t_args->global_metrics->count_info     += local_metrics.count_info;
    t_args->global_metrics->count_warn     += local_metrics.count_warn;
    t_args->global_metrics->count_error    += local_metrics.count_error;
    t_args->global_metrics->count_critical += local_metrics.count_critical;
    t_args->global_metrics->count_4xx      += local_metrics.count_4xx;
    t_args->global_metrics->count_5xx      += local_metrics.count_5xx;

    /* CORREÇÃO: Em vez de adicionar o IP diretamente (causando duplicados),
     * verificar se já existe na lista global e somar a contagem.
     * Sem esta verificação, o mesmo IP aparecia várias vezes no top 10
     * se fosse encontrado por mais do que uma thread. */
    for (int i = 0; i < local_metrics.ip_num; i++) {
        int found = -1;

        /* Procurar se este IP já existe na lista global */
        for (int j = 0; j < t_args->global_metrics->ip_num; j++) {
            if (strcmp(t_args->global_metrics->ip_list[j], local_metrics.ip_list[i]) == 0) {
                found = j;
                break;
            }
        }

        if (found >= 0) {
            /* IP já existe — somar a contagem */
            t_args->global_metrics->ip_count[found] += local_metrics.ip_count[i];
        } else if (t_args->global_metrics->ip_num < MAX_IPS) {
            /* IP novo — adicionar à lista */
            strncpy(t_args->global_metrics->ip_list[t_args->global_metrics->ip_num],
                    local_metrics.ip_list[i], IP_LEN - 1);
            t_args->global_metrics->ip_list[t_args->global_metrics->ip_num][IP_LEN - 1] = '\0';
            t_args->global_metrics->ip_count[t_args->global_metrics->ip_num] = local_metrics.ip_count[i];
            t_args->global_metrics->ip_num++;
        }
    }

    /* Alertas: copiar da local para a global até ao limite */
    for (int i = 0; i < local_metrics.num_alerts && t_args->global_metrics->num_alerts < MAX_ALERTS; i++) {
        strncpy(t_args->global_metrics->alerts[t_args->global_metrics->num_alerts],
                local_metrics.alerts[i], ALERT_LEN - 1);
        t_args->global_metrics->alerts[t_args->global_metrics->num_alerts][ALERT_LEN - 1] = '\0';
        t_args->global_metrics->num_alerts++;
    }

    pthread_mutex_unlock(t_args->mutex);

    pthread_exit(NULL);
}