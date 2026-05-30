/*
 * main_sockets.c  —  Código do processo PAI
 *
 * O pai faz a orquestração toda:
 *   1. Lê o directório e descobre os ficheiros de log.
 *   2. Cria um socket Unix e fica à escuta.
 *   3. Cria N filhos com fork(), cada um com uma fatia dos ficheiros.
 *   4. Aceita as ligações dos filhos e lê as mensagens que eles enviam:
 *        MSG_PROGRESSO → actualiza o dashboard no ecrã
 *        MSG_RESULTADO → acumula as contagens no total geral
 *   5. Quando todos os filhos terminaram, imprime o relatório final.
 *
 * Protocolo de mensagens (igual ao worker):
 *   Cada mensagem começa com um int:
 *     1 = MSG_PROGRESSO  →  a seguir vem um ProgressUpdate
 *     2 = MSG_RESULTADO  →  a seguir vem um WorkerResult
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

#define MSG_PROGRESSO 1
#define MSG_RESULTADO 2

/* ═════════════════════════════════════════════════════════════════════════════
 * DASHBOARD
 *
 * O dashboard funciona com um truque de terminal:
 *   - Imprimimos uma linha por worker no início (para "reservar" espaço).
 *   - Quando queremos actualizar, subimos o cursor N linhas com \033[NA,
 *     apagamos tudo abaixo com \033[J, e voltamos a imprimir as barras.
 *   - O resultado visual é uma animação in-place sem scroll.
 * ════════════════════════════════════════════════════════════════════════════= */

#define LARGURA_BARRA 20

/* Desenha dashboard com progresso de cada worker no terminal */
static void desenhar_dashboard(ProgressUpdate *progressos, int num_workers) {
    /* Subir o cursor tantas linhas quantos workers existem */
    printf("\033[%dA", num_workers);
    /* Apagar tudo abaixo do cursor (as linhas antigas) */
    printf("\033[J");

    for (int i = 0; i < num_workers; i++) {
        long feitas = progressos[i].bytes_done;
        long total  = progressos[i].bytes_total;

        int pct = (total > 0) ? (int)(feitas * 100 / total) : 0;
        if (pct > 100) pct = 100;

        /* Construir a barra de progresso com '#' e '.' */
        char barra[LARGURA_BARRA + 1];
        int cheio = pct * LARGURA_BARRA / 100;
        for (int b = 0; b < LARGURA_BARRA; b++)
            barra[b] = (b < cheio) ? '#' : '.';
        barra[LARGURA_BARRA] = '\0';

        printf("Worker %2d [%s] %3d%% (%ld/%ld bytes)\n",
               i, barra, pct, feitas, total);
    }

    fflush(stdout); /* forçar o flush — o terminal pode ter buffering */
}

/* ═════════════════════════════════════════════════════════════════════════════
 * UTILITÁRIOS DO PAI
 * ════════════════════════════════════════════════════════════════════════════= */

/* Liberta memoria alocada para ficheiros */
static void libertar_ficheiros(char **ficheiros, int total) {
    for (int i = 0; i < total; i++) free(ficheiros[i]);
    free(ficheiros);
}

/* Obtem tamanho total em bytes de todos os ficheiros */
static off_t obter_bytes_totais(char **ficheiros, int total_ficheiros) {
    off_t total = 0;
    struct stat st;
    for (int i = 0; i < total_ficheiros; i++) {
        if (stat(ficheiros[i], &st) == 0)
            total += st.st_size;
    }
    return total;
}

/* Lê directory e retorna array de caminhos para ficheiros .log e .json */
static char **ler_directorio(const char *dir, int *total_out) {
    int capacidade = 10;
    int total = 0;
    char **ficheiros = malloc(capacidade * sizeof(char *));
    if (!ficheiros) { perror("malloc"); exit(1); }

    DIR *d = opendir(dir);
    if (!d) { perror("opendir"); exit(1); }

    struct dirent *entrada;
    while ((entrada = readdir(d)) != NULL) {
        char *nome = entrada->d_name;
        int len = strlen(nome);

        /* Só nos interessam ficheiros .log e .json */
        int e_log  = (len > 4 && strcmp(nome + len - 4, ".log")  == 0);
        int e_json = (len > 5 && strcmp(nome + len - 5, ".json") == 0);
        if (!e_log && !e_json) continue;

        /* Crescer o array se necessário */
        if (total == capacidade) {
            capacidade *= 2;
            ficheiros = realloc(ficheiros, capacidade * sizeof(char *));
            if (!ficheiros) { perror("realloc"); exit(1); }
        }

        /* Guardar o caminho completo: "directorio/ficheiro.log" */
        char caminho[512];
        snprintf(caminho, sizeof(caminho), "%s/%s", dir, nome);
        ficheiros[total++] = strdup(caminho);
    }

    closedir(d);
    *total_out = total;
    return ficheiros;
}

