/**
 * @file main_sockets.c
 * @brief Servidor pai para análise paralela de logs via Unix Domain Sockets.
 *
 * @details
 * O processo pai actua como servidor: cria um Unix Domain Socket, aguarda conexões
 * dos N processos filho e envia a cada um a configuração da sua fatia (WorkerConfig).
 * A comunicação é bidirecional — o pai recebe progresso e resultado pelos mesmos sockets.
 *
 * Fluxo de execução:
 *  1. Criar socket Unix Domain, bind(2) em SOCKET_PATH e listen(2).
 *  2. fork(2) × N — cada filho chama run_worker() que se liga ao servidor.
 *  3. Loop de accept(2): receber as N conexões e enviar MSG_CONFIG a cada uma.
 *  4. Loop de select(2): multiplexar N sockets, ler MSG_PROGRESSO/MSG_RESULTADO.
 *  5. Acumular resultados, aguardar todos os filhos, imprimir relatório, remover socket.
 *
 * Protocolo de mensagens:
 *  - pai → filho: MSG_CONFIG  (0) + WorkerConfig
 *  - filho → pai: MSG_PROGRESSO (1) + ProgressUpdate
 *  - filho → pai: MSG_RESULTADO (2) + WorkerResult
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
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "ipc.h"
#include "parser.h"
#include "worker.h"

/** @brief Tipo de mensagem de progresso enviada pelo filho ao pai. */
#define MSG_PROGRESSO  1

/** @brief Tipo de mensagem de resultado final enviada pelo filho ao pai. */
#define MSG_RESULTADO  2

/** @brief Número de caracteres '#' ou '.' na barra de progresso de cada worker. */
#define LARGURA_BARRA  20

/**
 * @brief Redesenha o dashboard de barras de progresso no terminal.
 *
 * @details Usa sequências de escape ANSI (`\033[NA` sobe N linhas, `\033[J` apaga
 * até ao fim do ecrã) para actualizar as barras in-place sem scroll. Requer terminal
 * com suporte a ANSI.
 *
 * @param progressos  Array com o estado de progresso de cada worker.
 * @param num_workers Número de workers (== número de linhas do dashboard).
 */
