#include "ipc.h"
#include "parser.h"
#include "posix_io.h"
#include "worker.h"
#include "worker_prodcons.h"

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

#define BUFFER_SLOTS 10
#define LINE_SIZE 512

static void esperar(sem_t *s) {
    while (sem_wait(s) == -1 && errno == EINTR) {
    }
}

static void assinalar(sem_t *s) {
    sem_post(s);
}

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

static void enviar_resultado(int pipe_fd, Metrics *m) {
    int tipo = MSG_RESULTADO;
    write(pipe_fd, &tipo, sizeof(tipo));

    WorkerResult r;
    preparar_resultado(m, &r);
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

// Update LogQueue and LogLine usage
LogQueue buffer; // Updated to use lowercase 'queue' and 'is_poison' field
int produtores_ativos = 0;
pthread_mutex_t metrics_mutex = PTHREAD_MUTEX_INITIALIZER;

void init_bounded_buffer(void) {
    buffer.in    = 0;
    buffer.out   = 0;
    buffer.count = 0;
    produtores_ativos = 1;
    pthread_mutex_init(&buffer.mutex, NULL);
    pthread_mutex_init(&metrics_mutex, NULL);
    sem_init(&buffer.empty, 0, BUFFER_SIZE);
    sem_init(&buffer.full,  0, 0);
}

void destroy_bounded_buffer(void) {
    pthread_mutex_destroy(&buffer.mutex);
    sem_destroy(&buffer.empty);
    sem_destroy(&buffer.full);
}

static void inserir_no_buffer(LogEntry entry) {
    esperar(&buffer.empty);
    pthread_mutex_lock(&buffer.mutex);
    buffer.queue[buffer.in] = entry;
    buffer.in = (buffer.in + 1) % BUFFER_SIZE;
    buffer.count++;
    pthread_mutex_unlock(&buffer.mutex);
    assinalar(&buffer.full);
}

void *run_consumer(void *arg) {
    (void)arg;
    while (1) {
        esperar(&buffer.full);
        pthread_mutex_lock(&buffer.mutex);

        if (buffer.count == 0 && !produtores_ativos) {
            pthread_mutex_unlock(&buffer.mutex);
            break;
        }

        LogEntry entry = buffer.queue[buffer.out];
        buffer.out = (buffer.out + 1) % BUFFER_SIZE;
        buffer.count--;

        pthread_mutex_unlock(&buffer.mutex);
        assinalar(&buffer.empty);

        // Process entry here
    }
    return NULL;
}
