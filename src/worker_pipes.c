#include "ipc.h"
#include "parser.h"
#include "posix_io.h"
#include "worker.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define BUF_SIZE       4096
#define LINE_MAX_LOCAL 512
//test
#define MSG_PROGRESSO 1
#define MSG_RESULTADO 2

/* ─────────────────────────────────────────────────────────────────────────────
 * Funções para enviar mensagens ao pai via pipe
 * ───────────────────────────────────────────────────────────────────────────── */

/* Envia atualização de progresso ao pai */
static void enviar_progresso(int pipe_fd, int worker_index, long bytes_done, long bytes_total) {
    int tipo = MSG_PROGRESSO;
    write(pipe_fd, &tipo, sizeof(tipo));

    ProgressUpdate pu;
    pu.pid          = getpid();
    pu.worker_index = worker_index;
    pu.bytes_done   = bytes_done;
    pu.bytes_total  = bytes_total;
    write(pipe_fd, &pu, sizeof(pu));
}

/* Prepara o resultado final para envio ao pai */
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
    int n = m->ip_num;
    if (n > MAX_IPS) n = MAX_IPS;

    for (int i = 0; i < n; i++) {
        strncpy(ips[i], m->ip_list[i], IP_LEN - 1);
        ips[i][IP_LEN - 1] = '\0';
        counts[i] = m->ip_count[i];
    }

    for (int i = 0; i < n - 1; i++) {
        for (int j = 0; j < n - i - 1; j++) {
            if (counts[j] < counts[j + 1]) {
                long tmp_count = counts[j];
                counts[j] = counts[j + 1];
                counts[j + 1] = tmp_count;

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

/* Envia resultado final ao pai */
static void enviar_resultado(int pipe_fd, Metrics *m) {
    int tipo = MSG_RESULTADO;
    write(pipe_fd, &tipo, sizeof(tipo));

    WorkerResult r;
    preparar_resultado(m, &r);
    write(pipe_fd, &r, sizeof(r));
}

/*
 * run_worker_pipe - Processa a fatia [byte_inicio, byte_fim) de ficheiros
 *
 * Utiliza lseek() para saltar diretamente para o offset certo em cada ficheiro.
 * Garante processamento de apenas linhas completas (evita fragmentação entre workers).
 */
void run_worker_pipe(char **ficheiros, int total_ficheiros, int pipe_fd_write,
                     int worker_index, off_t byte_inicio, off_t byte_fim, int verbose) {
    Metrics m;
    init_metrics(&m);

    off_t quota          = byte_fim - byte_inicio;
    off_t global_offset  = 0; /* byte global acumulado até ao início do ficheiro actual */
    long  linhas_feitas  = 0; /* contador de linhas para cadência do progresso */

    char buf[BUF_SIZE];
    char linha[LINE_MAX_LOCAL];

    if (verbose)
        posix_writef(STDOUT_FILENO,
                     "[Worker %d PID %d] intervalo bytes: [%lld, %lld)\n",
                     worker_index, (int)getpid(),
                     (long long)byte_inicio, (long long)byte_fim);

    for (int i = 0; i < total_ficheiros; i++) {
        struct stat st;
        if (stat(ficheiros[i], &st) != 0) continue;
        off_t fsize = st.st_size;

        /* Ficheiro completamente antes da nossa fatia → ignorar */
        if (global_offset + fsize <= byte_inicio) {
            global_offset += fsize;
            continue;
        }
        /* Ficheiro completamente depois da nossa fatia → parar */
        if (global_offset >= byte_fim) break;

        /* Offset local (dentro deste ficheiro) onde a nossa fatia começa e acaba */
        off_t local_start = (byte_inicio > global_offset) ? byte_inicio - global_offset : 0;
        off_t local_end   = (byte_fim < global_offset + fsize) ? byte_fim - global_offset : fsize;

        int fd = open(ficheiros[i], O_RDONLY);
        if (fd < 0) { global_offset += fsize; continue; }

        if (verbose)
            posix_writef(STDOUT_FILENO,
                         "[Worker %d] %s local [%lld-%lld]\n",
                         worker_index, ficheiros[i],
                         (long long)local_start, (long long)local_end);

        /* Saltar directamente para o offset de início */
        if (lseek(fd, local_start, SEEK_SET) < 0) {
            perror("lseek");
            close(fd);
            global_offset += fsize;
            continue;
        }

        off_t file_pos = local_start;

        /*
         * Se não estamos no início do ficheiro, estamos potencialmente a meio de
         * uma linha que pertence ao worker anterior.  Avançamos até ao próximo '\n'.
         */
        if (local_start > 0) {
            char c;
            ssize_t r;
            while ((r = read(fd, &c, 1)) == 1) {
                file_pos++;
                if (c == '\n') break;
            }
            if (r <= 0) {
                /* EOF ou erro — não há linhas neste ficheiro para nós */
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

                    linhas_feitas++;
                    if (linhas_feitas % 100 == 0) {
                        /* Progresso em bytes dentro da nossa quota */
                        off_t bytes_done = global_offset + file_pos - byte_inicio;
                        if (bytes_done > quota) bytes_done = quota;
                        enviar_progresso(pipe_fd_write, worker_index,
                                         (long)bytes_done, (long)quota);
                    }

                    /* Completámos a última linha da nossa fatia → podemos parar */
                    if (file_pos >= local_end) done = 1;

                } else if (c != '\r') {
                    if (len < LINE_MAX_LOCAL - 1) linha[len++] = c;
                    /*
                     * Se já passámos local_end mas ainda não vimos '\n', continuamos
                     * a acumular para terminar a linha actual (sem entrar no done).
                     */
                }
            }
        }

        /* Última linha sem '\n' (fim de ficheiro sem terminador) */
        if (len > 0) {
            linha[len] = '\0';
            if (fmt == FORMAT_UNKNOWN) fmt = detect_format(linha);
            LogEntry entry;
            if (parse_line(linha, fmt, &entry) == 0)
                update_metrics(&m, &entry);
        }

        close(fd);
        global_offset += fsize;
    }

    /* Progresso final a 100% */
    enviar_progresso(pipe_fd_write, worker_index, (long)quota, (long)quota);

    /* Resultado final */
    enviar_resultado(pipe_fd_write, &m);

    if (close(pipe_fd_write) == -1) {
        perror("close");
        exit(1);
    }

    exit(0);
}
