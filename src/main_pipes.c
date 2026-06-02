/**
 * @file main_pipes.c
 * @brief Orquestrador multi-processo com comunicação via pipes POSIX e dashboard em tempo real.
 *
 * @details
 * O processo pai divide o espaço de endereçamento virtual (bytes totais de todos os
 * ficheiros) em N fatias iguais e cria N pipes + N processos filho. Cada filho processa
 * a sua fatia e envia mensagens ao pai via pipe; o pai usa select(2) para monitorizar
 * todos os pipes em simultâneo (multiplexagem assíncrona) e actualiza um dashboard
 * de barras de progresso no terminal.
 *
 * Fluxo de execução:
 *  1. Descobrir ficheiros .log/.json e calcular o total de bytes.
 *  2. Dividir [0, total_bytes) em N fatias.
 *  3. Criar N pipes; fork(2) × N — cada filho chama run_worker_pipe().
 *  4. Loop de select(): ler MSG_PROGRESSO e MSG_RESULTADO de cada pipe activo.
 *  5. Acumular resultados, aguardar todos os filhos e imprimir relatório final.
 *
 * Protocolo de mensagens (unidireccional: filho → pai):
 *  - MSG_PROGRESSO (1): int + ProgressUpdate
 *  - MSG_RESULTADO (2): int + WorkerResult
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

#include "ipc.h"
#include "parser.h"
#include "posix_io.h"
#include "worker.h"

/** @brief Tipo de mensagem de progresso enviada pelo filho. */
#define MSG_PROGRESSO  1

/** @brief Tipo de mensagem de resultado final enviada pelo filho. */
#define MSG_RESULTADO  2

/** @brief Número de caracteres '#' ou '.' na barra de progresso de cada worker. */
#define LARGURA_BARRA  20

/** @brief Dimensão do buffer de leitura interno dos workers. */
#define BUF_SIZE       4096

/* Declaração da função definida em worker_pipes.c */
void run_worker_pipe(char **ficheiros, int total_ficheiros, int pipe_fd_write,
                     int worker_index, off_t byte_inicio, off_t byte_fim, int verbose);

/**
 * @brief Redesenha o dashboard de barras de progresso no terminal.
 *
 * @details Usa sequências de escape ANSI para subir o cursor N linhas e apagar
 * tudo abaixo, reescrevendo as barras actualizadas. A sequência `\033[%dA`
 * sobe o cursor, `\033[J` apaga do cursor até ao fim do ecrã.
 *
 * @param progressos  Array com o estado de progresso de cada worker.
 * @param num_workers Número de workers (== número de linhas do dashboard).
 */
static void desenhar_dashboard(ProgressUpdate *progressos, int num_workers) {
    printf("\033[%dA", num_workers); /* subir cursor N linhas */
    printf("\033[J");                /* apagar tudo abaixo do cursor */

    for (int i = 0; i < num_workers; i++) {
        long feitas = progressos[i].bytes_done;
        long total  = progressos[i].bytes_total;

        int pct = (total > 0) ? (int)(feitas * 100 / total) : 0;
        if (pct > 100) pct = 100;

        char barra[LARGURA_BARRA + 1];
        int cheio = pct * LARGURA_BARRA / 100;
        for (int b = 0; b < LARGURA_BARRA; b++)
            barra[b] = (b < cheio) ? '#' : '.';
        barra[LARGURA_BARRA] = '\0';

        printf("Worker %2d [%s] %3d%% (%ld/%ld bytes)\n",
               i, barra, pct, feitas, total);
    }
    fflush(stdout);
}

/**
 * @brief Liberta a memória do array de caminhos de ficheiros.
 *
 * @param ficheiros       Array de strings alocadas com strdup.
 * @param total_ficheiros Número de elementos no array.
 */
static void libertar_ficheiros(char **ficheiros, int total_ficheiros) {
    if (ficheiros == NULL) return;
    for (int i = 0; i < total_ficheiros; i++) free(ficheiros[i]);
    free(ficheiros);
}

/**
 * @brief Calcula o total de bytes de todos os ficheiros em @p ficheiros.
 *
 * @param ficheiros       Array de caminhos absolutos.
 * @param total_ficheiros Número de ficheiros.
 * @return Soma dos tamanhos em bytes; 0 se nenhum ficheiro for acessível.
 */
