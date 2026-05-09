#include "worker_prodcons.h"
#include "parser.h"
#include "posix_io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <errno.h>
#include <ctype.h>
#include <time.h>

#define BUF_SIZE 4096
#define LINE_MAX 512

LogEntry alert_buffer[ALERT_BUFFER_SIZE];
int alert_prodptr = 0;
int alert_consptr = 0;
int alert_count = 0;
pthread_mutex_t alert_trinco;
sem_t alert_vagas;
sem_t alert_itens;
volatile int consumidores_ativos = 1;

static void esperar(sem_t *sem) {
    while (sem_wait(sem) == -1) {
        if (errno != EINTR) {
            perror("sem_wait");
            pthread_exit(NULL);
        }
    }
}

static void assinalar(sem_t *sem) {
    if (sem_post(sem) == -1) {
        perror("sem_post");
        pthread_exit(NULL);
    }
}

void init_bounded_buffer(void) {
    prodptr = 0;
    consptr = 0;
    buffer_count = 0;
    produtores_ativos = 1;

    pthread_mutex_init(&trinco, NULL);
    pthread_mutex_init(&metrics_mutex, NULL);
    sem_init(&vagas, 0, BUFFER_SIZE);
    sem_init(&itens, 0, 0);
}

void destroy_bounded_buffer(void) {
    pthread_mutex_destroy(&trinco);
    pthread_mutex_destroy(&metrics_mutex);
    sem_destroy(&vagas);
    sem_destroy(&itens);
}

void init_alert_buffer(void) {
    alert_prodptr = 0;
    alert_consptr = 0;
    alert_count = 0;
    consumidores_ativos = 1;

    pthread_mutex_init(&alert_trinco, NULL);
    sem_init(&alert_vagas, 0, ALERT_BUFFER_SIZE);
    sem_init(&alert_itens, 0, 0);
}

void destroy_alert_buffer(void) {
    pthread_mutex_destroy(&alert_trinco);
    sem_destroy(&alert_vagas);
    sem_destroy(&alert_itens);
}

static const char *level_to_string(LogLevel level) {
    switch (level) {
        case LEVEL_DEBUG: return "DEBUG";
        case LEVEL_INFO: return "INFO";
        case LEVEL_WARN: return "WARN";
        case LEVEL_ERROR: return "ERROR";
        case LEVEL_CRITICAL: return "CRITICAL";
        default: return "UNKNOWN";
    }
}

static int contains_case_insensitive(const char *text, const char *needle) {
    size_t needle_len = strlen(needle);

    if (needle_len == 0) return 1;

    for (const char *p = text; *p; p++) {
        size_t i = 0;
        while (needle[i] &&
               p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i == needle_len) return 1;
    }

    return 0;
}

static int is_sql_injection(const LogEntry *entry) {
    const char *msg = entry->message;

    return contains_case_insensitive(msg, "sql injection") ||
           contains_case_insensitive(msg, "union select") ||
           contains_case_insensitive(msg, " or 1=1") ||
           contains_case_insensitive(msg, "' or ") ||
           contains_case_insensitive(msg, "\" or ") ||
           contains_case_insensitive(msg, "drop table") ||
           contains_case_insensitive(msg, "information_schema");
}

static int is_security_alert(const LogEntry *entry) {
    return entry->level == LEVEL_ERROR ||
           entry->level == LEVEL_CRITICAL ||
           is_sql_injection(entry);
}

void send_alert(LogEntry *entry) {
    if (sem_trywait(&alert_vagas) != 0) {
        if (errno == EAGAIN) {
            posix_writef(STDERR_FILENO, "[AVISO] Buffer de alertas cheio! Alerta descartado: %s\n",
                    entry->message);
            return;
        }

        perror("sem_trywait");
        return;
    }

    pthread_mutex_lock(&alert_trinco);
    alert_buffer[alert_prodptr] = *entry;
    alert_prodptr = (alert_prodptr + 1) % ALERT_BUFFER_SIZE;
    alert_count++;
    pthread_mutex_unlock(&alert_trinco);

    assinalar(&alert_itens);
}

