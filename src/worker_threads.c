#include "worker_threads.h"
#include "parser.h"
#include "posix_io.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>

#define BUF_SIZE 4096
#define LINE_MAX 512

void *run_worker_thread(void *arg) {
    ThreadArgs *t = (ThreadArgs *)arg;

    off_t byte_inicio = t->byte_inicio;
    off_t byte_fim    = t->byte_fim;
    off_t quota       = byte_fim - byte_inicio;

    *(t->bytes_total) = (long)quota;
    *(t->bytes_done)  = 0;

    Metrics local_metrics;
    init_metrics(&local_metrics);

    off_t global_offset = 0;
    char  buf[BUF_SIZE];
    char  linha[LINE_MAX];

    for (int i = 0; i < t->total_ficheiros; i++) {
        struct stat st;
        if (stat(t->ficheiros[i], &st) != 0) continue;
        off_t fsize = st.st_size;

        /* Ficheiro completamente antes da nossa fatia → ignorar */
        if (global_offset + fsize <= byte_inicio) {
            global_offset += fsize;
            continue;
        }
        /* Ficheiro completamente depois da nossa fatia → parar */
        if (global_offset >= byte_fim) break;

        off_t local_start = (byte_inicio > global_offset) ? byte_inicio - global_offset : 0;
        off_t local_end   = (byte_fim < global_offset + fsize) ? byte_fim - global_offset : fsize;

        int fd = open(t->ficheiros[i], O_RDONLY);
        if (fd < 0) { global_offset += fsize; continue; }

        if (t->verbose)
            posix_writef(STDERR_FILENO, "[Thread %d] %s bytes [%lld-%lld]\n",
                         t->worker_index, t->ficheiros[i],
                         (long long)local_start, (long long)local_end);

        if (lseek(fd, local_start, SEEK_SET) < 0) {
            perror("lseek");
            close(fd);
            global_offset += fsize;
            continue;
        }

        off_t file_pos = local_start;

        /*
         * Se não estamos no início do ficheiro, estamos potencialmente a meio
         * de uma linha que pertence à thread anterior. Avançar até ao '\n'.
         */
        if (local_start > 0) {
            char c;
            ssize_t r;
            while ((r = read(fd, &c, 1)) == 1) {
                file_pos++;
                if (c == '\n') break;
            }
            if (r <= 0) {
                close(fd);
                global_offset += fsize;
                continue;
            }
        }

        int len  = 0;
        int done = 0;
        LogFormat fmt = FORMAT_UNKNOWN;

        while (!done) {
            ssize_t n = read(fd, buf, BUF_SIZE);
            if (n <= 0) break;

            /* Atualizar progresso em bytes */
            off_t pos_na_quota = (global_offset + file_pos) - byte_inicio;
            if (pos_na_quota < 0)     pos_na_quota = 0;
            if (pos_na_quota > quota) pos_na_quota = quota;
            *(t->bytes_done) = (long)pos_na_quota;

            for (int b = 0; b < (int)n && !done; b++) {
                char c = buf[b];
                file_pos++;

                if (c == '\n') {
                    if (len > 0) {
                        linha[len] = '\0';
                        if (fmt == FORMAT_UNKNOWN) fmt = detect_format(linha);
                        LogEntry entry;
                        if (parse_line(linha, fmt, &entry) == 0)
                            update_metrics(&local_metrics, &entry);
                        len = 0;
                    }
                    if (file_pos >= local_end) done = 1;
                } else if (c != '\r') {
                    if (len < LINE_MAX - 1) linha[len++] = c;
                }
            }
        }

        /* Última linha sem '\n' */
        if (len > 0) {
            linha[len] = '\0';
            if (fmt == FORMAT_UNKNOWN) fmt = detect_format(linha);
            LogEntry entry;
            if (parse_line(linha, fmt, &entry) == 0)
                update_metrics(&local_metrics, &entry);
        }

        close(fd);
        global_offset += fsize;
    }

    /* Forçar 100% no final */
    *(t->bytes_done) = (long)quota;

    /* Fundir métricas locais nas globais com proteção de mutex */
    pthread_mutex_lock(t->mutex);

    t->global_metrics->total_lines    += local_metrics.total_lines;
    t->global_metrics->count_debug    += local_metrics.count_debug;
    t->global_metrics->count_info     += local_metrics.count_info;
    t->global_metrics->count_warn     += local_metrics.count_warn;
    t->global_metrics->count_error    += local_metrics.count_error;
    t->global_metrics->count_critical += local_metrics.count_critical;
    t->global_metrics->count_4xx      += local_metrics.count_4xx;
    t->global_metrics->count_5xx      += local_metrics.count_5xx;

    for (int i = 0; i < local_metrics.ip_num; i++) {
        int found = -1;
        for (int j = 0; j < t->global_metrics->ip_num; j++) {
            if (strcmp(t->global_metrics->ip_list[j], local_metrics.ip_list[i]) == 0) {
                found = j;
                break;
            }
        }
        if (found >= 0) {
            t->global_metrics->ip_count[found] += local_metrics.ip_count[i];
        } else if (t->global_metrics->ip_num < MAX_IPS) {
            strncpy(t->global_metrics->ip_list[t->global_metrics->ip_num],
                    local_metrics.ip_list[i], IP_LEN - 1);
            t->global_metrics->ip_list[t->global_metrics->ip_num][IP_LEN - 1] = '\0';
            t->global_metrics->ip_count[t->global_metrics->ip_num] = local_metrics.ip_count[i];
            t->global_metrics->ip_num++;
        }
    }

    for (int i = 0; i < local_metrics.num_alerts && t->global_metrics->num_alerts < MAX_ALERTS; i++) {
        strncpy(t->global_metrics->alerts[t->global_metrics->num_alerts],
                local_metrics.alerts[i], ALERT_LEN - 1);
        t->global_metrics->alerts[t->global_metrics->num_alerts][ALERT_LEN - 1] = '\0';
        t->global_metrics->num_alerts++;
    }

    pthread_mutex_unlock(t->mutex);

    pthread_exit(NULL);
}