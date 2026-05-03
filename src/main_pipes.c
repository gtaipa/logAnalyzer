#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ipc.h"
#include "parser.h"

void run_worker_pipe(char **ficheiros, int inicio, int fim, int pipe_fd_write, int verbose);

static void libertar_ficheiros(char **ficheiros, int total_ficheiros) {
    // Libertar cada string antes do vetor evita fugas de memoria no processo pai.
    if (ficheiros == NULL) {
        return;
    }

    for (int i = 0; i < total_ficheiros; i++) {
        free(ficheiros[i]);
    }

    free(ficheiros);
}

static int converter_num_processos(const char *texto) {
    char *fim = NULL;

    // Validar o numero de processos impede divisao por zero e argumentos parcialmente invalidos.
    errno = 0;
    long valor = strtol(texto, &fim, 10);
    if (errno != 0 || fim == texto || *fim != '\0' || valor <= 0) {
        fprintf(stderr, "Numero de processos invalido: %s\n", texto);
        exit(1);
    }

    return (int)valor;
}

static void acumular_resultado(WorkerResult *total, const WorkerResult *result) {
    // Somar no pai centraliza os resultados porque os filhos nao partilham memoria apos fork().
    total->total_lines += result->total_lines;
    total->count_debug += result->count_debug;
    total->count_info += result->count_info;
    total->count_warn += result->count_warn;
    total->count_error += result->count_error;
    total->count_critical += result->count_critical;
    total->count_4xx += result->count_4xx;
    total->count_5xx += result->count_5xx;
}

static void imprimir_relatorio(const WorkerResult *total) {
    // Mostrar o relatorio apenas depois da leitura dos pipes garante que todos os dados foram agregados.
    printf("\n=== RELATORIO FINAL ===\n");
    printf("Total de linhas : %ld\n", total->total_lines);
    printf("DEBUG           : %ld\n", total->count_debug);
    printf("INFO            : %ld\n", total->count_info);
    printf("WARNINGS        : %ld\n", total->count_warn);
    printf("ERRORS          : %ld\n", total->count_error);
    printf("CRITICAL        : %ld\n", total->count_critical);
    printf("4xx             : %ld\n", total->count_4xx);
    printf("5xx             : %ld\n", total->count_5xx);
}

