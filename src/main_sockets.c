/*
 * main_sockets.c  —  Código do processo PAI (Unix Domain Sockets)
 *
 * Protocolo simplificado após introdução de sinais para progresso:
 *   - MSG_CONFIG    (pai → filho): configuração da fatia de bytes
 *   - MSG_RESULTADO (filho → pai): métricas finais
 *
 * O progresso do dashboard é agora comunicado via SIGUSR1 + mmap partilhado,
 * eliminando as mensagens MSG_PROGRESSO do socket.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
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

#define MSG_RESULTADO 2
#define LARGURA_BARRA 20

/*
 * g_progress — array partilhado (mmap MAP_SHARED) entre pai e filhos.
 * g_redraw   — flag activada pelo handler de SIGUSR1.
 */
volatile int             *g_progress = NULL;
static volatile sig_atomic_t g_redraw  = 0;

static void handler_sigusr1(int sig) {
    (void)sig;
    g_redraw = 1;
}

/* ── Dashboard ────────────────────────────────────────────────────── */

static void desenhar_dashboard(int num_workers) {
    printf("\033[%dA", num_workers);
    printf("\033[J");

    for (int i = 0; i < num_workers; i++) {
        int pct = (int)g_progress[i];
        if (pct > 100) pct = 100;

        char barra[LARGURA_BARRA + 1];
        int cheio = pct * LARGURA_BARRA / 100;
        for (int b = 0; b < LARGURA_BARRA; b++)
            barra[b] = (b < cheio) ? '#' : '.';
        barra[LARGURA_BARRA] = '\0';

        printf("Worker %2d [%s] %3d%%\n", i, barra, pct);
    }
    fflush(stdout);
}

/* ── Utilitários ──────────────────────────────────────────────────── */

static void libertar_ficheiros(char **ficheiros, int total) {
    for (int i = 0; i < total; i++) free(ficheiros[i]);
    free(ficheiros);
}

static off_t obter_bytes_totais(char **ficheiros, int total_ficheiros) {
    off_t total = 0;
    struct stat st;
    for (int i = 0; i < total_ficheiros; i++) {
        if (stat(ficheiros[i], &st) == 0)
            total += st.st_size;
    }
    return total;
}

static char **ler_directorio(const char *dir, int *total_out) {
    int    capacidade = 10, total = 0;
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
        for (int i = 0; i < total->num_alerts; i++)
            printf("%2d. %s\n", i + 1, total->alerts[i]);
    }
    printf("=================================\n");
}