static off_t obter_bytes_totais(char **ficheiros, int total_ficheiros) {
    off_t total = 0;
    struct stat st;
    for (int i = 0; i < total_ficheiros; i++) {
        if (stat(ficheiros[i], &st) == 0)
            total += st.st_size;
    }
    return total;
}

/**
 * @brief Converte uma string para número de processos com validação rigorosa.
 *
 * @details Usa strtol para detectar erros de conversão que atoi ignora
 * (texto extra após o número, overflow, valor zero ou negativo).
 *
 * @param texto String com o número de processos.
 * @return Número convertido (> 0); termina o processo em caso de erro.
 */
static int converter_num_processos(const char *texto) {
    char *fim = NULL;
    errno     = 0;
    long valor = strtol(texto, &fim, 10);
    if (errno != 0 || fim == texto || *fim != '\0' || valor <= 0) {
        posix_writef(STDERR_FILENO, "Numero de processos invalido: %s\n", texto);
        exit(1);
    }
    return (int)valor;
}

/**
 * @brief Funde o resultado de um worker na tabela global de métricas e IPs.
 *
 * @details Acumula contadores escalares e faz merge da tabela de top-10 IPs do worker
 * na tabela global (pesquisa linear + inserção ou incremento). Após cada merge, ordena
 * a tabela global por contagem decrescente via bubble sort e actualiza total->top_ips
 * com os 10 IPs mais frequentes.
 *
 * @param total            Resultado global acumulado (actualizado in-place).
 * @param r                Resultado do worker a fundir.
 * @param ip_list_global   Tabela global de IPs (até 256 entradas).
 * @param ip_count_global  Contagens correspondentes na tabela global.
 * @param ip_num_global    Ponteiro para o número actual de IPs na tabela global.
 */
static void acumular_resultado(WorkerResult *total, const WorkerResult *r,
                               char ip_list_global[256][IP_LEN], long ip_count_global[256], int *ip_num_global) {
    total->total_lines    += r->total_lines;
    total->count_debug    += r->count_debug;
    total->count_info     += r->count_info;
    total->count_warn     += r->count_warn;
    total->count_error    += r->count_error;
    total->count_critical += r->count_critical;
    total->count_4xx      += r->count_4xx;
    total->count_5xx      += r->count_5xx;

    /* Fundir os top-10 IPs deste worker na tabela global */
    for (int k = 0; k < 10; k++) {
        if (r->top_ips[k][0] == '\0' || r->top_ips_counts[k] <= 0) continue;

        int found = -1;
        for (int i = 0; i < *ip_num_global; i++) {
            if (strcmp(ip_list_global[i], r->top_ips[k]) == 0) {
                found = i;
                break;
            }
        }
        if (found == -1 && *ip_num_global < 256) {
            strncpy(ip_list_global[*ip_num_global], r->top_ips[k], IP_LEN - 1);
            ip_list_global[*ip_num_global][IP_LEN - 1] = '\0';
            ip_count_global[*ip_num_global]             = r->top_ips_counts[k];
            (*ip_num_global)++;
        } else if (found >= 0) {
            ip_count_global[found] += r->top_ips_counts[k];
        }
    }

    for (int i = 0; i < total->num_alerts && total->num_alerts < MAX_ALERTS; i++) {
        strncpy(total->alerts[total->num_alerts], r->alerts[i], ALERT_LEN - 1);
        total->alerts[total->num_alerts][ALERT_LEN - 1] = '\0';
        total->num_alerts++;
    }

    /* Ordenar tabela global por contagem decrescente e actualizar top_ips */
    for (int i = 0; i < *ip_num_global - 1; i++) {
        for (int j = 0; j < *ip_num_global - i - 1; j++) {
            if (ip_count_global[j] < ip_count_global[j + 1]) {
                long tmp_count      = ip_count_global[j];
                ip_count_global[j]  = ip_count_global[j + 1];
                ip_count_global[j + 1] = tmp_count;

                char tmp_ip[IP_LEN];
                strncpy(tmp_ip,              ip_list_global[j],     IP_LEN);
                strncpy(ip_list_global[j],   ip_list_global[j + 1], IP_LEN);
                strncpy(ip_list_global[j + 1], tmp_ip,              IP_LEN);
            }
        }
    }

    memset(total->top_ips,        0, sizeof(total->top_ips));
    memset(total->top_ips_counts, 0, sizeof(total->top_ips_counts));
    int limite = *ip_num_global < 10 ? *ip_num_global : 10;
    for (int i = 0; i < limite; i++) {
        strncpy(total->top_ips[i], ip_list_global[i], IP_LEN - 1);
        total->top_ips[i][IP_LEN - 1]  = '\0';
        total->top_ips_counts[i]        = ip_count_global[i];
    }
}

