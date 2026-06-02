/**
 * @file main_basic.c
 * @brief Processo PAI - Arquitectura Multi-Processo Básica (sem IPC)
 * @author Implementação do Analisador de Logs
 * @version 1.0
 * @date 2026
 * 
 * @details
 * Implementação da Fase 1B (15% do projeto).
 * Estrutura multi-processo clássica sem mecanismos de IPC durante execução.
 * 
 * FLUXO:
 *  1. Pai descobre ficheiros .log/.json
 *  2. Divide lista entre N processos filho
 *  3. Cria filhos com fork()
 *  4. Aguarda conclusão com waitpid()
 *  5. Cada filho processa de forma independente
 *  6. Filhos escrevem resultados em ficheiros separados
 * 
 * USO: ./logAnalyzer_basic <diretorio> <num_processos> <modo> [--verbose]
 */

#include <dirent.h> // Incluir header para navegação de diretórios
#include <errno.h> // Incluir header com variável de erro
#include <fcntl.h> // Incluir header com flags de ficheiro
#include <stdio.h> // Incluir header com funções de I/O padrão
#include <stdlib.h> // Incluir header com funções gerais
#include <string.h> // Incluir header com funções de string
#include <sys/stat.h> // Incluir header com estruturas de ficheiro
#include <sys/types.h> // Incluir header com tipos de dados
#include <sys/wait.h> // Incluir header com funções de espera
#include <unistd.h> // Incluir header com funções POSIX

#include "parser.h" // Incluir header com funções de parsing
#include "posix_io.h" // Incluir header com funções de I/O POSIX

#define BUF_SIZE        4096 // Definir tamanho do buffer de leitura
#define LINE_MAX_BASIC  512 // Definir tamanho máximo de linha

/**
 * @brief Processa um ficheiro de log e acumula métricas
 * 
 * @details
 * Abre ficheiro, lê byte a byte, acumula linhas e parseia.
 * Utiliza exclusivamente chamadas POSIX (open, read, close).
 * 
 * @param caminho Caminho completo do ficheiro
 * @param m Estrutura de métricas a preencher
 * @param verbose Flag para output detalhado
 * @param pid PID do processo (para mensagens)
 * @return 0 em sucesso, -1 em erro de abertura
 */
static int processar_ficheiro(const char *caminho, Metrics *m, int verbose, pid_t pid) { // Função para processar ficheiro
    int fd = open(caminho, O_RDONLY); // Abrir ficheiro para leitura
    if (fd < 0) { // Se erro ao abrir
        perror("open"); // Imprimir erro
        return -1; // Retornar erro
    }

    if (verbose) // Se modo verbose
        posix_writef(STDOUT_FILENO, "[PID %d] A processar: %s\n", (int)pid, caminho); // Imprimir mensagem

    char buf[BUF_SIZE]; // Declarar buffer de leitura
    char linha[LINE_MAX_BASIC]; // Declarar linha local
    int  len = 0; // Inicializar comprimento da linha

    LogFormat fmt = FORMAT_UNKNOWN; // Inicializar formato

    ssize_t n; // Declarar bytes lidos

    while ((n = read(fd, buf, BUF_SIZE)) > 0) { // Ciclo de leitura
        for (ssize_t b = 0; b < n; b++) { // Ciclo para cada byte
            char c = buf[b]; // Obter caracter

            if (c == '\n') { // Se newline
                if (len > 0) { // Se linha tem conteúdo
                    linha[len] = '\0'; // Terminar string

                    if (fmt == FORMAT_UNKNOWN) // Se formato desconhecido
                        fmt = detect_format(linha); // Detetar formato

                    LogEntry entry; // Declarar entrada
                    if (parse_line(linha, fmt, &entry) == 0) // Se parsing ok
                        update_metrics(m, &entry); // Atualizar métricas

                    len = 0; // Resetar comprimento
                }
            } else if (c != '\r') { // Se não carriage return
                if (len < LINE_MAX_BASIC - 1) // Se espaço na linha
                    linha[len++] = c; // Adicionar caracter
            }
        }
    }

    if (n < 0) // Se erro de leitura
        perror("read"); // Imprimir erro

    if (len > 0) { // Se linha no fim sem newline
        linha[len] = '\0'; // Terminar string
        if (fmt == FORMAT_UNKNOWN) // Se formato desconhecido
            fmt = detect_format(linha); // Detetar formato
        LogEntry entry; // Declarar entrada
        if (parse_line(linha, fmt, &entry) == 0) // Se parsing ok
            update_metrics(m, &entry); // Atualizar métricas
    }

    if (close(fd) < 0) // Se erro ao fechar
        perror("close"); // Imprimir erro

    return 0; // Retornar sucesso
}

