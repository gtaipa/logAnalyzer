#include "parser.h"
#include "posix_io.h"
#include "worker_prodcons.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/stat.h>

#define BUF_SIZE       4096
#define LINE_MAX_LOCAL  512

LogQueue        buffer;
int             produtores_ativos = 0;
pthread_mutex_t metrics_mutex     = PTHREAD_MUTEX_INITIALIZER;

static void esperar(sem_t *s) {
    while (sem_wait(s) == -1 && errno == EINTR)
        ;
}

static void assinalar(sem_t *s) {
    sem_post(s);
}

void init_bounded_buffer(void) {
    buffer.in    = 0;
    buffer.out   = 0;
    buffer.count = 0;
    pthread_mutex_init(&buffer.mutex, NULL);
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

void *run_producer(void *arg) {
    ProducerArgs *a = (ProducerArgs *)arg;

    off_t byte_inicio = a->byte_inicio;
    off_t byte_fim    = a->byte_fim;
    off_t quota       = byte_fim - byte_inicio;

    if (a->bytes_total) *(a->bytes_total) = (long)quota;
    if (a->bytes_done)  *(a->bytes_done)  = 0;

    off_t global_offset = 0;
    char  buf[BUF_SIZE];
    char  linha[LINE_MAX_LOCAL];

    for (int i = 0; i < a->total_ficheiros; i++) {
        struct stat st;
        if (stat(a->ficheiros[i], &st) != 0) continue;
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

        int fd = open(a->ficheiros[i], O_RDONLY);
        if (fd < 0) { global_offset += fsize; continue; }

        if (a->verbose)
            posix_writef(STDOUT_FILENO,
                         "[Produtor %d] %s bytes [%lld-%lld]\n",
                         a->worker_index, a->ficheiros[i],
                         (long long)local_start, (long long)local_end);

        if (lseek(fd, local_start, SEEK_SET) < 0) {
            perror("lseek");
            close(fd);
            global_offset += fsize;
            continue;
        }

        off_t file_pos = local_start;

        /*
         * Se não estamos no início do ficheiro, avançar até ao '\n'
         * para não processar uma linha que pertence ao produtor anterior.
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
            if (a->bytes_done) *(a->bytes_done) = (long)pos_na_quota;

            for (ssize_t b = 0; b < n && !done; b++) {
                char c = buf[b];
                file_pos++;

                if (c == '\n') {
                    if (len > 0) {
                        linha[len] = '\0';
                        if (fmt == FORMAT_UNKNOWN)
                            fmt = detect_format(linha);
                        LogEntry entry;
                        if (parse_line(linha, fmt, &entry) == 0)
                            inserir_no_buffer(entry);
                        len = 0;
                    }
                    if (file_pos >= local_end) done = 1;
                } else if (c != '\r') {
                    if (len < LINE_MAX_LOCAL - 1)
                        linha[len++] = c;
                }
            }
        }

        /* Última linha sem '\n' */
        if (len > 0) {
            linha[len] = '\0';
            if (fmt == FORMAT_UNKNOWN) fmt = detect_format(linha);
            LogEntry entry;
            if (parse_line(linha, fmt, &entry) == 0)
                inserir_no_buffer(entry);
        }

        close(fd);
        global_offset += fsize;
    }

    /* Forçar 100% no dashboard */
    if (a->bytes_done) *(a->bytes_done) = (long)quota;

    /* Decrementar produtores ativos e acordar consumidores se for o último */
    pthread_mutex_lock(&buffer.mutex);
    produtores_ativos--;
    int consumidores_a_acordar = (produtores_ativos == 0) ? buffer.count + 1 : 0;
    pthread_mutex_unlock(&buffer.mutex);

    for (int k = 0; k < consumidores_a_acordar; k++)
        assinalar(&buffer.full);

    return NULL;
}

void *run_consumer(void *arg) {
    ConsumerArgs *a = (ConsumerArgs *)arg;

    while (1) {
        esperar(&buffer.full);
        pthread_mutex_lock(&buffer.mutex);

        if (buffer.count == 0 && produtores_ativos == 0) {
            pthread_mutex_unlock(&buffer.mutex);
            assinalar(&buffer.full);
            break;
        }

        if (buffer.count == 0) {
            pthread_mutex_unlock(&buffer.mutex);
            continue;
        }

        LogEntry entry = buffer.queue[buffer.out];
        buffer.out = (buffer.out + 1) % BUFFER_SIZE;
        buffer.count--;

        pthread_mutex_unlock(&buffer.mutex);
        assinalar(&buffer.empty);

        pthread_mutex_lock(a->metrics_mutex);
        update_metrics(a->global_metrics, &entry);
        pthread_mutex_unlock(a->metrics_mutex);
    }

    return NULL;
}