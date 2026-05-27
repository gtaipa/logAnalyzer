#include "ipc.h"
#include "parser.h"
#include "posix_io.h"
#include "worker.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define BUF_SIZE       4096
#define LINE_MAX_LOCAL 512
#define MSG_RESULTADO  2

/*
 * g_progress — definido em main_pipes.c, apontado para a região mmap MAP_SHARED.
 * O filho escreve apenas no seu slot [worker_index]; o pai lê todos os slots.
 */
extern volatile int *g_progress;

/* ── Resultado final (enviado uma vez, pelo pipe, no fim) ─────────── */

static void preparar_resultado(const Metrics *m, WorkerResult *r) {
    memset(r, 0, sizeof(*r));
    r->pid            = getpid();
    r->total_lines    = m->total_lines;
    r->count_debug    = m->count_debug;
    r->count_info     = m->count_info;
    r->count_warn     = m->count_warn;
    r->count_error    = m->count_error;
    r->count_critical = m->count_critical;
    r->count_4xx      = m->count_4xx;
    r->count_5xx      = m->count_5xx;

    char ips[MAX_IPS][IP_LEN];
    long counts[MAX_IPS];
    int n = m->ip_num < MAX_IPS ? m->ip_num : MAX_IPS;

    for (int i = 0; i < n; i++) {
        strncpy(ips[i], m->ip_list[i], IP_LEN - 1);
        ips[i][IP_LEN - 1] = '\0';
        counts[i] = m->ip_count[i];
    }
    for (int i = 0; i < n - 1; i++) {
        for (int j = 0; j < n - i - 1; j++) {
            if (counts[j] < counts[j + 1]) {
                long tmp = counts[j]; counts[j] = counts[j + 1]; counts[j + 1] = tmp;
                char tmp_ip[IP_LEN];
                strncpy(tmp_ip, ips[j], IP_LEN);
                strncpy(ips[j], ips[j + 1], IP_LEN);
                strncpy(ips[j + 1], tmp_ip, IP_LEN);
            }
        }
    }
    int limite = n < 10 ? n : 10;
    for (int i = 0; i < limite; i++) {
        strncpy(r->top_ips[i], ips[i], IP_LEN - 1);
        r->top_ips[i][IP_LEN - 1] = '\0';
        r->top_ips_counts[i] = counts[i];
    }
    r->num_alerts = m->num_alerts < MAX_ALERTS ? m->num_alerts : MAX_ALERTS;
    for (int i = 0; i < r->num_alerts; i++) {
        strncpy(r->alerts[i], m->alerts[i], ALERT_LEN - 1);
        r->alerts[i][ALERT_LEN - 1] = '\0';
    }
}

static void enviar_resultado(int pipe_fd, Metrics *m) {
    int tipo = MSG_RESULTADO;
    write(pipe_fd, &tipo, sizeof(tipo));
    WorkerResult r;
    preparar_resultado(m, &r);
    write(pipe_fd, &r, sizeof(r));
}

/* ── Notificação de progresso via SIGUSR1 ─────────────────────────
 *
 * Regra: a cada 10% do trabalho concluído, o filho actualiza o seu
 * slot em g_progress e envia SIGUSR1 ao pai.  O pai redesenha o
 * dashboard sem custo de I/O no pipe (que fica reservado só para
 * o resultado final).
 * ─────────────────────────────────────────────────────────────────── */
static inline void notificar_progresso(int worker_index, off_t bytes_feitos,
                                       off_t quota, int *last_milestone) {
    int pct = (quota > 0) ? (int)(bytes_feitos * 100 / quota) : 100;
    if (pct > 100) pct = 100;

    int milestone = pct / 10;          /* 0..10 — muda a cada 10% */
    if (milestone > *last_milestone) {
        *last_milestone          = milestone;
        g_progress[worker_index] = pct;
        kill(getppid(), SIGUSR1);
    }
}

/* ── Processamento por fatia de bytes ────────────────────────────── */