/**
 * @brief Escreve resultados em ficheiro results_<pid>.txt
 * 
 * @details
 * Cria ficheiro de output com formato especificado no enunciado.
 * Uma linha por ficheiro processado com estatísticas.
 * 
 * @param pid PID do processo (para nome do ficheiro)
 * @param ficheiros Array de caminhos processados
 * @param total_ficheiros Número de ficheiros
 * @param metricas_por_ficheiro Array de métricas por ficheiro
 */
static void escrever_resultados(pid_t pid, // Função para escrever resultados
                                char **ficheiros, int total_ficheiros,
                                const Metrics *metricas_por_ficheiro) {
    char caminho_resultado[64]; // Declarar buffer para caminho
    snprintf(caminho_resultado, sizeof(caminho_resultado), "results_%d.txt", (int)pid); // Construir nome

    int fd = open(caminho_resultado, O_WRONLY | O_CREAT | O_TRUNC, 0644); // Abrir ficheiro para escrita
    if (fd < 0) { // Se erro ao abrir
        perror("open results"); // Imprimir erro
        return; // Retornar
    }

    for (int i = 0; i < total_ficheiros; i++) { // Ciclo para cada ficheiro
        const char *nome = strrchr(ficheiros[i], '/'); // Procurar último /
        nome = (nome != NULL) ? nome + 1 : ficheiros[i]; // Usar nome base

        char linha[512]; // Declarar linha
        int len = snprintf(linha, sizeof(linha), // Construir linha
                           "PID:%d;FICHEIRO:%s;LINHAS:%ld;ERRORS:%ld;WARNINGS:%ld\n",
                           (int)pid,
                           nome,
                           metricas_por_ficheiro[i].total_lines,
                           metricas_por_ficheiro[i].count_error,
                           metricas_por_ficheiro[i].count_warn);

        if (write(fd, linha, (size_t)len) < 0) // Se erro ao escrever
            perror("write results"); // Imprimir erro
    }

    if (close(fd) < 0) // Se erro ao fechar
        perror("close results"); // Imprimir erro

    posix_writef(STDOUT_FILENO, // Imprimir mensagem
                 "[PID %d] Resultados escritos em %s\n", (int)pid, caminho_resultado);
}

/**
 * @brief Ponto de entrada do processo filho
 * 
 * @details
 * Cada filho processa sua lista de ficheiros de forma independente.
 * Escreve resultados em ficheiro próprio. Chama exit() no fim.
 * 
 * @param ficheiros Array de ficheiros a processar
 * @param total_ficheiros Número de ficheiros
 * @param verbose Flag para output detalhado
 */
static void run_worker_basic(char **ficheiros, int total_ficheiros, int verbose) { // Função do worker
    pid_t pid = getpid(); // Obter PID

    Metrics *metricas = calloc((size_t)total_ficheiros, sizeof(Metrics)); // Alocar métricas
    if (!metricas) { // Se erro
        perror("calloc"); // Imprimir erro
        exit(EXIT_FAILURE); // Sair com erro
    }

    for (int i = 0; i < total_ficheiros; i++) { // Ciclo para cada ficheiro
        init_metrics(&metricas[i]); // Inicializar métricas
        processar_ficheiro(ficheiros[i], &metricas[i], verbose, pid); // Processar
    }

    escrever_resultados(pid, ficheiros, total_ficheiros, metricas); // Escrever resultados

    free(metricas); // Libertar memória
    exit(0); // Sair com sucesso
}

/**
 * @brief Função principal do processo PAI
 * 
 * @details
 * Descobre ficheiros, divide entre N filhos, aguarda conclusão.
 * 
 * @param argc Número de argumentos
 * @param argv Argumentos: programa, diretório, num_processos, modo, [--verbose]
 * @return 0 em sucesso, 1 em erro
 */
