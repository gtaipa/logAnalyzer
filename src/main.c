#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/wait.h>

int main(int argc, char *argv[]) {

    if (argc < 4) {
        printf("O comando introduzido não conta com todos os argumentos obrigatorios.\n");
        printf("Exemplo: %s /var/log/ 4 security --verbose\n", argv[0]);
        exit(1);
    }

    char *diretorio   = argv[1];
    int num_processos = atoi(argv[2]);
    char *modo        = argv[3];

    int verbose = 0;
    for (int i = 4; i < argc; i++) {
        if (strcmp(argv[i], "--verbose") == 0) {
            verbose = 1;
        }
    }

    printf("Diretorio: %s\n", diretorio);
    printf("Processos: %d\n", num_processos);
    printf("Modo: %s\n", modo);
    printf("Verbose: %s\n", verbose ? "sim" : "nao");

    int capacidade = 10;
    int total_ficheiros = 0;
    char **ficheiros = malloc(capacidade * sizeof(char *));
    if (ficheiros == NULL) {
        perror("malloc");
        exit(1);
    }

    DIR *dir = opendir(diretorio);
    if (dir == NULL) {
        perror("opendir");
        exit(1);
    }

    struct dirent *entrada;
    while ((entrada = readdir(dir)) != NULL) {

        char *nome = entrada->d_name;
        int len = strlen(nome);

        if (len > 4 && strcmp(nome + len - 4, ".log") == 0) {

            if (total_ficheiros == capacidade) {
                capacidade *= 2;
                ficheiros = realloc(ficheiros, capacidade * sizeof(char *));
                if (ficheiros == NULL) {
                    perror("realloc");
                    exit(1);
                }
            }

            char caminho[512];
            snprintf(caminho, sizeof(caminho), "%s/%s", diretorio, nome);

            ficheiros[total_ficheiros] = strdup(caminho);
            total_ficheiros++;

            if (verbose) printf("Encontrei: %s\n", ficheiros[total_ficheiros - 1]);
        }
    }

    closedir(dir);

    printf("Total de ficheiros .log encontrados: %d\n", total_ficheiros);

    printf("\n--- Conteudo da lista ---\n");
    for (int i = 0; i < total_ficheiros; i++) {
        printf("ficheiros[%d] = %s\n", i, ficheiros[i]);
    }

    int ficheiros_por_processo = total_ficheiros / num_processos;

    for (int i = 0; i < num_processos; i++) {

        pid_t pid = fork();

        if (pid < 0) {
            perror("fork");
            exit(1);

        } else if (pid == 0) {
            int inicio = i * ficheiros_por_processo;
            int fim = (i == num_processos - 1) ? total_ficheiros : inicio + ficheiros_por_processo;

            printf("[Filho %d] Vou processar %d ficheiros\n", getpid(), fim - inicio);

            for (int j = inicio; j < fim; j++) {
                printf("[Filho %d] A processar: %s\n", getpid(), ficheiros[j]);
            }

            exit(0);
        }
    }

    for (int i = 0; i < num_processos; i++) {
        wait(NULL);
    }

    printf("\n[Pai] Todos os workers terminaram!\n");

    return 0;
}
