/**
 * @file main_basic.c
 * @brief Orquestrador multi-processo básico sem IPC durante execução.
 *
 * @details
 * Implementa a variante mais simples de paralelismo por processos: o pai divide a lista
 * de ficheiros de log entre N filhos e aguarda a conclusão de todos via waitpid(2). Não
 * existe comunicação entre processos durante o processamento — cada filho trabalha de
 * forma completamente independente e escreve os seus resultados num ficheiro próprio
 * chamado `results_<pid>.txt`.
 *
 * Esta ausência de IPC é intencional: simplifica o código e elimina o overhead de
 * sincronização, ao custo de não ter visibilidade do progresso em tempo real.
 *
 * Fluxo de execução:
 *  1. Descobrir ficheiros .log/.json no directório fornecido.
 *  2. Dividir a lista em N sublistas (distribuição por ficheiros, não por bytes).
 *  3. fork(2) × N — cada filho recebe a sua sublista e chama run_worker_basic().
 *  4. Pai aguarda todos os filhos com waitpid(2) e termina.
 *
 * Uso: ./logAnalyzer_basic <directorio> <num_processos> <modo> [--verbose]
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "parser.h"
#include "posix_io.h"

/** @brief Dimensão do buffer de leitura por invocação de read(2). */
#define BUF_SIZE        4096

/** @brief Comprimento máximo de uma linha de log aceite pelo parser. */
#define LINE_MAX_BASIC  512

/**
 * @brief Processa um ficheiro de log usando chamadas POSIX puras e acumula métricas.
 *
 * @details Abre o ficheiro com open(2) em modo só-leitura, lê em blocos de BUF_SIZE
 * bytes e reconstrói linhas num buffer local. Usa exclusivamente syscalls POSIX
 * (open, read, close) para evitar conflitos com os buffers internos do stdio após fork(2).
 *
 * @param caminho Caminho absoluto do ficheiro a processar.
 * @param m       Estrutura de métricas a actualizar.
 * @param verbose Se não-zero, imprime o caminho de cada ficheiro processado.
 * @param pid     PID do processo filho (para prefixo nas mensagens verbose).
 * @return 0 em caso de sucesso; -1 se open(2) falhar.
 */
static int processar_ficheiro(const char *caminho, Metrics *m, int verbose, pid_t pid) {
    int fd = open(caminho, O_RDONLY);
    if (fd < 0) {
        perror("open");
        return -1;
    }

    if (verbose)
        posix_writef(STDOUT_FILENO, "[PID %d] A processar: %s\n", (int)pid, caminho);

    char buf[BUF_SIZE];
    char linha[LINE_MAX_BASIC];
    int  len = 0;

    LogFormat fmt = FORMAT_UNKNOWN; /* inferido na primeira linha válida do ficheiro */

    ssize_t n;
    while ((n = read(fd, buf, BUF_SIZE)) > 0) {
        for (ssize_t b = 0; b < n; b++) {
            char c = buf[b];

            if (c == '\n') {
                if (len > 0) {
                    linha[len] = '\0';
                    if (fmt == FORMAT_UNKNOWN)
                        fmt = detect_format(linha);
                    LogEntry entry;
                    if (parse_line(linha, fmt, &entry) == 0)
                        update_metrics(m, &entry);
                    len = 0;
                }
            } else if (c != '\r') {
                /* Ignorar '\r' de terminadores CRLF (ficheiros Windows) */
                if (len < LINE_MAX_BASIC - 1)
                    linha[len++] = c;
            }
        }
    }

    if (n < 0)
        perror("read");

    /* Tratar última linha sem '\n' (ficheiro sem newline final) */
    if (len > 0) {
        linha[len] = '\0';
        if (fmt == FORMAT_UNKNOWN)
            fmt = detect_format(linha);
        LogEntry entry;
        if (parse_line(linha, fmt, &entry) == 0)
            update_metrics(m, &entry);
    }

    if (close(fd) < 0)
        perror("close");

    return 0;
}