int main(int argc, char *argv[]) { // Função principal
    if (argc < 4) { // Se argumentos insuficientes
        printf("Uso: %s <diretorio> <num_processos> <modo> [--verbose]\n", argv[0]); // Imprimir uso
        exit(1); // Sair com erro
    }

    char *diretorio     = argv[1]; // Obter diretório
    int   num_processos = atoi(argv[2]); // Converter número de processos
    char *modo          = argv[3]; // Obter modo

    int verbose = 0; // Inicializar verbose
    for (int i = 4; i < argc; i++) { // Ciclo para argumentos
        if (strcmp(argv[i], "--verbose") == 0) verbose = 1; // Se verbose
    }

    if (parser_set_mode_from_string(modo) != 0) { // Se erro no modo
        fprintf(stderr, "Modo invalido: %s\n", modo); // Imprimir erro
        exit(1); // Sair com erro
    }

    int   capacidade      = 10; // Inicializar capacidade
    int   total_ficheiros = 0; // Inicializar total
    char **ficheiros      = malloc((size_t)capacidade * sizeof(char *)); // Alocar array

    DIR *dir = opendir(diretorio); // Abrir diretório
    if (dir == NULL) { perror("opendir"); exit(1); } // Se erro, sair

    struct dirent *entrada; // Declarar entrada
    while ((entrada = readdir(dir)) != NULL) { // Ciclo enquanto há entradas
        char *nome = entrada->d_name; // Obter nome
        size_t len = strlen(nome); // Obter comprimento

        if (!((len > 4 && strcmp(nome + len - 4, ".log") == 0) || // Se not .log
              (len > 5 && strcmp(nome + len - 5, ".json") == 0))) { // Se not .json
            continue; // Continuar
        }

        if (total_ficheiros == capacidade) { // Se capacidade atingida
            capacidade *= 2; // Duplicar capacidade
            ficheiros = realloc(ficheiros, (size_t)capacidade * sizeof(char *)); // Realocar
        }

        char caminho[512]; // Declarar caminho
        snprintf(caminho, sizeof(caminho), "%s/%s", diretorio, nome); // Construir caminho
        ficheiros[total_ficheiros++] = strdup(caminho); // Guardar cópia
    }
    closedir(dir); // Fechar diretório

    if (total_ficheiros == 0) { // Se sem ficheiros
        printf("Nenhum ficheiro encontrado.\n"); // Imprimir mensagem
        free(ficheiros); // Libertar memória
        exit(0); // Sair
    }

    printf("Ficheiros encontrados: %d | Workers: %d | Modo: %s\n\n", // Imprimir resumo
           total_ficheiros, num_processos, modo);

    int ficheiros_por_worker = total_ficheiros / num_processos; // Calcular por worker
    int ficheiros_extra = total_ficheiros % num_processos; // Calcular extra

    fflush(NULL); // Esvaziar buffers

    pid_t *pids = malloc((size_t)num_processos * sizeof(pid_t)); // Alocar PIDs

    for (int i = 0; i < num_processos; i++) { // Ciclo para criar filhos
        int inicio = i * ficheiros_por_worker + (i < ficheiros_extra ? i : ficheiros_extra); // Calcular início
        int fim = inicio + ficheiros_por_worker + (i < ficheiros_extra ? 1 : 0); // Calcular fim

        pid_t pid = fork(); // Criar processo filho
        if (pid < 0) { perror("fork"); exit(1); } // Se erro, sair

        if (pid == 0) { // Se processo filho
            run_worker_basic(&ficheiros[inicio], fim - inicio, verbose); // Executar worker
            exit(0); // Sair
        }

        pids[i] = pid; // Guardar PID
    }

    for (int i = 0; i < num_processos; i++) { // Ciclo para cada worker
        int status; // Declarar status
        waitpid(pids[i], &status, 0); // Esperar worker
        if (WIFEXITED(status) && WEXITSTATUS(status) != 0) // Se terminou com erro
            fprintf(stderr, "Worker %d terminou com erro\n", i); // Imprimir erro
    }

    printf("\nTodos os processos terminaram.\n"); // Imprimir mensagem

    for (int i = 0; i < total_ficheiros; i++) free(ficheiros[i]); // Libertar strings
    free(ficheiros); // Libertar array
    free(pids); // Libertar PIDs

    return 0; // Retornar sucesso
}