void *run_alert_monitor(void *arg) {
    (void)arg;

    FILE *f_alertas = fopen("alertas_seguranca.txt", "a");
    if (!f_alertas) {
        perror("alertas_seguranca.txt");
        pthread_exit(NULL);
    }

    while (1) {
        LogEntry alerta;

        esperar(&alert_itens);
        pthread_mutex_lock(&alert_trinco);

        if (alert_count == 0 && !consumidores_ativos) {
            pthread_mutex_unlock(&alert_trinco);
            break;
        }

        if (alert_count == 0) {
            pthread_mutex_unlock(&alert_trinco);
            continue;
        }

        alerta = alert_buffer[alert_consptr];
        alert_consptr = (alert_consptr + 1) % ALERT_BUFFER_SIZE;
        alert_count--;

        pthread_mutex_unlock(&alert_trinco);
        assinalar(&alert_vagas);

        time_t agora = time(NULL);
        struct tm *tm_info = localtime(&agora);
        char timestamp[26];

        if (tm_info) {
            strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);
        } else {
            strncpy(timestamp, "tempo-indisponivel", sizeof(timestamp) - 1);
            timestamp[sizeof(timestamp) - 1] = '\0';
        }

        fprintf(f_alertas,
                "[%s] [%s] IP: %-15s | Evento: %s\n",
                timestamp,
                level_to_string(alerta.level),
                alerta.ip[0] ? alerta.ip : "-",
                alerta.message);
        fflush(f_alertas);
    }

    fclose(f_alertas);
    pthread_exit(NULL);
}

static void inserir_no_buffer(LogEntry entry) {
    esperar(&vagas);
    pthread_mutex_lock(&trinco);

    buffer[prodptr] = entry;
    prodptr = (prodptr + 1) % BUFFER_SIZE;
    buffer_count++;

    pthread_mutex_unlock(&trinco);
    assinalar(&itens);
}

static void process_file_prodcons(const char *path, int worker_index) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        perror("open");
        return;
    }

    if (verbose) posix_writef(STDOUT_FILENO, "[Produtor %d] A abrir: %s\n", worker_index, path);

    char buf[BUF_SIZE];
    char line[LINE_MAX];
    int line_len = 0;
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
                if (parse_line(line, fmt, &entry) == 0) {
                    inserir_no_buffer(entry);
                }

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
        if (parse_line(line, fmt, &entry) == 0) {
            inserir_no_buffer(entry);
        }
    }

    close(fd);
    if (verbose) posix_writef(STDOUT_FILENO, "[Produtor %d] Terminou o ficheiro: %s\n", worker_index, path);
}

void *run_producer(void *arg) {
    int worker_index = *(int *)arg;
    int ficheiros_por_produtor = total_ficheiros / num_produtores;
    int inicio = (worker_index - 1) * ficheiros_por_produtor;
    int fim = (worker_index == num_produtores)
        ? total_ficheiros
        : inicio + ficheiros_por_produtor;

    if (verbose)
        posix_writef(STDOUT_FILENO, "[Produtor %d] A comecar a processar ficheiros [%d, %d)\n",
               worker_index, inicio, fim);

    for (int i = inicio; i < fim; i++) {
        process_file_prodcons(ficheiros[i], worker_index);
    }

    if (verbose)
        posix_writef(STDOUT_FILENO, "[Produtor %d] Terminou todos os ficheiros!\n", worker_index);

    pthread_exit(NULL);
}

void *run_consumer(void *arg) {
    int worker_index = *(int *)arg;

    if (verbose)
        posix_writef(STDOUT_FILENO, "[Consumidor %d] A comecar a processar o buffer...\n", worker_index);

    while (1) {
        LogEntry entry;

        esperar(&itens);
        pthread_mutex_lock(&trinco);

        if (buffer_count == 0 && !produtores_ativos) {
            pthread_mutex_unlock(&trinco);
            if (verbose)
                posix_writef(STDOUT_FILENO, "[Consumidor %d] Sem mais dados. A terminar.\n", worker_index);
            break;
        }

        entry = buffer[consptr];
        consptr = (consptr + 1) % BUFFER_SIZE;
        buffer_count--;

        pthread_mutex_unlock(&trinco);
        assinalar(&vagas);

        pthread_mutex_lock(&metrics_mutex);
        update_metrics(&global_metrics, &entry);
        pthread_mutex_unlock(&metrics_mutex);

        if (is_security_alert(&entry)) {
            send_alert(&entry);
        }
    }

    if (verbose)
        posix_writef(STDOUT_FILENO, "[Consumidor %d] Saiu do loop principal.\n", worker_index);

    pthread_exit(NULL);
}