void run_worker_pipe(char **ficheiros, int total_ficheiros, int pipe_fd_write,
                     int worker_index, off_t byte_inicio, off_t byte_fim, int verbose) {
    Metrics m;
    init_metrics(&m);

    off_t quota         = byte_fim - byte_inicio;
    off_t global_offset = 0;
    off_t bytes_feitos  = 0;   /* bytes processados dentro da nossa fatia */
    int   last_milestone = -1; /* última décima de 10% já reportada */

    char buf[BUF_SIZE];
    char linha[LINE_MAX_LOCAL];

    if (verbose)
        posix_writef(STDOUT_FILENO,
                     "[Worker %d PID %d] bytes [%lld, %lld)\n",
                     worker_index, (int)getpid(),
                     (long long)byte_inicio, (long long)byte_fim);

    for (int i = 0; i < total_ficheiros; i++) {
        struct stat st;
        if (stat(ficheiros[i], &st) != 0) continue;
        off_t fsize = st.st_size;

        if (global_offset + fsize <= byte_inicio) { global_offset += fsize; continue; }
        if (global_offset >= byte_fim) break;

        off_t local_start = (byte_inicio > global_offset) ? byte_inicio - global_offset : 0;
        off_t local_end   = (byte_fim < global_offset + fsize) ? byte_fim - global_offset : fsize;

        int fd = open(ficheiros[i], O_RDONLY);
        if (fd < 0) { global_offset += fsize; continue; }

        if (verbose)
            posix_writef(STDOUT_FILENO, "[Worker %d] %s [%lld-%lld]\n",
                         worker_index, ficheiros[i],
                         (long long)local_start, (long long)local_end);

        if (lseek(fd, local_start, SEEK_SET) < 0) {
            perror("lseek"); close(fd); global_offset += fsize; continue;
        }

        off_t file_pos = local_start;

        /* Se não estamos no início do ficheiro, avançar até ao próximo '\n' */
        if (local_start > 0) {
            char c; ssize_t r;
            while ((r = read(fd, &c, 1)) == 1) {
                file_pos++;
                if (c == '\n') break;
            }
            if (r <= 0) { close(fd); global_offset += fsize; continue; }
        }

        int len  = 0;
        int done = 0;
        LogFormat fmt = FORMAT_UNKNOWN;

        while (!done) {
            ssize_t n = read(fd, buf, BUF_SIZE);
            if (n <= 0) break;

            for (int b = 0; b < (int)n && !done; b++) {
                char c = buf[b];
                file_pos++;

                if (c == '\n') {
                    if (len > 0) {
                        linha[len] = '\0';
                        if (fmt == FORMAT_UNKNOWN) fmt = detect_format(linha);
                        LogEntry entry;
                        if (parse_line(linha, fmt, &entry) == 0)
                            update_metrics(&m, &entry);
                        len = 0;
                    }

                    /* Actualizar contagem de bytes e verificar milestone */
                    bytes_feitos = global_offset + file_pos - byte_inicio;
                    if (bytes_feitos > quota) bytes_feitos = quota;
                    notificar_progresso(worker_index, bytes_feitos,
                                        quota, &last_milestone);

                    if (file_pos >= local_end) done = 1;

                } else if (c != '\r') {
                    if (len < LINE_MAX_LOCAL - 1) linha[len++] = c;
                }
            }
        }

        /* Última linha sem '\n' */
        if (len > 0) {
            linha[len] = '\0';
            if (fmt == FORMAT_UNKNOWN) fmt = detect_format(linha);
            LogEntry entry;
            if (parse_line(linha, fmt, &entry) == 0) update_metrics(&m, &entry);
        }

        close(fd);
        global_offset += fsize;
    }

    /* Garantir 100% no dashboard antes de enviar o resultado */
    g_progress[worker_index] = 100;
    kill(getppid(), SIGUSR1);

    enviar_resultado(pipe_fd_write, &m);

    if (close(pipe_fd_write) == -1) { perror("close"); exit(1); }
    exit(0);
}
