#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/wait.h>

#include "worker.h"
#include "ipc.h"

int main(int argc, char *argv[]) {

    /* Verificar argumentos obrigatórios */
    if (argc < 4) {
        printf("Uso: %s <diretorio> <num_processos> <modo> [--verbose]\n", argv[0]);
        printf("Exemplo: %s datasets/ 4 security --verbose\n", argv[0]);
        exit(1);
    }

    char *diretorio   = argv[1];
    int num_processos = atoi(argv[2]);
    char *modo        = argv[3];

    int verbose = 0;
    for (int i = 4; i < argc; i++) {
        if (strcmp(argv[i], "--verbose") == 0) verbose = 1;
    }

    printf("Diretorio: %s\n", diretorio);
    printf("Processos: %d\n", num_processos);
    printf("Modo: %s\n", modo);
    printf("Verbose: %s\n", verbose ? "sim" : "nao");

    /* ---- DESCOBRIR FICHEIROS .log ---- */
    int capacidade = 10;
    int total_ficheiros = 0;
    char **ficheiros = malloc(capacidade * sizeof(char *));
    if (ficheiros == NULL) { perror("malloc"); exit(1); }

    DIR *dir = opendir(diretorio);
    if (dir == NULL) { perror("opendir"); exit(1); }

    struct dirent *entrada;
    while ((entrada = readdir(dir)) != NULL) {
        char *nome = entrada->d_name;
        int len = strlen(nome);

        if (len > 4 && strcmp(nome + len - 4, ".log") == 0) {
            if (total_ficheiros == capacidade) {
                capacidade *= 2;
                ficheiros = realloc(ficheiros, capacidade * sizeof(char *));
                if (ficheiros == NULL) { perror("realloc"); exit(1); }
            }
            char caminho[512];
            snprintf(caminho, sizeof(caminho), "%s/%s", diretorio, nome);
            ficheiros[total_ficheiros++] = strdup(caminho);
            if (verbose) printf("Encontrei: %s\n", ficheiros[total_ficheiros - 1]);
        }
    }
    closedir(dir);

    printf("Total de ficheiros .log encontrados: %d\n", total_ficheiros);

    if (total_ficheiros == 0) {
        printf("Nenhum ficheiro .log encontrado em %s\n", diretorio);
        exit(0);
    }

    /* Ajustar número de processos se houver menos ficheiros */
    if (num_processos > total_ficheiros) {
        num_processos = total_ficheiros;
        printf("Ajustado para %d processo(s)\n", num_processos);
    }

    /* ---- CRIAR PIPE PARA IPC ---- */
    int pipe_fd[2];
    if (pipe(pipe_fd) < 0) {
        perror("pipe");
        exit(1);
    }

    /* ---- CRIAR OS PROCESSOS FILHO ---- */
    int ficheiros_por_processo = total_ficheiros / num_processos;

    for (int i = 0; i < num_processos; i++) {
        pid_t pid = fork();

        if (pid < 0) {
            perror("fork");
            exit(1);

        } else if (pid == 0) {
            /* ---- FILHO ---- */
            close(pipe_fd[0]);  /* fecha leitura no filho */

            int inicio = i * ficheiros_por_processo;
            int fim    = (i == num_processos - 1) ? total_ficheiros
                                                   : inicio + ficheiros_por_processo;

            run_worker(ficheiros, inicio, fim, pipe_fd[1], verbose);
            close(pipe_fd[1]);
            exit(0);
        }
        /* pai continua o loop */
    }

    /* O pai não escreve no pipe */
    close(pipe_fd[1]);

    /* Ler resultados em tempo real dos filhos */
    WorkerResult resultado;
    WorkerResult total = {0};

    while (1) {
        ssize_t lidos = readn(pipe_fd[0], &resultado, sizeof(resultado));
        if (lidos < 0) {
            perror("readn");
            break;
        }
        if (lidos == 0) {
            /* EOF: todos os filhos fecharam a extremidade de escrita */
            break;
        }
        if (lidos != sizeof(resultado)) {
            fprintf(stderr, "[Pai] Leu %zd bytes incompletos do pipe (esperado %zu)\n", lidos, sizeof(resultado));
            break;
        }

        printf("[Pai] Recebi do filho %d: LINHAS=%ld DEBUG=%ld INFO=%ld WARNINGS=%ld ERRORS=%ld CRITICAL=%ld 4xx=%ld 5xx=%ld TOP_IP=%s\n",
               resultado.pid,
               resultado.total_lines,
               resultado.count_debug,
               resultado.count_info,
               resultado.count_warn,
               resultado.count_error,
               resultado.count_critical,
               resultado.count_4xx,
               resultado.count_5xx,
               resultado.top_ip);

        total.total_lines    += resultado.total_lines;
        total.count_debug    += resultado.count_debug;
        total.count_info     += resultado.count_info;
        total.count_warn     += resultado.count_warn;
        total.count_error    += resultado.count_error;
        total.count_critical += resultado.count_critical;
        total.count_4xx      += resultado.count_4xx;
        total.count_5xx      += resultado.count_5xx;
    }

    close(pipe_fd[0]);

    /* ---- PAI ESPERA PELOS FILHOS ---- */
    for (int i = 0; i < num_processos; i++) {
        wait(NULL);
    }

    printf("\n[Pai] Todos os workers terminaram!\n");
    printf("[Pai] Agregado: LINHAS=%ld DEBUG=%ld INFO=%ld WARNINGS=%ld ERRORS=%ld CRITICAL=%ld 4xx=%ld 5xx=%ld\n",
           total.total_lines,
           total.count_debug,
           total.count_info,
           total.count_warn,
           total.count_error,
           total.count_critical,
           total.count_4xx,
           total.count_5xx);

    printf("\n[Pai] Todos os workers terminaram!\n");

    /* Libertar memória */
    for (int i = 0; i < total_ficheiros; i++) free(ficheiros[i]);
    free(ficheiros);

    return 0;
}