/* ── main ─────────────────────────────────────────────────────────── */

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

    /* ── 1. Descobrir ficheiros ── */
    int    total_ficheiros = 0;
    char **ficheiros       = ler_directorio(dir, &total_ficheiros);

    if (total_ficheiros == 0) {
        printf("Nenhum ficheiro .log ou .json encontrado em: %s\n", dir);
        free(ficheiros); exit(0);
    }
    if (num_procs > total_ficheiros) num_procs = total_ficheiros;

    printf("Ficheiros encontrados: %d | Workers: %d | Modo: %s\n\n",
           total_ficheiros, num_procs, modo);

    /* ── 2. Dimensão e divisão por bytes ── */
    printf("A calcular dimensao total...\n");
    off_t total_bytes    = obter_bytes_totais(ficheiros, total_ficheiros);
    printf("Total de bytes encontrados: %lld\n\n", (long long)total_bytes);

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

    /* ── 3. Memória partilhada para progresso ── */
    g_progress = mmap(NULL, (size_t)num_procs * sizeof(int),
                      PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (g_progress == MAP_FAILED) { perror("mmap"); exit(1); }
    memset((void *)g_progress, 0, (size_t)num_procs * sizeof(int));

    /* ── 4. Handler SIGUSR1 ── */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler_sigusr1;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGUSR1, &sa, NULL) != 0) { perror("sigaction"); exit(1); }

    /* ── 5. Criar socket servidor ── */
    unlink(SOCKET_PATH);

    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); exit(1); }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); exit(1);
    }
    if (listen(server_fd, 64) < 0) { perror("listen"); exit(1); }

    pid_t *pids = malloc(num_procs * sizeof(pid_t));

    fflush(NULL);
    time_t t_inicio = time(NULL);

    /* ── 6. Fork ── */
    for (int i = 0; i < num_procs; i++) {
        pid_t pid = fork();
        if (pid < 0) { perror("fork"); exit(1); }
        if (pid == 0) {
            close(server_fd);
            run_worker(ficheiros, total_ficheiros, num_procs, i, verbose);
            exit(0);
        }
        pids[i] = pid;
    }

    /* ── 7. Aceitar ligações dos filhos e enviar configuração ── */
    int *client_fds   = malloc(num_procs * sizeof(int));
    for (int i = 0; i < num_procs; i++) client_fds[i] = -1;

    int num_conectados = 0;
    while (num_conectados < num_procs) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(server_fd, &readfds);

        struct timeval tv = {5, 0};
        int activity = select(server_fd + 1, &readfds, NULL, NULL, &tv);

        if (activity < 0) {
            if (errno == EINTR) {
                if (g_redraw) { g_redraw = 0; }  /* ainda sem dashboard activo */
                continue;
            }
            perror("select"); exit(1);
        }
        if (activity == 0) { fprintf(stderr, "Timeout ao aguardar workers\n"); continue; }

        if (FD_ISSET(server_fd, &readfds)) {
            int client_fd = accept(server_fd, NULL, NULL);
            if (client_fd < 0) { perror("accept"); exit(1); }

            int idx = -1;
            for (int i = 0; i < num_procs; i++) {
                if (client_fds[i] == -1) { idx = i; break; }
            }
            if (idx >= 0) {
                client_fds[idx] = client_fd;
                int tipo = MSG_CONFIG;
                write(client_fds[idx], &tipo, sizeof(tipo));
                write(client_fds[idx], &configs[idx], sizeof(WorkerConfig));
                num_conectados++;
                if (verbose)
                    printf("Worker %d conectado (%d/%d)\n", idx, num_conectados, num_procs);
            }
        }
    }

    /* ── 8. Reservar espaço no terminal para o dashboard ── */
    for (int i = 0; i < num_procs; i++)
        printf("Worker %2d [....................] 0%%\n", i);
    fflush(stdout);

    /* ── 9. Loop principal: aguardar resultados dos filhos ── */
    char ip_list_global[256][IP_LEN];
    long ip_count_global[256] = {0};
    int  ip_num_global = 0;

    WorkerResult total = {0};
    int resultados = 0;

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

        struct timeval tv = {1, 0};
        int activity = select(max_fd + 1, &readfds, NULL, NULL, &tv);

        if (activity < 0) {
            if (errno == EINTR) {
                if (g_redraw) { g_redraw = 0; desenhar_dashboard(num_procs); }
                continue;
            }
            perror("select"); exit(1);
        }

        if (g_redraw) { g_redraw = 0; desenhar_dashboard(num_procs); }
        if (activity == 0) continue;

        for (int i = 0; i < num_procs; i++) {
            if (client_fds[i] == -1 || !FD_ISSET(client_fds[i], &readfds))
                continue;

            int tipo;
            ssize_t lidos = read(client_fds[i], &tipo, sizeof(tipo));
            if (lidos <= 0) {
                fprintf(stderr, "Worker %d: ligação fechada inesperadamente\n", i);
                close(client_fds[i]);
                client_fds[i] = -1;
                resultados++;
                continue;
            }

            if (tipo == MSG_RESULTADO) {
                WorkerResult r;
                read(client_fds[i], &r, sizeof(r));
                acumular_resultado(&total, &r, (char (*)[IP_LEN])ip_list_global,
                                   ip_count_global, &ip_num_global);
                desenhar_dashboard(num_procs);
                close(client_fds[i]);
                client_fds[i] = -1;
                resultados++;
            }
        }
    }

    /* ── 10. Fechar servidor e esperar filhos ── */
    close(server_fd);
    unlink(SOCKET_PATH);

    for (int i = 0; i < num_procs; i++) {
        int status;
        waitpid(pids[i], &status, 0);
        if (WIFEXITED(status) && WEXITSTATUS(status) != 0)
            fprintf(stderr, "Worker %d terminou com erro %d\n", i, WEXITSTATUS(status));
    }

    long elapsed = (long)(time(NULL) - t_inicio);

    munmap((void *)g_progress, (size_t)num_procs * sizeof(int));

    imprimir_relatorio(&total, modo);
    printf("Tempo de processamento: %ldmin %02lds\n", elapsed / 60, elapsed % 60);

    free(client_fds);
    free(pids);
    free(configs);
    libertar_ficheiros(ficheiros, total_ficheiros);

    return 0;
}