/**
 * @brief Escreve os resultados do processo filho no ficheiro `results_<pid>.txt`.
 *
 * @details Cada filho cria o seu próprio ficheiro de resultados com nome único baseado
 * no PID, evitando colisões de escrita entre processos concorrentes sem necessidade de
 * locks ou IPC. O formato é uma linha por ficheiro processado.
 *
 * @param pid                   PID do processo filho (usado no nome do ficheiro).
 * @param ficheiros             Array de caminhos processados por este filho.
 * @param total_ficheiros       Número de ficheiros.
 * @param metricas_por_ficheiro Array paralelo de métricas, um elemento por ficheiro.
 */
static void escrever_resultados(pid_t pid,
                                char **ficheiros, int total_ficheiros,
                                const Metrics *metricas_por_ficheiro) {
    char caminho_resultado[64];
    snprintf(caminho_resultado, sizeof(caminho_resultado), "results_%d.txt", (int)pid);

    int fd = open(caminho_resultado, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        perror("open results");
        return;
    }

    for (int i = 0; i < total_ficheiros; i++) {
        /* Usar apenas o nome base do ficheiro (sem path) na linha de resultado */
        const char *nome = strrchr(ficheiros[i], '/');
        nome = (nome != NULL) ? nome + 1 : ficheiros[i];

        char linha[512];
        int len = snprintf(linha, sizeof(linha),
                           "PID:%d;FICHEIRO:%s;LINHAS:%ld;ERRORS:%ld;WARNINGS:%ld\n",
                           (int)pid,
                           nome,
                           metricas_por_ficheiro[i].total_lines,
                           metricas_por_ficheiro[i].count_error,
                           metricas_por_ficheiro[i].count_warn);

        if (write(fd, linha, (size_t)len) < 0)
            perror("write results");
    }

    if (close(fd) < 0)
        perror("close results");

    posix_writef(STDOUT_FILENO,
                 "[PID %d] Resultados escritos em %s\n", (int)pid, caminho_resultado);
}

/**
 * @brief Ponto de entrada de cada processo filho — processa a sublista de ficheiros atribuída.
 *
 * @details Aloca um array de Metrics proporcional ao número de ficheiros (um elemento
 * por ficheiro para granularidade por ficheiro nos resultados). Processa cada ficheiro
 * de forma sequencial e escreve os resultados antes de terminar com exit(0).
 *
 * @param ficheiros       Array de caminhos dos ficheiros a processar.
 * @param total_ficheiros Número de ficheiros nesta sublista.
 * @param verbose         Se não-zero, activa mensagens detalhadas.
 */
static void run_worker_basic(char **ficheiros, int total_ficheiros, int verbose) {
    pid_t pid = getpid();

    /*
     * calloc garante que todos os campos de Metrics ficam a zero sem necessidade de
     * init_metrics() explícita; porém chamamos init_metrics para consistência com
     * o resto do código que pode usar campos não inicializados por calloc (e.g., strings).
     */
    Metrics *metricas = calloc((size_t)total_ficheiros, sizeof(Metrics));
    if (!metricas) {
        perror("calloc");
        exit(EXIT_FAILURE);
    }

    for (int i = 0; i < total_ficheiros; i++) {
        init_metrics(&metricas[i]);
        processar_ficheiro(ficheiros[i], &metricas[i], verbose, pid);
    }

    escrever_resultados(pid, ficheiros, total_ficheiros, metricas);

    free(metricas);
    exit(0);
}

/**
 * @brief Ponto de entrada do processo pai — descobre ficheiros, cria filhos e aguarda.
 *
 * @details A distribuição de ficheiros entre N filhos usa divisão inteira com resto:
 * os primeiros (total % N) filhos recebem um ficheiro extra para equilibrar a carga.
 * fflush(NULL) antes do fork garante que os buffers de stdio não são duplicados nos
 * filhos, evitando que a mesma saída seja impressa múltiplas vezes.
 *
 * @param argc Número de argumentos na linha de comandos.
 * @param argv Argumentos: programa, directório, num_processos, modo, [--verbose].
 * @return 0 em caso de sucesso; 1 em erro.
 */
