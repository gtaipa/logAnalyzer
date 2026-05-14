#include "ipc.h"
#include "parser.h"
#include "posix_io.h"
#include "worker.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BUF_SIZE 4096
#define LINE_MAX_LOCAL 512

#define MSG_PROGRESSO 1
#define MSG_RESULTADO 2

static void enviar_progresso(int pipe_fd, int worker_index, long feitas, long total) {
    int tipo = MSG_PROGRESSO;
    write(pipe_fd, &tipo, sizeof(tipo));

    ProgressUpdate pu;
    pu.pid          = getpid();
    pu.worker_index = worker_index;
    pu.lines_done   = feitas;
    pu.lines_total  = total;   
    write(pipe_fd, &pu, sizeof(pu));
}

static void enviar_resultado(int pipe_fd, Metrics *m) {
    int tipo = MSG_RESULTADO;
    write(pipe_fd, &tipo, sizeof(tipo));

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
    write(pipe_fd, &r, sizeof(r));
}

void run_worker_pipe(char **ficheiros, int total_ficheiros, int pipe_fd_write, 
                     int worker_index, long linha_inicio, long linha_fim, int verbose) {
    
    Metrics m;
    init_metrics(&m);

    long minha_quota = linha_fim - linha_inicio;
    long linha_global = 0;
    long feitas = 0;
    
    if (verbose) {
        posix_writef(STDOUT_FILENO, "[Filho %d] Intervalo: %ld a %ld (quota: %ld linhas)\n",
               getpid(), linha_inicio, linha_fim, minha_quota);
    }

    for (int i = 0; i < total_ficheiros; i++) {
        if (linha_global >= linha_fim) break;
        
        int fd = open(ficheiros[i], O_RDONLY);
        if (fd == -1) continue;
        
        char buf[BUF_SIZE];
        char linha[LINE_MAX_LOCAL];
        int len = 0;
        LogFormat fmt = FORMAT_UNKNOWN;
        int bytes;
        
        while ((bytes = read(fd, buf, BUF_SIZE)) > 0) {
            for (int b = 0; b < bytes; b++) {
                char c = buf[b];
                
                if (c == '\n' || c == '\r') {
                    if (len == 0) continue;
                    linha[len] = '\0';
                    
                    if (linha_global >= linha_inicio && linha_global < linha_fim) {
                        if (fmt == FORMAT_UNKNOWN) fmt = detect_format(linha);
                        LogEntry entry;
                        if (parse_line(linha, fmt, &entry) == 0)
                            update_metrics(&m, &entry);
                        
                        feitas++;
                        // Envia progresso a cada 100 linhas processadas
                        if (feitas % 100 == 0) {
                            enviar_progresso(pipe_fd_write, worker_index, feitas, minha_quota);
                        }
                    }
                    
                    linha_global++;
                    len = 0;
                    
                    if (linha_global >= linha_fim) break;
                } else {
                    if (len < LINE_MAX_LOCAL - 1) linha[len++] = c;
                }
            }
            if (linha_global >= linha_fim) break;
        }
        
        if (len > 0 && linha_global >= linha_inicio && linha_global < linha_fim) {
            linha[len] = '\0';
            if (fmt == FORMAT_UNKNOWN) fmt = detect_format(linha);
            LogEntry entry;
            if (parse_line(linha, fmt, &entry) == 0) update_metrics(&m, &entry);
            feitas++;
            linha_global++;
        }
        
        close(fd);
    }

    // Enviar progresso a 100%
    enviar_progresso(pipe_fd_write, worker_index, minha_quota, minha_quota);
    
    // Enviar resultado final
    enviar_resultado(pipe_fd_write, &m);

    if (close(pipe_fd_write) == -1) {
        perror("close");
        exit(1);
    }

    exit(0);
}