static void desenhar_dashboard(ProgressUpdate *progressos, int num_workers) {
    printf("\033[%dA", num_workers);
    printf("\033[J");

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
 * @param ficheiros Array de strings alocadas com strdup.
 * @param total     Número de elementos.
 */
static void libertar_ficheiros(char **ficheiros, int total) {
    for (int i = 0; i < total; i++) free(ficheiros[i]);
    free(ficheiros);
}

/**
 * @brief Calcula o total de bytes de todos os ficheiros em @p ficheiros.
 *
 * @param ficheiros       Array de caminhos absolutos.
 * @param total_ficheiros Número de ficheiros.
 * @return Soma dos tamanhos em bytes.
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
 * @brief Descobre ficheiros .log/.json num directório e retorna um array dinâmico.
 *
 * @details Usa readdir(3) para iterar as entradas e filtra por extensão.
 * O array cresce com realloc quando a capacidade é atingida (estratégia de duplicação).
 *
 * @param dir       Caminho do directório a pesquisar.
 * @param total_out Ponteiro onde guardar o número de ficheiros encontrados.
 * @return Array de strings alocado com malloc; o chamador é responsável pela libertação.
 */
static char **ler_directorio(const char *dir, int *total_out) {
    int    capacidade = 10;
    int    total      = 0;
    char **ficheiros  = malloc(capacidade * sizeof(char *));
    if (!ficheiros) { perror("malloc"); exit(1); }

    DIR *d = opendir(dir);
    if (!d) { perror("opendir"); exit(1); }

    struct dirent *entrada;
    while ((entrada = readdir(d)) != NULL) {
        char *nome = entrada->d_name;
        int   len  = strlen(nome);

        int e_log  = (len > 4 && strcmp(nome + len - 4, ".log")  == 0);
        int e_json = (len > 5 && strcmp(nome + len - 5, ".json") == 0);
        if (!e_log && !e_json) continue;

        if (total == capacidade) {
            capacidade *= 2;
            ficheiros = realloc(ficheiros, capacidade * sizeof(char *));
            if (!ficheiros) { perror("realloc"); exit(1); }
        }

        char caminho[512];
        snprintf(caminho, sizeof(caminho), "%s/%s", dir, nome);
        ficheiros[total++] = strdup(caminho);
    }

    closedir(d);
    *total_out = total;
    return ficheiros;
}

/**
 * @brief Funde o resultado de um worker na tabela global de métricas e IPs.
 *
 * @details Acumula contadores escalares e faz merge da tabela de top-10 IPs do worker
 * na tabela global (pesquisa linear + inserção ou incremento). Após cada merge, ordena
 * a tabela global por contagem decrescente e actualiza total->top_ips.
 *
 * @param total            Resultado global acumulado (actualizado in-place).
 * @param r                Resultado do worker a fundir.
 * @param ip_list_global   Tabela global de IPs (até 256 entradas).
 * @param ip_count_global  Contagens correspondentes.
 * @param ip_num_global    Ponteiro para o número actual de IPs na tabela global.
 */
static void acumular(WorkerResult *total, WorkerResult *r,
                     char ip_list_global[256][IP_LEN], long ip_count_global[256], int *ip_num_global) {
    total->total_lines    += r->total_lines;
    total->count_debug    += r->count_debug;
    total->count_info     += r->count_info;
    total->count_warn     += r->count_warn;
    total->count_error    += r->count_error;
    total->count_critical += r->count_critical;
    total->count_4xx      += r->count_4xx;
    total->count_5xx      += r->count_5xx;

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

    for (int i = 0; i < r->num_alerts && total->num_alerts < MAX_ALERTS; i++) {
        strncpy(total->alerts[total->num_alerts], r->alerts[i], ALERT_LEN - 1);
        total->alerts[total->num_alerts][ALERT_LEN - 1] = '\0';
        total->num_alerts++;
    }

    /* Ordenar tabela global por contagem decrescente */
    for (int i = 0; i < *ip_num_global - 1; i++) {
        for (int j = 0; j < *ip_num_global - i - 1; j++) {
            if (ip_count_global[j] < ip_count_global[j + 1]) {
                long tmp_count         = ip_count_global[j];
                ip_count_global[j]     = ip_count_global[j + 1];
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
        total->top_ips[i][IP_LEN - 1] = '\0';
        total->top_ips_counts[i]       = ip_count_global[i];
    }
}

/**
 * @brief Imprime o relatório final com todas as métricas agregadas.
 *
 * @param total Resultado global acumulado.
 * @param modo  Nome do modo de análise utilizado.
 */
static void imprimir_relatorio(WorkerResult *total, char *modo) {
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
 * @brief Ponto de entrada do processo pai — servidor de Unix Domain Sockets.
 *
 * @details Cria o socket de servidor antes do fork para que o socket exista quando
 * os filhos tentarem ligar-se. unlink(SOCKET_PATH) antes do bind remove ficheiros
 * órfãos de execuções anteriores que não terminaram correctamente.
 *
 * O loop de accept aguarda as N conexões dos filhos e envia MSG_CONFIG a cada uma
 * usando o índice de chegada como worker_index (FIFO). O select posterior multiplexar
 * os N sockets clientes para receber progresso e resultados.
 *
 * @param argc Número de argumentos na linha de comandos.
 * @param argv Argumentos: programa, directório, num_processos, modo, [--verbose].
 * @return 0 em caso de sucesso; 1 em erro.
 */
int main(int argc, char *argv[]) {
    if (argc < 4) {
        printf("Uso: %s <directorio> <num_processos> <modo> [--verbose]\n", argv[0]);
        exit(1);
    }

    char *dir       = argv[1];
    int   num_procs = atoi(argv[2]);
    char *modo      = argv[3];
    int   verbose   = (argc > 4 && strcmp(argv[4], "--verbose") == 0);

    if (parser_set_mode_from_string(modo) != 0) {
        fprintf(stderr, "Modo invalido: %s\n", modo);
        exit(1);
    }

    int    total_ficheiros = 0;
    char **ficheiros       = ler_directorio(dir, &total_ficheiros);

    if (total_ficheiros == 0) {
        printf("Nenhum ficheiro .log ou .json encontrado em: %s\n", dir);
        free(ficheiros);
        exit(0);
    }

    /* Limitar num_procs ao número de ficheiros disponíveis */
    if (num_procs > total_ficheiros)
        num_procs = total_ficheiros;

    printf("Ficheiros encontrados: %d | Workers: %d | Modo: %s\n\n",
           total_ficheiros, num_procs, modo);

    printf("A calcular dimensao total...\n");
    off_t total_bytes = obter_bytes_totais(ficheiros, total_ficheiros);
    printf("Total de bytes encontrados: %lld\n\n", (long long)total_bytes);

    WorkerConfig *configs = malloc(num_procs * sizeof(WorkerConfig));
    if (!configs) { perror("malloc"); exit(1); }

    off_t bytes_por_worker = total_bytes / num_procs;
    for (int i = 0; i < num_procs; i++) {
        configs[i].worker_index        = i;
        configs[i].byte_inicio         = (off_t)i * bytes_por_worker;
        /* O último worker absorve o resto da divisão inteira */
        configs[i].byte_fim            = (i == num_procs - 1) ? total_bytes
                                         : configs[i].byte_inicio + bytes_por_worker;
        configs[i].total_bytes_globais = total_bytes;
    }

    /* Remover socket órfão de execuções anteriores antes de bind */
    unlink(SOCKET_PATH);

    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); exit(1); }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        exit(1);
    }

    /* backlog de 64 garante que todos os filhos podem ligar-se sem ECONNREFUSED */
    if (listen(server_fd, 64) < 0) {
        perror("listen");
        exit(1);
    }

    pid_t *pids = malloc(num_procs * sizeof(pid_t));

    /* fflush antes do fork evita duplicação dos buffers de stdio nos filhos */
    fflush(NULL);

    time_t t_inicio = time(NULL);

    for (int i = 0; i < num_procs; i++) {
        pid_t pid = fork();
        if (pid < 0) { perror("fork"); exit(1); }

        if (pid == 0) {
            /* Filho não precisa do socket de servidor — fechar para não bloquear unlink */
            if (close(server_fd) == -1) {
                perror("close");
                exit(1);
            }
            run_worker(ficheiros, total_ficheiros, num_procs, i, verbose);
            exit(0);
        }

        pids[i] = pid;
    }

    /* Aceitar as N conexões dos filhos e enviar a configuração a cada um */
    int *client_fds  = malloc(num_procs * sizeof(int));
    for (int i = 0; i < num_procs; i++) client_fds[i] = -1;

    int num_conectados = 0;

    while (num_conectados < num_procs) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(server_fd, &readfds);

        struct timeval tv;
        tv.tv_sec  = 5; /* timeout de 5 s — suficiente para qualquer filho arrancar */
        tv.tv_usec = 0;

        int activity = select(server_fd + 1, &readfds, NULL, NULL, &tv);
        if (activity < 0) { perror("select"); exit(1); }

        if (activity == 0) {
            /* Timeout sem nova conexão — o filho pode ter falhado no arranque */
            fprintf(stderr, "Timeout: aguardando conexão de worker\n");
            continue;
        }

        if (FD_ISSET(server_fd, &readfds)) {
            int client_fd = accept(server_fd, NULL, NULL);
            if (client_fd < 0) { perror("accept"); exit(1); }

            /* Atribuir ao primeiro slot livre — os filhos ligam-se pela ordem de fork */
            int idx = -1;
            for (int i = 0; i < num_procs; i++) {
                if (client_fds[i] == -1) { idx = i; break; }
            }

            if (idx >= 0) {
                client_fds[idx] = client_fd;

                /* Enviar configuração: tipo primeiro, depois a struct (protocolo fixo) */
                int tipo = MSG_CONFIG;
                write(client_fds[idx], &tipo, sizeof(tipo));
                write(client_fds[idx], &configs[idx], sizeof(WorkerConfig));

                num_conectados++;
                if (verbose)
                    printf("Worker %d conectado (total: %d/%d)\n",
                           idx, num_conectados, num_procs);
            }
        }
    }

    ProgressUpdate *progressos = calloc(num_procs, sizeof(ProgressUpdate));
    for (int i = 0; i < num_procs; i++) {
        progressos[i].worker_index = i;
        printf("Worker %2d [....................] -- Aguardar...\n", i);
    }
    fflush(stdout);

    char ip_list_global[256][IP_LEN];
    long ip_count_global[256] = {0};
    int  ip_num_global        = 0;

    WorkerResult total     = {0};
    int          resultados = 0;

    /*
     * Loop de multiplexagem: select(2) com timeout de 1 s monitoriza os N sockets
     * clientes. EOF num socket (lidos == 0) indica que o filho terminou inesperadamente.
     */
    while (resultados < num_procs) {
        fd_set readfds;
        FD_ZERO(&readfds);

        int max_fd = -1;
        for (int i = 0; i < num_procs; i++) {
            if (client_fds[i] != -1) {
                FD_SET(client_fds[i], &readfds);
                if (client_fds[i] > max_fd) max_fd = client_fds[i];
            }
        }

        struct timeval tv;
        tv.tv_sec  = 1;
        tv.tv_usec = 0;

        int activity = select(max_fd + 1, &readfds, NULL, NULL, &tv);
        if (activity < 0) { perror("select"); exit(1); }
        if (activity == 0) continue;

        for (int i = 0; i < num_procs; i++) {
            if (client_fds[i] == -1 || !FD_ISSET(client_fds[i], &readfds))
                continue;

            int     tipo;
            ssize_t lidos = read(client_fds[i], &tipo, sizeof(tipo));
            if (lidos <= 0) {
                fprintf(stderr, "Worker %d: ligação fechada inesperadamente\n", i);
                close(client_fds[i]);
                client_fds[i] = -1;
                resultados++;
                continue;
            }

            if (tipo == MSG_PROGRESSO) {
                ProgressUpdate pu;
                read(client_fds[i], &pu, sizeof(pu));
                progressos[pu.worker_index] = pu;
                desenhar_dashboard(progressos, num_procs);

            } else if (tipo == MSG_RESULTADO) {
                WorkerResult r;
                read(client_fds[i], &r, sizeof(r));
                acumular(&total, &r,
                         (char (*)[IP_LEN])ip_list_global,
                         ip_count_global, &ip_num_global);

                progressos[i].bytes_done = progressos[i].bytes_total; /* marcar 100 % */
                desenhar_dashboard(progressos, num_procs);

                close(client_fds[i]);
                client_fds[i] = -1;
                resultados++;
            }
        }
    }

    close(server_fd);
    unlink(SOCKET_PATH); /* remover o ficheiro do socket do sistema de ficheiros */

    for (int i = 0; i < num_procs; i++) {
        int status;
        waitpid(pids[i], &status, 0);
        if (WIFEXITED(status) && WEXITSTATUS(status) != 0)
            fprintf(stderr, "Worker %d terminou com erro %d\n", i, WEXITSTATUS(status));
    }

    long elapsed = (long)(time(NULL) - t_inicio);

    imprimir_relatorio(&total, modo);
    printf("Tempo de processamento: %ldmin %02lds\n", elapsed / 60, elapsed % 60);

    free(progressos);
    free(client_fds);
    free(pids);
    free(configs);
    libertar_ficheiros(ficheiros, total_ficheiros);

    return 0;
}
