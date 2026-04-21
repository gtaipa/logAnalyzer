#include "worker_prodcons.h"
#include "parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>

#define BUF_SIZE 4096
#define LINE_MAX 512

void init_bounded_buffer(BoundedBuffer *buffer) {
    buffer->in = 0;
    buffer->out = 0;
    buffer->count = 0;
    
    pthread_mutex_init(&buffer->mutex, NULL);
    pthread_cond_init(&buffer->cond_not_full, NULL);
    pthread_cond_init(&buffer->cond_not_empty, NULL);
}

void destroy_bounded_buffer(BoundedBuffer *buffer) {
    pthread_mutex_destroy(&buffer->mutex);
    pthread_cond_destroy(&buffer->cond_not_full);
    pthread_cond_destroy(&buffer->cond_not_empty);
}

static void process_file_prodcons(
    const char *path, 
    BoundedBuffer *buffer,
    int verbose, 
    int worker_index
) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        perror("open");
        return;
    }

    if (verbose) printf("[Produtor %d] A abrir: %s\n", worker_index, path);

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
                    
                    pthread_mutex_lock(&buffer->mutex);
                    
                    while (buffer->count == BUFFER_SIZE) {
                        if (verbose)
                            printf("[Produtor %d] Buffer cheio! Esperando...\n", worker_index);
                        pthread_cond_wait(&buffer->cond_not_full, &buffer->mutex);
                    }
                    
                    buffer->buffer[buffer->in] = entry;
                    buffer->in = (buffer->in + 1) % BUFFER_SIZE;
                    buffer->count++;
                    
                    if (verbose && buffer->count == 1)
                        printf("[Produtor %d] Inseriu primeira entrada. Count=%d\n",
                               worker_index, buffer->count);
                    
                    pthread_cond_signal(&buffer->cond_not_empty);
                    
                    pthread_mutex_unlock(&buffer->mutex);
                }
                
                line_len = 0;
            } else {
                if (line_len < LINE_MAX - 1) line[line_len++] = c;
            }
        }
    }

    /* Processar última linha, se houver */
    if (line_len > 0) {
        line[line_len] = '\0';
        if (fmt == FORMAT_UNKNOWN) fmt = detect_format(line);
        
        LogEntry entry;
        if (parse_line(line, fmt, &entry) == 0) {
            pthread_mutex_lock(&buffer->mutex);
            
            while (buffer->count == BUFFER_SIZE) {
                pthread_cond_wait(&buffer->cond_not_full, &buffer->mutex);
            }
            
            buffer->buffer[buffer->in] = entry;
            buffer->in = (buffer->in + 1) % BUFFER_SIZE;
            buffer->count++;
            
            pthread_cond_signal(&buffer->cond_not_empty);
            pthread_mutex_unlock(&buffer->mutex);
        }
    }

    close(fd);
    if (verbose) printf("[Produtor %d] Terminou o ficheiro: %s\n", worker_index, path);
}

void *run_producer(void *arg) {
    ProducerConsumerArgs *pc_args = (ProducerConsumerArgs *)arg;
    int verbose = pc_args->verbose;
    int worker_index = pc_args->worker_index;
    
    if (verbose)
        printf("[Produtor %d] A começar a processar ficheiros [%d, %d)\n",
               worker_index, pc_args->inicio, pc_args->fim);

    /* Processar os ficheiros atribuídos a este produtor */
    for (int i = pc_args->inicio; i < pc_args->fim; i++) {
        process_file_prodcons(
            pc_args->ficheiros[i],
            pc_args->buffer,
            verbose,
            worker_index
        );
    }

    if (verbose)
        printf("[Produtor %d] Terminou todos os ficheiros!\n", worker_index);

    pthread_exit(NULL);
}

void *run_consumer(void *arg) {
    ProducerConsumerArgs *pc_args = (ProducerConsumerArgs *)arg;
    int verbose = pc_args->verbose;
    int worker_index = pc_args->worker_index;
    BoundedBuffer *buffer = pc_args->buffer;
    Metrics *global_metrics = pc_args->global_metrics;
    pthread_mutex_t *metrics_mutex = pc_args->metrics_mutex;

    if (verbose)
        printf("[Consumidor %d] A começar a processar o buffer...\n", worker_index);

    while (1) {
        LogEntry entry;

        pthread_mutex_lock(&buffer->mutex);

        while (buffer->count == 0 && produtores_ativos) {
            if (verbose && worker_index == 1)
                printf("[Consumidores] A aguardar dados do buffer...\n");
            pthread_cond_wait(&buffer->cond_not_empty, &buffer->mutex);
        }

        if (buffer->count == 0 && !produtores_ativos) {
            pthread_mutex_unlock(&buffer->mutex);
            if (verbose)
                printf("[Consumidor %d] Sem mais dados. A terminar.\n", worker_index);
            break;
        }

        entry = buffer->buffer[buffer->out];
        buffer->out = (buffer->out + 1) % BUFFER_SIZE;
        buffer->count--;

        if (verbose && buffer->count == BUFFER_SIZE - 1)
            printf("[Consumidor %d] Buffer agora tem espaço. Count=%d\n",
                   worker_index, buffer->count);

        pthread_cond_signal(&buffer->cond_not_full);

        pthread_mutex_unlock(&buffer->mutex);

        pthread_mutex_lock(metrics_mutex);
        update_metrics(global_metrics, &entry);
        pthread_mutex_unlock(metrics_mutex);
    }

    if (verbose)
        printf("[Consumidor %d] Saiu do loop principal.\n", worker_index);

    pthread_exit(NULL);
}