/**
 * @brief Imprime o relatório final com todas as métricas agregadas.
 *
 * @param total Resultado global acumulado de todos os workers.
 * @param modo  Nome do modo de análise utilizado.
 */
static void imprimir_relatorio(const WorkerResult *total, char *modo) {
    printf("\n=== RELATORIO FINAL (%s) ===\n", modo);
    printf("Total de linhas  : %ld\n", total->total_lines);
    printf("DEBUG            : %ld\n", total->count_debug);
    printf("INFO             : %ld\n", total->count_info);
    printf("WARNINGS         : %ld\n", total->count_warn);
    printf("ERRORS           : %ld\n", total->count_error);
    printf("CRITICAL         : %ld\n", total->count_critical);
    printf("HTTP 4xx         : %ld\n", total->count_4xx);
    printf("HTTP 5xx         : %ld\n", total->count_5xx);
    printf("\n--- TOP 10 IPs ---\n");
    for (int i = 0; i < 10; i++) {
        if (total->top_ips[i][0] == '\0' || total->top_ips_counts[i] <= 0) break;
        printf("%2d. %s (%ld pedidos)\n", i + 1, total->top_ips[i], total->top_ips_counts[i]);
    }

    printf("\n--- ALERTAS CRITICOS ---\n");
    if (total->num_alerts == 0) {
        printf("Sem alertas criticos.\n");
    } else {
        for (int i = 0; i < total->num_alerts; i++) {
            printf("%2d. %s\n", i + 1, total->alerts[i]);
        }
    }
    printf("=================================\n");
}

/**
 * @brief Ponto de entrada do processo pai.
 *
 * @details Coordena a criação de N workers, a multiplexagem de N pipes via select(2),
 * a acumulação de resultados e a impressão do relatório final. O loop de select usa
 * timeout de 1 s para não bloquear indefinidamente caso um worker morra sem fechar o pipe.
 *
 * @param argc Número de argumentos na linha de comandos.
 * @param argv Argumentos: programa, directório, num_processos, modo, [--verbose].
 * @return 0 em caso de sucesso; 1 em erro.
 */
