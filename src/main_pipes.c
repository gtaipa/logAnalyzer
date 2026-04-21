#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>

#include "ipc.h"

void run_worker_pipe(char **ficheiros, int inicio, int fim, int pipe_fd_write, int verbose);

int main(int argc, char *argv[]) {
    if (argc < 4) {
        printf("Uso: %s <diretorio> <num_processos> <modo> [--verbose]\n", argv[0]);
        return 1;
    }

    char *diretorio = argv[1];
    int num_processos = atoi(argv[2]);
    char *modo = argv[3];

    int verbose = 0;
    for (int i = 4; i < argc; i++)
        if (strcmp(argv[i], "--verbose") == 0)
            verbose = 1;

    printf("Diretorio: %s\n", diretorio);
    printf("Processos: %d\n", num_processos);
    printf("Modo: %s\n", modo);
    printf("Verbose: %s\n", verbose ? "sim" : "nao");

    int capacidade = 10, total_ficheiros = 0;
    char **ficheiros = malloc(capacidade * sizeof(char *));
    if (!ficheiros) {
        perror("malloc");
        return 1;
    }

    DIR *dir = opendir(diretorio);
    if (!dir) {
        perror("opendir");
        return 1;
    }

    struct dirent *entrada;
    while ((entrada = readdir(dir)) != NULL) {
        char *nome = entrada->d_name;
        int len = strlen(nome);
        if (len > 4 && strcmp(nome + len - 4, ".log") == 0) {
            if (total_ficheiros == capacidade) {
                capacidade *= 2;
                ficheiros = realloc(ficheiros, capacidade * sizeof(char *));
                if (!ficheiros) {
                    perror("realloc");
                    closedir(dir);
                    return 1;
                }
            }
            char caminho[512];
            snprintf(caminho, sizeof(caminho), "%s/%s", diretorio, nome);
            ficheiros[total_ficheiros++] = strdup(caminho);
            if (verbose)
                printf("Encontrei: %s\n", ficheiros[total_ficheiros - 1]);
        }
    }
    closedir(dir);

    printf("Total de ficheiros .log encontrados: %d\n", total_ficheiros);

    if (total_ficheiros == 0) {
        printf("Nenhum ficheiro .log encontrado.\n");
        return 0;
    }

    if (num_processos > total_ficheiros) {
        num_processos = total_ficheiros;
        printf("Ajustado para %d processo(s)\n", num_processos);
    }

    int fd[2];
    if (pipe(fd) < 0) {
        perror("pipe");
        return 1;
    }

    int ficheiros_por_processo = total_ficheiros / num_processos;

    for (int i = 0; i < num_processos; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            return 1;
        }

        if (pid == 0) {
            close(fd[0]);
            int inicio = i * ficheiros_por_processo;
            int fim = (i == num_processos - 1)
                          ? total_ficheiros
                          : inicio + ficheiros_por_processo;
            run_worker_pipe(ficheiros, inicio, fim, fd[1], verbose);
            return 0;
        }
    }

    close(fd[1]); // CRÍTICO: o pai fecha a sua ponta de escrita antes de ler

    WorkerResult total = {0};
    WorkerResult result;
    
    // AQUI: read alterado para readn
    while (readn(fd[0], &result, sizeof(result)) > 0) {
        printf("[PAI] Recebi dados do Filho %d\n", result.pid);
        total.total_lines    += result.total_lines;
        total.count_debug    += result.count_debug;
        total.count_info     += result.count_info;
        total.count_warn     += result.count_warn;
        total.count_error    += result.count_error;
        total.count_critical += result.count_critical;
        total.count_4xx      += result.count_4xx;
        total.count_5xx      += result.count_5xx;
    }
    close(fd[0]);

    for (int i = 0; i < num_processos; i++)
        wait(NULL);

    printf("\n=== RELATORIO FINAL ===\n");
    printf("Total de linhas : %ld\n", total.total_lines);
    printf("DEBUG           : %ld\n", total.count_debug);
    printf("INFO            : %ld\n", total.count_info);
    printf("WARNINGS        : %ld\n", total.count_warn);
    printf("ERRORS          : %ld\n", total.count_error);
    printf("CRITICAL        : %ld\n", total.count_critical);
    printf("4xx             : %ld\n", total.count_4xx);
    printf("5xx             : %ld\n", total.count_5xx);

    for (int i = 0; i < total_ficheiros; i++)
        free(ficheiros[i]);
    free(ficheiros);

    return 0;
}