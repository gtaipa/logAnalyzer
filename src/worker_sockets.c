#include "worker.h"
#include "parser.h"
#include "ipc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>

#define BUF_SIZE  4096
#define LINHA_MAX  512

/* ─────────────────────────────────────────────────────────────────────────────
 * Funções para enviar mensagens ao pai
 * ───────────────────────────────────────────────────────────────────────────── */
/* ─────────────────────────────────────────────────────────────────────────────
 * Funções para enviar mensagens ao pai via Unix Domain Socket
 * ───────────────────────────────────────────────────────────────────────────── */

/* Envia atualização de progresso ao pai via socket */
static void enviar_progresso(int sock, int worker_index, long bytes_done, long bytes_total) {
    int tipo = MSG_PROGRESSO;
    write(sock, &tipo, sizeof(tipo));

    ProgressUpdate pu;
    pu.pid          = getpid();
    pu.worker_index = worker_index;
    pu.bytes_done   = bytes_done;
    pu.bytes_total  = bytes_total;
    write(sock, &pu, sizeof(pu));
}

/* Prepara resultado final para envio ao pai */
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

/* Envia resultado final ao pai via socket */
static void enviar_resultado(int sock, Metrics *m) {
    int tipo = MSG_RESULTADO;
    write(sock, &tipo, sizeof(tipo));

    WorkerResult r;
    preparar_resultado(m, &r);
    write(sock, &r, sizeof(r));
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Processamento por fatia de bytes
 *
 * Recebe o intervalo global [byte_inicio, byte_fim).  Para cada ficheiro cujo
 * intervalo de bytes se sobreponha ao nosso, usa lseek() para saltar directamente
 * para o offset correcto e lê apenas os bytes que nos pertencem.
 *
 * Tratamento de fronteiras:
 *   • Início no meio de um ficheiro → avança até ao próximo '\n' (evita linha
 *     parcial que pertence ao worker anterior).
 *   • Fim no meio de uma linha → continua a ler até ao '\n' (para não partir
 *     uma linha entre dois workers).
 * ───────────────────────────────────────────────────────────────────────────── */
/* ─────────────────────────────────────────────────────────────────────────────
 * Processamento de lógicas por fatia de bytes com tratamento de fronteiras
 * ───────────────────────────────────────────────────────────────────────────── */

/* Processa intervalo de bytes com sincronização entre workers */
static void processar_por_bytes(char **ficheiros, int total_ficheiros, Metrics *m,
                                int sock, int worker_index,
                                off_t byte_inicio, off_t byte_fim, int verbose) {
    off_t quota         = byte_fim - byte_inicio;
    off_t global_offset = 0;
    long  linhas_feitas = 0;

    char buf[BUF_SIZE];
    char linha[LINHA_MAX];

    for (int i = 0; i < total_ficheiros; i++) {
        struct stat st;
        if (stat(ficheiros[i], &st) != 0) continue;
        off_t fsize = st.st_size;

        /* Ficheiro completamente antes da nossa fatia */
        if (global_offset + fsize <= byte_inicio) {
            global_offset += fsize;
            continue;
        }
        /* Ficheiro completamente depois da nossa fatia */
        if (global_offset >= byte_fim) break;

        off_t local_start = (byte_inicio > global_offset) ? byte_inicio - global_offset : 0;
        off_t local_end   = (byte_fim < global_offset + fsize) ? byte_fim - global_offset : fsize;

        int fd = open(ficheiros[i], O_RDONLY);
        if (fd < 0) { global_offset += fsize; continue; }

        if (verbose)
            printf("[Worker %d] %s local [%lld-%lld]\n",
                   worker_index, ficheiros[i],
                   (long long)local_start, (long long)local_end);

        if (lseek(fd, local_start, SEEK_SET) < 0) {
            perror("lseek");
            close(fd);
            global_offset += fsize;
            continue;
        }

        off_t file_pos = local_start;

        /* Se não estamos no início do ficheiro, avançar até ao próximo '\n' */
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

            for (int b = 0; b < (int)n && !done; b++) {
                char c = buf[b];
                file_pos++;

                if (c == '\n') {
                    if (len > 0) {
                        linha[len] = '\0';
                        if (fmt == FORMAT_UNKNOWN) fmt = detect_format(linha);
                        LogEntry entrada;
                        if (parse_line(linha, fmt, &entrada) == 0)
                            update_metrics(m, &entrada);
                        len = 0;
                    }

                    linhas_feitas++;
                    if (linhas_feitas % 100 == 0) {
                        off_t bytes_done = global_offset + file_pos - byte_inicio;
                        if (bytes_done > quota) bytes_done = quota;
                        enviar_progresso(sock, worker_index,
                                         (long)bytes_done, (long)quota);
                    }

                    /* Linha completa e já passámos o fim da nossa fatia → parar */
                    if (file_pos >= local_end) done = 1;

                } else if (c != '\r') {
                    if (len < LINHA_MAX - 1) linha[len++] = c;
                    /* Se passámos local_end a meio de uma linha, continuamos a acumular */
                }
            }
        }

        /* Última linha sem '\n' (EOF sem terminador) */
        if (len > 0) {
            linha[len] = '\0';
            if (fmt == FORMAT_UNKNOWN) fmt = detect_format(linha);
            LogEntry entrada;
            if (parse_line(linha, fmt, &entrada) == 0)
                update_metrics(m, &entrada);
        }

        close(fd);
        global_offset += fsize;
    }
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Função Principal do Worker (sockets)
 * ───────────────────────────────────────────────────────────────────────────── */
void run_worker(char **ficheiros, int total_ficheiros, int num_processos, int worker_index_original, int verbose) {
    (void)num_processos;
    (void)worker_index_original;

    int sock = connect_to_server();
    if (sock < 0) { perror("connect_to_server"); exit(1); }

    int tipo;
    read(sock, &tipo, sizeof(tipo));

    if (tipo != MSG_CONFIG) {
        fprintf(stderr, "Worker esperava MSG_CONFIG, recebeu tipo %d\n", tipo);
        exit(1);
    }

    WorkerConfig cfg;
    read(sock, &cfg, sizeof(cfg));

    int   worker_index = cfg.worker_index;
    off_t byte_inicio  = cfg.byte_inicio;
    off_t byte_fim     = cfg.byte_fim;
    off_t quota        = byte_fim - byte_inicio;

    if (verbose)
        printf("[Worker %d (PID %d)] intervalo bytes: [%lld, %lld) quota=%lld\n",
               worker_index, (int)getpid(),
               (long long)byte_inicio, (long long)byte_fim, (long long)quota);

    Metrics m;
    init_metrics(&m);

    processar_por_bytes(ficheiros, total_ficheiros, &m,
                        sock, worker_index, byte_inicio, byte_fim, verbose);

    enviar_progresso(sock, worker_index, (long)quota, (long)quota);
    enviar_resultado(sock, &m);
    close(sock);
}
