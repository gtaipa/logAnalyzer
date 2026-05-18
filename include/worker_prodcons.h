#ifndef WORKER_PRODCONS_H
#define WORKER_PRODCONS_H

#include <semaphore.h>
#include <pthread.h>

#define BUFFER_SIZE 10

// 1. A unidade básica de dados 📦
typedef struct {
    int log_type;
    char text_line[256];
} LogEntry;

// 2. A estrutura de controlo do Buffer Circular 
typedef struct {
    LogEntry queue[BUFFER_SIZE]; 
    int count;   // Número atual de itens
    int in;      // Índice para o Produtor (entrada)
    int out;     // Índice para o Consumidor (saída)
    pthread_mutex_t mutex; 
    sem_t empty; // Conta as vagas (espaços livres)
    sem_t full;  // Conta os itens (mensagens prontas)
} LogQueue;

// 3. Declaração do buffer como externo 
// Isto diz ao compilador que a variável 'buffer' existe, 
// mas será criada num ficheiro .c (normalmente no main.c)
extern LogQueue buffer;

// 4. Assinaturas das funções (os "contratos") 

void* producer_routine(void* arg);
void* consumer_routine(void* arg);

#endif