/* Somar os resultados de um filho ao total geral */
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

    /* Atualizar Top 10 IPs globalmente a partir do Top 10 de cada worker */
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
            ip_count_global[*ip_num_global] = r->top_ips_counts[k];
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
    
    for (int i = 0; i < *ip_num_global - 1; i++) {
        for (int j = 0; j < *ip_num_global - i - 1; j++) {
            if (ip_count_global[j] < ip_count_global[j + 1]) {
                long tmp_count = ip_count_global[j];
                ip_count_global[j] = ip_count_global[j + 1];
                ip_count_global[j + 1] = tmp_count;

                char tmp_ip[IP_LEN];
                strncpy(tmp_ip, ip_list_global[j], IP_LEN);
                strncpy(ip_list_global[j], ip_list_global[j + 1], IP_LEN);
                strncpy(ip_list_global[j + 1], tmp_ip, IP_LEN);
            }
        }
    }

    memset(total->top_ips, 0, sizeof(total->top_ips));
    memset(total->top_ips_counts, 0, sizeof(total->top_ips_counts));
    int limite = *ip_num_global < 10 ? *ip_num_global : 10;
    for (int i = 0; i < limite; i++) {
        strncpy(total->top_ips[i], ip_list_global[i], IP_LEN - 1);
        total->top_ips[i][IP_LEN - 1] = '\0';
        total->top_ips_counts[i] = ip_count_global[i];
    }
}

/* Imprimir o relatório final */
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

/* ═════════════════════════════════════════════════════════════════════════════
 * MAIN
 * ════════════════════════════════════════════════════════════════════════════= */

