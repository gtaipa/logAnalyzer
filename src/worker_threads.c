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

static void process_file_thread(const char *path, Metrics *local_m, int verbose, int worker_index, long *bytes_done) {
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
            } else {
                if (line_len < LINE_MAX - 1) line[line_len++] = c;
            }
        }
        // Atualiza o progresso com bytes lidos (consistente com pipes)
        *bytes_done += bytes_read;
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
    
    // Inicializar contadores (sem pré-contagem, agora com bytes)
    *(t_args->lines_done) = 0;
    *(t_args->lines_total) = 0;  // Será calculado durante o processamento

    // Criar métricas locais
    Metrics local_metrics;
    init_metrics(&local_metrics);

    // Processar os ficheiros (apenas uma leitura, contando bytes)
    for (int i = t_args->inicio; i < t_args->fim; i++) {
        process_file_thread(t_args->ficheiros[i], &local_metrics, t_args->verbose, t_args->worker_index, t_args->lines_done);
    }

    // No final, lines_done contém o total de bytes lidos
    *(t_args->lines_total) = *(t_args->lines_done);

    // Atualizar a variável GLOBAL partilhada por todos, usando o Mutex
    pthread_mutex_lock(t_args->mutex);
    merge_metrics(t_args->global_metrics, &local_metrics);
    pthread_mutex_unlock(t_args->mutex);

    pthread_exit(NULL);
}
