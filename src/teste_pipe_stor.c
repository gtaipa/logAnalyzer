#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>

// Estrutura simples de métricas (estilo direto do professor)
typedef struct {
    long linhas;
    int erros;
} Resultados;

int main(void) {
    int fd[2]; // O pipe: fd[0] é para ler, fd[1] é para escrever
    pid_t pid;
    Resultados res;

    // 1. Criar o tubo (Pipe)
    if (pipe(fd) < 0) { 
        perror("Erro ao criar pipe"); 
        exit(1); 
    }

    // 2. Criar o processo filho
    pid = fork();

    if (pid == -1) {
        perror("Erro no fork"); 
        exit(1);
    }

    if (pid == 0) { 
        /* --- CÓDIGO DO FILHO --- */
        close(fd[0]); // O filho não vai ler, por isso fecha a porta de leitura
        
        // Simular o trabalho de analisar logs
        res.linhas = 100;
        res.erros = 5;
        
        // Enviar a estrutura toda pelo tubo (sem funções complicadas)
        write(fd[1], &res, sizeof(Resultados));
        
        close(fd[1]); // Fecha a porta de escrita quando termina
        exit(0);

    } else { 
        /* --- CÓDIGO DO PAI --- */
        close(fd[1]); // O pai não vai escrever, fecha a porta de escrita
        
        // Fica à espera e lê a estrutura do tubo
        read(fd[0], &res, sizeof(Resultados));
        
        // Imprime o que recebeu do filho
        printf("Pai recebeu: %ld linhas e %d erros\n", res.linhas, res.erros);
        
        close(fd[0]); // Fecha a porta de leitura
        wait(NULL);   // Espera que o filho morra (para não virar zombie)
    }

    return 0;
}
