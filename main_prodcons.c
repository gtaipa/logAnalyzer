#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>
#include <sys/types.h>
#include <sys/fcntl.h>
#include <unistd.h>
#include <semaphore.h>
#include "worker_prodcons.h"
#define BUFFER_SIZE 10  


LogQueue buffer;

int main(){
 pthread_mutex_init(&(buffer.mutex), NULL);
 sem_init(&(buffer.empty), 0, BUFFER_SIZE);
 sem_init(&(buffer.full), 0, 0);
    
}
void *producer(void *arg) {
    int fd = open("log.txt", O_RDONLY);
    if (fd < 0) {
    perror("Erro no open");
    exit(EXIT_FAILURE);
}
    ssize_t line_size;

    while (1) {
        sem_wait(&(buffer.empty)); // Espera por espaço vazio
        pthread_mutex_lock(&(buffer.mutex)); // Tranca a porta
        
        line_size = read(fd, buffer.Queue[buffer.in].text_line, BUFFER_SIZE);
        
        // Se o ficheiro acabou, enviamos a pílula venenosa!
        if (line_size == 0) {
            buffer.Queue[buffer.in].text_line[0] = '\0'; // Sinal de fim
            buffer.in = (buffer.in + 1) % BUFFER_SIZE;
            pthread_mutex_unlock(&(buffer.mutex));
            sem_post(&(buffer.full)); // Avisa o consumidor!
            break; // Sai do loop
        }
        
        // Se não acabou, preenche normalmente
        buffer.Queue[buffer.in].text_line[line_size] = '\0';
        buffer.in = (buffer.in + 1) % BUFFER_SIZE;
        
        pthread_mutex_unlock(&(buffer.mutex)); // Destranca a porta
        sem_post(&(buffer.full)); // Incrementa o número de espaços cheios 
    }
    close(fd); // Boa prática: fechar o ficheiro no fim
    return NULL;
}

void* consumer_routine(void* arg) {
    // Abre o ficheiro de saída (adicionei permissões 0666 para evitar erros de leitura depois) , O_TRUNC serve para apagar o conteudo do ficheiro se ele ja existir 
    int fd2 = open("results.txt", O_CREAT | O_WRONLY | O_TRUNC, 0666);
    if (fd2 < 0) {
    perror("Erro no open");
    exit(EXIT_FAILURE);
}
   
    while (1) {
        sem_wait(&(buffer.full)); // Espera por um espaço cheio
        pthread_mutex_lock(&(buffer.mutex)); // Tranca a porta

        // 1. Verifica se é a pílula venenosa
        if (buffer.Queue[buffer.out].text_line[0] == '\0') {
            buffer.out = (buffer.out + 1) % BUFFER_SIZE;
            pthread_mutex_unlock(&(buffer.mutex));
            sem_post(&(buffer.empty)); // CORREÇÃO: Avisa que há um espaço VAZIO
            break; // Sai do loop
        }

        // 2. Se não for, copia a linha para o ficheiro
        write(fd2, buffer.Queue[buffer.out].text_line, strlen(buffer.Queue[buffer.out].text_line));
       
        // 3. Avança o índice
        buffer.out = (buffer.out + 1) % BUFFER_SIZE;

        pthread_mutex_unlock(&(buffer.mutex)); // Destranca a porta
        sem_post(&(buffer.empty)); // Avisa que há um espaço vazio
    }
    close(fd2); // Boa prática: fechar o ficheiro
    return NULL;
}