int main(int argc, char *argv[]) {
    if (argc < 4) {
        posix_writef(STDOUT_FILENO, "Uso: %s <diretorio> <num_processos> <modo> [--verbose]\n", argv[0]);
        exit(1);
    }

    char *diretorio     = argv[1];
    int   num_processos = converter_num_processos(argv[2]);
    char *modo          = argv[3];

    int verbose = 0;
    for (int i = 4; i < argc; i++) {
        if (strcmp(argv[i], "--verbose") == 0) verbose = 1;
    }

    if (parser_set_mode_from_string(modo) != 0) {
        posix_writef(STDERR_FILENO, "Modo invalido: %s\n", modo);
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
        posix_writef(STDOUT_FILENO, "Nenhum ficheiro encontrado.\n");
        libertar_ficheiros(ficheiros, total_ficheiros);
        exit(0);
    }

    posix_writef(STDOUT_FILENO, "A calcular dimensao total...\n");
    off_t total_bytes = obter_bytes_totais(ficheiros, total_ficheiros);
    posix_writef(STDOUT_FILENO, "Total de bytes encontrados: %lld\n\n", (long long)total_bytes);

    off_t bytes_por_worker = total_bytes / num_processos;

    WorkerConfig *configs = malloc((size_t)num_processos * sizeof(WorkerConfig));
    for (int i = 0; i < num_processos; i++) {
        configs[i].worker_index        = i;
        configs[i].byte_inicio         = (off_t)i * bytes_por_worker;
        /* O último worker absorve o resto da divisão inteira */
        configs[i].byte_fim            = (i == num_processos - 1) ? total_bytes
                                         : configs[i].byte_inicio + bytes_por_worker;
        configs[i].total_bytes_globais = total_bytes;
    }

    pid_t *pids          = malloc((size_t)num_processos * sizeof(pid_t));
    int   *pipes_leitura = malloc((size_t)num_processos * sizeof(int));
    for (int i = 0; i < num_processos; i++) pipes_leitura[i] = -1;

    /* fflush antes do fork evita duplicação dos buffers de stdio nos filhos */
    fflush(NULL);

    time_t t_inicio = time(NULL);

    for (int i = 0; i < num_processos; i++) {
        int fd[2]; /* fd[0]=leitura (pai), fd[1]=escrita (filho) */

        if (pipe(fd) == -1) { perror("pipe"); exit(1); }

        pid_t pid = fork();
        if (pid < 0) { perror("fork"); exit(1); }

        if (pid == 0) {
            close(fd[0]); /* filho não lê do pipe — fechar para evitar descritores órfãos */

            /* Fechar os extremos de leitura dos pipes de irmãos já criados */
            for (int j = 0; j < i; j++) {
                if (pipes_leitura[j] != -1) close(pipes_leitura[j]);
            }

            run_worker_pipe(ficheiros, total_ficheiros, fd[1], i,
                            configs[i].byte_inicio, configs[i].byte_fim, verbose);
            exit(0);
        }

        close(fd[1]); /* pai não escreve no pipe — fechar para detectar EOF quando filho terminar */
        pids[i]          = pid;
        pipes_leitura[i] = fd[0];
    }

    ProgressUpdate *progressos = calloc(num_processos, sizeof(ProgressUpdate));
    for (int i = 0; i < num_processos; i++) {
        progressos[i].worker_index = i;
        printf("Worker %2d [....................] -- Aguardar...\n", i);
    }
    fflush(stdout);

    WorkerResult total = {0};
    int resultados     = 0;

    char ip_list_global[256][IP_LEN];
    long ip_count_global[256] = {0};
    int  ip_num_global        = 0;

    /*
     * Loop de multiplexagem: select(2) com timeout de 1 s bloqueia até que pelo menos
     * um pipe esteja pronto para leitura. O loop termina quando resultados == num_processos.
     */
    while (resultados < num_processos) {
        fd_set readfds;
        FD_ZERO(&readfds);
        int max_fd = -1;

        for (int i = 0; i < num_processos; i++) {
            if (pipes_leitura[i] != -1) {
                FD_SET(pipes_leitura[i], &readfds);
                if (pipes_leitura[i] > max_fd) max_fd = pipes_leitura[i];
            }
        }

        struct timeval tv;
        tv.tv_sec  = 1; /* timeout de 1 s para não bloquear indefinidamente */
        tv.tv_usec = 0;

        int activity = select(max_fd + 1, &readfds, NULL, NULL, &tv);
        if (activity < 0) { perror("select"); exit(1); }
        if (activity == 0) continue; /* timeout: nenhum pipe pronto, tentar de novo */

        for (int i = 0; i < num_processos; i++) {
            if (pipes_leitura[i] == -1 || !FD_ISSET(pipes_leitura[i], &readfds)) continue;

            int     tipo;
            ssize_t lidos = read(pipes_leitura[i], &tipo, sizeof(tipo));

            if (lidos <= 0) {
                /* EOF no pipe: filho fechou o seu extremo de escrita */
                close(pipes_leitura[i]);
                pipes_leitura[i] = -1;
                resultados++;
                continue;
            }

            if (tipo == MSG_PROGRESSO) {
                ProgressUpdate pu;
                read(pipes_leitura[i], &pu, sizeof(pu));
                progressos[pu.worker_index] = pu;
                desenhar_dashboard(progressos, num_processos);

            } else if (tipo == MSG_RESULTADO) {
                WorkerResult r;
                read(pipes_leitura[i], &r, sizeof(r));
                acumular_resultado(&total, &r,
                                   (char (*)[IP_LEN])ip_list_global,
                                   ip_count_global, &ip_num_global);

                progressos[i].bytes_done = progressos[i].bytes_total; /* marcar 100 % */
                desenhar_dashboard(progressos, num_processos);

                close(pipes_leitura[i]);
                pipes_leitura[i] = -1;
                resultados++;
            }
        }
    }

    for (int i = 0; i < num_processos; i++) {
        int status;
        waitpid(pids[i], &status, 0);
    }

    long elapsed = (long)(time(NULL) - t_inicio);

    imprimir_relatorio(&total, modo);
    printf("Tempo de processamento: %ldmin %02lds\n", elapsed / 60, elapsed % 60);

    free(progressos);
    free(pipes_leitura);
    free(pids);
    free(configs);
    libertar_ficheiros(ficheiros, total_ficheiros);

    return 0;
}