int main(int argc, char *argv[]) {
    // 1. Validar argumentos no processo principal antes de criar qualquer recurso POSIX.
    if (argc < 4) {
        printf("Uso: %s <diretorio> <num_processos> <modo> [--verbose]\n", argv[0]);
        exit(1);
    }

    char *diretorio = argv[1];
    int num_processos = converter_num_processos(argv[2]);
    char *modo = argv[3];

    // 2. Interpretar flags opcionais antes do trabalho pesado para manter a configuracao imutavel.
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

    // 3. Configurar o parser uma unica vez no pai para que os filhos herdem esse estado apos fork().
    if (parser_set_mode_from_string(modo) != 0) {
        fprintf(stderr, "Modo invalido: %s (use security|performance|traffic|full)\n", modo);
        exit(1);
    }

    // 4. Reservar memoria dinamica para guardar os caminhos dos ficheiros encontrados no diretorio.
    int capacidade = 10;
    int total_ficheiros = 0;
    char **ficheiros = malloc((size_t)capacidade * sizeof(char *));
    if (ficheiros == NULL) {
        perror("malloc");
        exit(1);
    }

    // 5. Abrir o diretorio no pai porque a descoberta de ficheiros acontece antes da divisao por workers.
    DIR *dir = opendir(diretorio);
    if (dir == NULL) {
        perror("opendir");
        libertar_ficheiros(ficheiros, total_ficheiros);
        exit(1);
    }

    // 6. Percorrer o diretorio e guardar apenas ficheiros com extensao .log para processamento.
    struct dirent *entrada;
    errno = 0;
    while ((entrada = readdir(dir)) != NULL) {
        char *nome = entrada->d_name;
        size_t len = strlen(nome);

        if (len <= 4 || strcmp(nome + len - 4, ".log") != 0) {
            continue;
        }

        // 7. Aumentar o vetor antes de inserir novo caminho para nao escrever fora dos limites.
        if (total_ficheiros == capacidade) {
            capacidade *= 2;
            char **novo = realloc(ficheiros, (size_t)capacidade * sizeof(char *));
            if (novo == NULL) {
                perror("realloc");
                if (closedir(dir) == -1) {
                    perror("closedir");
                }
                libertar_ficheiros(ficheiros, total_ficheiros);
                exit(1);
            }
            ficheiros = novo;
        }

        // 8. Construir o caminho completo para que cada worker consiga abrir o ficheiro correto.
        char caminho[512];
        int escritos = snprintf(caminho, sizeof(caminho), "%s/%s", diretorio, nome);
        if (escritos < 0 || (size_t)escritos >= sizeof(caminho)) {
            fprintf(stderr, "Caminho demasiado longo: %s/%s\n", diretorio, nome);
            if (closedir(dir) == -1) {
                perror("closedir");
            }
            libertar_ficheiros(ficheiros, total_ficheiros);
            exit(1);
        }

        ficheiros[total_ficheiros] = strdup(caminho);
        if (ficheiros[total_ficheiros] == NULL) {
            perror("strdup");
            if (closedir(dir) == -1) {
                perror("closedir");
            }
            libertar_ficheiros(ficheiros, total_ficheiros);
            exit(1);
        }

        if (verbose) {
            printf("Encontrei: %s\n", ficheiros[total_ficheiros]);
        }
        total_ficheiros++;
    }

    // 9. Verificar erros de readdir e fechar o diretorio assim que a descoberta termina.
    if (errno != 0) {
        perror("readdir");
        if (closedir(dir) == -1) {
            perror("closedir");
        }
        libertar_ficheiros(ficheiros, total_ficheiros);
        exit(1);
    }

    if (closedir(dir) == -1) {
        perror("closedir");
        libertar_ficheiros(ficheiros, total_ficheiros);
        exit(1);
    }

    printf("Total de ficheiros .log encontrados: %d\n", total_ficheiros);

    // 10. Terminar sem criar filhos quando nao ha trabalho para distribuir.
    if (total_ficheiros == 0) {
        printf("Nenhum ficheiro .log encontrado.\n");
        libertar_ficheiros(ficheiros, total_ficheiros);
        exit(0);
    }

    // 11. Limitar workers ao numero de ficheiros evita processos sem trabalho e simplifica a distribuicao.
    if (num_processos > total_ficheiros) {
        num_processos = total_ficheiros;
        printf("Ajustado para %d processo(s)\n", num_processos);
    }

    // 12. Guardar PIDs e descritores de leitura permite ao pai ler resultados e recolher estados corretamente.
    pid_t *pids = malloc((size_t)num_processos * sizeof(pid_t));
    if (pids == NULL) {
        perror("malloc");
        libertar_ficheiros(ficheiros, total_ficheiros);
        exit(1);
    }

    int *pipes_leitura = malloc((size_t)num_processos * sizeof(int));
    if (pipes_leitura == NULL) {
        perror("malloc");
        free(pids);
        libertar_ficheiros(ficheiros, total_ficheiros);
        exit(1);
    }

    for (int i = 0; i < num_processos; i++) {
        pipes_leitura[i] = -1;
    }

    int ficheiros_por_processo = total_ficheiros / num_processos;

    // 13. Esvaziar buffers de stdio antes do fork evita que os filhos repitam mensagens ja impressas pelo pai.
    if (fflush(NULL) == EOF) {
        perror("fflush");
        free(pipes_leitura);
        free(pids);
        libertar_ficheiros(ficheiros, total_ficheiros);
        exit(1);
    }

    for (int i = 0; i < num_processos; i++) {
        int fd[2];
        pid_t pid;

        // 14. Criar o pipe anonimo antes do fork para pai e filho herdarem os mesmos descritores.
        if (pipe(fd) == -1) {
            perror("pipe");
            free(pipes_leitura);
            free(pids);
            libertar_ficheiros(ficheiros, total_ficheiros);
            exit(1);
        }

        // 15. Criar o processo com o padrao POSIX classico e abortar perante erro de fork().
        if ((pid = fork()) == -1) {
            perror("fork");
            if (close(fd[0]) == -1) {
                perror("close");
            }
            if (close(fd[1]) == -1) {
                perror("close");
            }
            free(pipes_leitura);
            free(pids);
            libertar_ficheiros(ficheiros, total_ficheiros);
            exit(1);
        }

        if (pid == 0) {
            // 16. No filho, fechar imediatamente a extremidade de leitura porque o worker apenas escreve.
            if (close(fd[0]) == -1) {
                perror("close");
                exit(1);
            }

            // 17. No filho, fechar leituras herdadas de pipes anteriores para manter a tabela de descritores limpa.
            for (int j = 0; j < i; j++) {
                if (pipes_leitura[j] != -1 && close(pipes_leitura[j]) == -1) {
                    perror("close");
                    exit(1);
                }
            }

            // 18. Calcular o intervalo de ficheiros deste worker antes de delegar o processamento.
            int inicio = i * ficheiros_por_processo;
            int fim = (i == num_processos - 1) ? total_ficheiros : inicio + ficheiros_por_processo;

            run_worker_pipe(ficheiros, inicio, fim, fd[1], verbose);
            exit(0);
        }

        // 19. No pai, fechar imediatamente a extremidade de escrita porque o pai apenas le resultados.
        if (close(fd[1]) == -1) {
            perror("close");
            if (close(fd[0]) == -1) {
                perror("close");
            }
            free(pipes_leitura);
            free(pids);
            libertar_ficheiros(ficheiros, total_ficheiros);
            exit(1);
        }

        pids[i] = pid;
        pipes_leitura[i] = fd[0];
    }

    WorkerResult total = {0};

    // 20. Ler um WorkerResult completo de cada pipe usando readn para evitar leituras parciais.
    for (int i = 0; i < num_processos; i++) {
        WorkerResult result;
        ssize_t bytes_lidos = readn(pipes_leitura[i], &result, sizeof(result));
        if (bytes_lidos == -1) {
            perror("readn");
            exit(1);
        }
        if (bytes_lidos != (ssize_t)sizeof(result)) {
            fprintf(stderr, "Erro: leitura incompleta do worker %d (%zd de %zu bytes)\n",
                    i, bytes_lidos, sizeof(result));
            exit(1);
        }

        printf("[PAI] Recebi dados do Filho %d\n", result.pid);
        acumular_resultado(&total, &result);

        // 21. Fechar a extremidade de leitura apos consumir o resultado liberta o descritor do pipe.
        if (close(pipes_leitura[i]) == -1) {
            perror("close");
            exit(1);
        }
        pipes_leitura[i] = -1;
    }

    // 22. Recolher cada filho com waitpid e interpretar o estado com as macros POSIX adequadas.
    for (int i = 0; i < num_processos; i++) {
        int status;
        if (waitpid(pids[i], &status, 0) == -1) {
            perror("waitpid");
            exit(1);
        }

        if (WIFEXITED(status)) {
            int codigo = WEXITSTATUS(status);
            if (codigo != 0) {
                fprintf(stderr, "[PAI] Filho %d terminou com codigo %d\n", pids[i], codigo);
                exit(1);
            }
        } else {
            fprintf(stderr, "[PAI] Filho %d nao terminou por exit normal\n", pids[i]);
            exit(1);
        }
    }

    imprimir_relatorio(&total);

    // 23. Libertar memoria reservada pelo pai antes de terminar o processo principal.
    free(pipes_leitura);
    free(pids);
    libertar_ficheiros(ficheiros, total_ficheiros);

    exit(0);
}