int main(int argc, char *argv[]) {
    if (argc < 4) {
        printf("Uso: %s <diretorio> <num_processos> <modo> [--verbose]\n", argv[0]);
        exit(1);
    }

    char *diretorio     = argv[1];
    int   num_processos = atoi(argv[2]);
    char *modo          = argv[3];

    int verbose = 0;
    for (int i = 4; i < argc; i++) {
        if (strcmp(argv[i], "--verbose") == 0) verbose = 1;
    }

    if (parser_set_mode_from_string(modo) != 0) {
        fprintf(stderr, "Modo invalido: %s\n", modo);
        exit(1);
    }

    int    capacidade      = 10;
    int    total_ficheiros = 0;
    char **ficheiros       = malloc((size_t)capacidade * sizeof(char *));

    DIR *dir = opendir(diretorio);
    if (dir == NULL) { perror("opendir"); exit(1); }

    struct dirent *entrada;
    while ((entrada = readdir(dir)) != NULL) {
        char  *nome = entrada->d_name;
        size_t len  = strlen(nome);

        /* Aceitar apenas ficheiros com extensão .log ou .json */
        if (!((len > 4 && strcmp(nome + len - 4, ".log")  == 0) ||
              (len > 5 && strcmp(nome + len - 5, ".json") == 0))) {
            continue;
        }

        if (total_ficheiros == capacidade) {
            capacidade *= 2;
            ficheiros = realloc(ficheiros, (size_t)capacidade * sizeof(char *));
        }

        char caminho[512];
        snprintf(caminho, sizeof(caminho), "%s/%s", diretorio, nome);
        ficheiros[total_ficheiros++] = strdup(caminho);
    }
    closedir(dir);

    if (total_ficheiros == 0) {
        printf("Nenhum ficheiro encontrado.\n");
        free(ficheiros);
        exit(0);
    }

    printf("Ficheiros encontrados: %d | Workers: %d | Modo: %s\n\n",
           total_ficheiros, num_processos, modo);

    int ficheiros_por_worker = total_ficheiros / num_processos;
    int ficheiros_extra      = total_ficheiros % num_processos;

    /*
     * fflush(NULL) antes do fork garante que quaisquer dados pendentes nos buffers
     * de stdio do pai não são duplicados nos filhos (o fork copia o estado dos buffers).
     */
    fflush(NULL);

    pid_t *pids = malloc((size_t)num_processos * sizeof(pid_t));

    for (int i = 0; i < num_processos; i++) {
        /* Cálculo do intervalo: os primeiros ficheiros_extra workers recebem +1 ficheiro */
        int inicio = i * ficheiros_por_worker + (i < ficheiros_extra ? i : ficheiros_extra);
        int fim    = inicio + ficheiros_por_worker + (i < ficheiros_extra ? 1 : 0);

        pid_t pid = fork();
        if (pid < 0) { perror("fork"); exit(1); }

        if (pid == 0) {
            run_worker_basic(&ficheiros[inicio], fim - inicio, verbose);
            exit(0); /* run_worker_basic chama exit() — linha de segurança */
        }

        pids[i] = pid;
    }

    for (int i = 0; i < num_processos; i++) {
        int status;
        waitpid(pids[i], &status, 0);
        if (WIFEXITED(status) && WEXITSTATUS(status) != 0)
            fprintf(stderr, "Worker %d terminou com erro\n", i);
    }

    printf("\nTodos os processos terminaram.\n");

    for (int i = 0; i < total_ficheiros; i++) free(ficheiros[i]);
    free(ficheiros);
    free(pids);

    return 0;
}
