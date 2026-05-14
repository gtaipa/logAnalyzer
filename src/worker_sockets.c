#include "worker.h"
#include "parser.h"
#include "ipc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#define BUF_SIZE  4096
#define LINHA_MAX  512

/* ─────────────────────────────────────────────────────────────────────────────
 * 1. Funções para enviar mensagens ao pai
 * ───────────────────────────────────────────────────────────────────────────── */
static void enviar_progresso(int sock, int worker_index, long feitas, long total) {
    int tipo = MSG_PROGRESSO;
    write(sock, &tipo, sizeof(tipo));

    ProgressUpdate pu;
    pu.pid          = getpid();
    pu.worker_index = worker_index;
    pu.lines_done   = feitas;
    pu.lines_total  = total;   
    write(sock, &pu, sizeof(pu));
}

static void enviar_resultado(int sock, Metrics *m) {
    int tipo = MSG_RESULTADO;
    write(sock, &tipo, sizeof(tipo));

    WorkerResult r;
    r.pid            = getpid();
    r.total_lines    = m->total_lines;
    r.count_debug    = m->count_debug;
    r.count_info     = m->count_info;
    r.count_warn     = m->count_warn;
    r.count_error    = m->count_error;
    r.count_critical = m->count_critical;
    r.count_4xx      = m->count_4xx;
    r.count_5xx      = m->count_5xx;

    strncpy(r.top_ip, "-", IP_LEN - 1);
    long top = 0;
    for (int i = 0; i < m->ip_num; i++) {
        if (m->ip_count[i] > top) {
            top = m->ip_count[i];
            strncpy(r.top_ip, m->ip_list[i], IP_LEN - 1);
        }
    }
    r.top_ip[IP_LEN - 1] = '\0';
    write(sock, &r, sizeof(r));
}

/* ─────────────────────────────────────────────────────────────────────────────
 * 2. Processamento inteligente por intervalo de linhas
 * ───────────────────────────────────────────────────────────────────────────── */
static void processar_ficheiros(char **ficheiros, int total_ficheiros, Metrics *m,
                                int sock, int worker_index,
                                long linha_inicio, long linha_fim, int verbose) {
    long linha_global = 0;
    long feitas = 0;
    long minha_quota = linha_fim - linha_inicio;

    for (int i = 0; i < total_ficheiros; i++) {
        // Se o worker já processou o seu intervalo de linhas, aborta os restantes ficheiros!
        if (linha_global >= linha_fim) break;

        int fd = open(ficheiros[i], O_RDONLY);
        if (fd < 0) continue;

        if (verbose) printf("[Filho %d] A ler ficheiro: %s\n", getpid(), ficheiros[i]);

        char buf[BUF_SIZE];
        char linha[LINHA_MAX];
        int  len  = 0;
        LogFormat fmt = FORMAT_UNKNOWN;
        int bytes;

        while ((bytes = read(fd, buf, BUF_SIZE)) > 0) {
            for (int b = 0; b < bytes; b++) {
                char c = buf[b];

                if (c == '\n' || c == '\r') {
                    if (len == 0) continue;
                    linha[len] = '\0';

                    // Só processa se a linha global pertencer ao SEU intervalo
                    if (linha_global >= linha_inicio && linha_global < linha_fim) {
                        if (fmt == FORMAT_UNKNOWN) fmt = detect_format(linha);
                        LogEntry entrada;
                        if (parse_line(linha, fmt, &entrada) == 0)
                            update_metrics(m, &entrada);

                        feitas++;
                        // Envia progresso a cada 100 linhas que ELE processou
                        if (feitas % 100 == 0)
                            enviar_progresso(sock, worker_index, feitas, minha_quota);
                    }

                    linha_global++;
                    len = 0;

                    // Otimização: Se atinge o limite do intervalo a meio de um ficheiro, PÁRA DE LER!
                    if (linha_global >= linha_fim) break; 
                } else {
                    if (len < LINHA_MAX - 1) linha[len++] = c;
                }
            }
            if (linha_global >= linha_fim) break; // Sai do ciclo 'while' de read
        }

        // Trata a última linha sem \n
        if (len > 0 && linha_global >= linha_inicio && linha_global < linha_fim) {
            linha[len] = '\0';
            if (fmt == FORMAT_UNKNOWN) fmt = detect_format(linha);
            LogEntry entrada;
            if (parse_line(linha, fmt, &entrada) == 0) update_metrics(m, &entrada);
            feitas++;
            linha_global++;
        }
        close(fd);
    }
}

/* ─────────────────────────────────────────────────────────────────────────────
 * 2. Função Principal do Worker
 * ───────────────────────────────────────────────────────────────────────────── */
void run_worker(char **ficheiros, int total_ficheiros, int num_processos, int worker_index, int verbose) {
    int sock = connect_to_server();
    if (sock < 0) { perror("connect_to_server"); exit(1); }

    /* ✓ NOVA: Receber configuração do PAI (sem contar linhas!) */
    int tipo;
    read(sock, &tipo, sizeof(tipo));
    
    if (tipo != MSG_CONFIG) {
        fprintf(stderr, "Worker esperava MSG_CONFIG, recebeu tipo %d\n", tipo);
        exit(1);
    }
    
    WorkerConfig cfg;
    read(sock, &cfg, sizeof(cfg));
    
    long linha_inicio = cfg.linha_inicio;
    long linha_fim = cfg.linha_fim;
    long minha_quota = linha_fim - linha_inicio;
    
    if (verbose)
        printf("[Worker %d (PID %d)] Intervalo: %ld a %ld (quota: %ld linhas)\n", 
               worker_index, getpid(), linha_inicio, linha_fim, minha_quota);

    Metrics m;
    init_metrics(&m);

    processar_ficheiros(ficheiros, total_ficheiros, &m, sock, worker_index, linha_inicio, linha_fim, verbose);

    /* Enviar progresso 100% e depois resultado */
    enviar_progresso(sock, worker_index, minha_quota, minha_quota);
    enviar_resultado(sock, &m);
    close(sock);
}