int main(int argc, char *argv[]) {

    if (argc < 4) {
        printf("Uso: %s <directorio> <num_processos> <modo> [--verbose]\n", argv[0]);
        exit(1);
    }

    char *dir          = argv[1];
    int   num_procs    = atoi(argv[2]);
    char *modo         = argv[3];
    int   verbose      = (argc > 4 && strcmp(argv[4], "--verbose") == 0);

    /* Configurar o modo do parser (security / traffic / full / ...) */
    if (parser_set_mode_from_string(modo) != 0) {
        fprintf(stderr, "Modo invalido: %s\n", modo);
        exit(1);
    }

    /* ── 1. Descobrir os ficheiros de log ── */
    int total_ficheiros = 0;
    char **ficheiros = ler_directorio(dir, &total_ficheiros);

    if (total_ficheiros == 0) {
        printf("Nenhum ficheiro .log ou .json encontrado em: %s\n", dir);
        free(ficheiros);
        exit(0);
    }

    /* Não faz sentido ter mais processos do que ficheiros */
    if (num_procs > total_ficheiros)
        num_procs = total_ficheiros;

    printf("Ficheiros encontrados: %d | Workers: %d | Modo: %s\n\n",
           total_ficheiros, num_procs, modo);

    /* ── 1.5 Obter dimensão total via stat() — sem ler os ficheiros ── */
    printf("A calcular dimensao total...\n");
    off_t total_bytes = obter_bytes_totais(ficheiros, total_ficheiros);
    printf("Total de bytes encontrados: %lld\n\n", (long long)total_bytes);

    /* Calcular configuração de cada worker antes do fork */
    WorkerConfig *configs = malloc(num_procs * sizeof(WorkerConfig));
    if (!configs) { perror("malloc"); exit(1); }

    off_t bytes_por_worker = total_bytes / num_procs;
    for (int i = 0; i < num_procs; i++) {
        configs[i].worker_index        = i;
        configs[i].byte_inicio         = (off_t)i * bytes_por_worker;
        configs[i].byte_fim            = (i == num_procs - 1) ? total_bytes
                                         : configs[i].byte_inicio + bytes_por_worker;
        configs[i].total_bytes_globais = total_bytes;
    }

    /* ── 2. Criar o socket servidor (antes do fork, para os filhos herdarem o path) ── */
    
    /* Remover socket antigo se existir */
    unlink(SOCKET_PATH);

    /* Criar socket AF_UNIX do tipo SOCK_STREAM */
    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); exit(1); }

    /* Configurar o endereço */
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    /* Bind — associar o socket ao path */
    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        exit(1);
    }

    /* Listen — aceitar até 64 ligações pendentes */
    if (listen(server_fd, 64) < 0) {
        perror("listen");
        exit(1);
    }

    pid_t *pids = malloc(num_procs * sizeof(pid_t));

    fflush(NULL); /* limpar buffers antes do fork para não duplicar output */

    time_t t_inicio = time(NULL);

    /* ── 3. Criar os filhos com fork() ── */
    for (int i = 0; i < num_procs; i++) {
        pid_t pid = fork();
        if (pid < 0) { perror("fork"); exit(1); }

        if (pid == 0) {
            if (close(server_fd) == -1) {
                perror("close");
                exit(1);
            }

            // O PAI AGORA PASSA TODOS OS FICHEIROS PARA TODOS OS FILHOS!
            // A divisão vai ser feita por linhas lá dentro.
            run_worker(ficheiros, total_ficheiros, num_procs, i, verbose);
            exit(0);
        }

        pids[i] = pid;
    }

    /* ── 4. Aceitar as ligações dos filhos (uma por filho, usando select para não bloquear) ── */
    int *client_fds = malloc(num_procs * sizeof(int));
    for (int i = 0; i < num_procs; i++) client_fds[i] = -1;
    
    int num_conectados = 0;
    
    while (num_conectados < num_procs) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(server_fd, &readfds);
        
        struct timeval tv;
        tv.tv_sec = 5;
        tv.tv_usec = 0;
        
        int activity = select(server_fd + 1, &readfds, NULL, NULL, &tv);
        if (activity < 0) { perror("select"); exit(1); }
        
        if (activity == 0) {
            fprintf(stderr, "Timeout: aguardando conexão de worker\n");
            continue;
        }
        
        if (FD_ISSET(server_fd, &readfds)) {
            /* Aceitar ligação de um cliente */
            int client_fd = accept(server_fd, NULL, NULL);
            if (client_fd < 0) { perror("accept"); exit(1); }
            
            /* Encontrar um slot vazio para este worker */
            /* Nota: não é por ordem, pode ser qualquer um */
            int idx = -1;
            for (int i = 0; i < num_procs; i++) {
                if (client_fds[i] == -1) {
                    idx = i;
                    break;
                }
            }
            
            if (idx >= 0) {
                client_fds[idx] = client_fd;
                
                /* Enviar configuração ao worker */
                int tipo = MSG_CONFIG;
                write(client_fds[idx], &tipo, sizeof(tipo));
                write(client_fds[idx], &configs[idx], sizeof(WorkerConfig));
                
                num_conectados++;
                if (verbose)
                    printf("Worker %d conectado (total: %d/%d)\n", idx, num_conectados, num_procs);
            }
        }
    }

    /* ── 5. Reservar espaço no terminal para o dashboard ── */
    ProgressUpdate *progressos = calloc(num_procs, sizeof(ProgressUpdate));
    for (int i = 0; i < num_procs; i++) {
        progressos[i].worker_index = i;
        printf("Worker %2d [....................] -- Aguardar...\n", i);
    }
    fflush(stdout);

    /* ── 6. Loop principal: receber mensagens dos filhos (com select, sem busy-waiting) ── */
    char ip_list_global[256][IP_LEN];
    long ip_count_global[256] = {0};
    int ip_num_global = 0;
    
    WorkerResult total = {0};
    int resultados = 0;

    while (resultados < num_procs) {
        fd_set readfds;
        FD_ZERO(&readfds);
        
        /* Adicionar todos os client_fds ao set para monitorar */
        int max_fd = -1;
        for (int i = 0; i < num_procs; i++) {
            if (client_fds[i] != -1) {
                FD_SET(client_fds[i], &readfds);
                if (client_fds[i] > max_fd) max_fd = client_fds[i];
            }
        }
        
        /* Esperar por actividade com timeout de 1 segundo */
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        
        int activity = select(max_fd + 1, &readfds, NULL, NULL, &tv);
        if (activity < 0) { perror("select"); exit(1); }
        
        if (activity == 0) continue; /* Timeout, tenta novamente */
        
        /* Verificar qual(ais) socket(s) têm dados disponíveis */
        for (int i = 0; i < num_procs; i++) {
            if (client_fds[i] == -1 || !FD_ISSET(client_fds[i], &readfds)) 
                continue;

            /* Ler o tipo da mensagem */
            int tipo;
            ssize_t lidos = read(client_fds[i], &tipo, sizeof(tipo));
            if (lidos <= 0) {
                /* Filho fechou o socket inesperadamente */
                fprintf(stderr, "Worker %d: ligação fechada inesperadamente\n", i);
                close(client_fds[i]);
                client_fds[i] = -1;
                resultados++;
                continue;
            }

            if (tipo == MSG_PROGRESSO) {
                /* Receber e actualizar progresso */
                ProgressUpdate pu;
                read(client_fds[i], &pu, sizeof(pu));
                progressos[pu.worker_index] = pu;
                desenhar_dashboard(progressos, num_procs);

            } else if (tipo == MSG_RESULTADO) {
                /* Receber resultado e acumular */
                WorkerResult r;
                read(client_fds[i], &r, sizeof(r));
                acumular(&total, &r, (char (*)[IP_LEN])ip_list_global, ip_count_global, &ip_num_global);

                /* Marcar este worker como 100% terminado */
                progressos[i].bytes_done = progressos[i].bytes_total;
                desenhar_dashboard(progressos, num_procs);

                close(client_fds[i]);
                client_fds[i] = -1;
                resultados++;
            }
        }
    }

    /* ── 7. Fechar o socket servidor e remover o ficheiro do socket ── */
    close(server_fd);
    unlink(SOCKET_PATH);

    /* ── 8. Esperar que todos os filhos terminem (recolher zombies) ── */
    for (int i = 0; i < num_procs; i++) {
        int status;
        waitpid(pids[i], &status, 0);
        if (WIFEXITED(status) && WEXITSTATUS(status) != 0)
            fprintf(stderr, "Worker %d terminou com erro %d\n", i, WEXITSTATUS(status));
    }

    /* ── 9. Relatório final ── */
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
