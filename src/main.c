#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>

#include "worker.h"
#include "ipc.h"

/* =========================================================
 * Variáveis globais para o dashboard
 * ========================================================= */
#define MAX_WORKERS 64

static struct {
    pid_t pid;
    long  lines_done;
    long  lines_total;
} worker_status[MAX_WORKERS];

static int    g_num_workers  = 0;
static time_t g_start_time   = 0;
static long   g_total_errors = 0;

/* =========================================================
 * draw_dashboard
 * ========================================================= */
static void draw_dashboard(void) {
    int linhas = g_num_workers + 5;
    printf("\033[%dA", linhas);
    printf("\033[J");

    time_t elapsed = time(NULL) - g_start_time;
    int hh = elapsed / 3600;
    int mm = (elapsed % 3600) / 60;
    int ss = elapsed % 60;

    long total_done  = 0;
    long total_total = 0;
    for (int i = 0; i < g_num_workers; i++) {
        total_done  += worker_status[i].lines_done;
        total_total += worker_status[i].lines_total;
    }
    int total_pct = (total_total > 0)
                    ? (int)(total_done * 100 / total_total) : 0;
    if (total_pct > 100) total_pct = 100;

    printf("╔══════════════════════════════════════════╗\n");
    printf("║       LOG ANALYZER - Real-time Monitor   ║\n");
    printf("╠══════════════════════════════════════════╣\n");

    for (int i = 0; i < g_num_workers; i++) {
        int pct = (worker_status[i].lines_total > 0)
                  ? (int)(worker_status[i].lines_done * 100
                          / worker_status[i].lines_total) : 0;
        if (pct > 100) pct = 100;

        char bar[21];
        int filled = pct / 5;
        for (int b = 0; b < 20; b++) bar[b] = (b < filled) ? '#' : '.';
        bar[20] = '\0';

        printf("║ Worker %-2d [%s] %3d%%           ║\n",
               i + 1, bar, pct);
    }

    printf("╠══════════════════════════════════════════╣\n");

    char tot_bar[21];
    int tot_filled = total_pct / 5;
    for (int b = 0; b < 20; b++) tot_bar[b] = (b < tot_filled) ? '#' : '.';
    tot_bar[20] = '\0';

    printf("║ Total     [%s] %3d%%           ║\n", tot_bar, total_pct);
    printf("║ Elapsed: %02d:%02d:%02d | Errors: %-6ld      ║\n",
           hh, mm, ss, g_total_errors);
    printf("╚══════════════════════════════════════════╝\n");
}

/* =========================================================
 * SIGALRM handler
 * ========================================================= */
static void sigalrm_handler(int sig) {
    (void)sig;
    draw_dashboard();
    alarm(1);
}

/* =========================================================
 * handle_client — lê mensagens de um filho até fechar a ligação
 *
 * O protocolo é simples:
 *   - O filho envia N ProgressUpdates (tamanho fixo)
 *   - O filho envia 1 WorkerResult no final (tamanho fixo)
 *   - O filho fecha o socket
 *
 * Distinguimos pelo worker_index: ProgressUpdate tem worker_index >= 0,
 * WorkerResult tem pid mas não tem worker_index — usamos o tamanho
 * das structs para distinguir. Como têm tamanhos diferentes lemos
 * sempre sizeof(ProgressUpdate) e verificamos se é o resultado final
 * com uma flag no protocolo.
 *
 * Solução mais simples: enviar um tipo antes de cada mensagem.
 * ========================================================= */
#define MSG_PROGRESS 1
#define MSG_RESULT   2

/* Cabeçalho simples que precede cada mensagem */
typedef struct {
    int type;   /* MSG_PROGRESS ou MSG_RESULT */
} MsgHeader;

/* =========================================================
 * main
 * ========================================================= */
int main(int argc, char *argv[]) {

    if (argc < 4) {
        printf("Uso: %s <diretorio> <num_processos> <modo> [--verbose]\n", argv[0]);
        exit(1);
    }

    char *diretorio   = argv[1];
    int num_processos = atoi(argv[2]);
    char *modo        = argv[3];

    int verbose = 0;
    for (int i = 4; i < argc; i++)
        if (strcmp(argv[i], "--verbose") == 0) verbose = 1;

    printf("Diretorio: %s\n", diretorio);
    printf("Processos: %d\n", num_processos);
    printf("Modo: %s\n", modo);
    printf("Verbose: %s\n", verbose ? "sim" : "nao");

    /* ---- DESCOBRIR FICHEIROS .log ---- */
    int capacidade = 10, total_ficheiros = 0;
    char **ficheiros = malloc(capacidade * sizeof(char *));
    if (!ficheiros) { perror("malloc"); exit(1); }

    DIR *dir = opendir(diretorio);
    if (!dir) { perror("opendir"); exit(1); }

    struct dirent *entrada;
    while ((entrada = readdir(dir)) != NULL) {
        char *nome = entrada->d_name;
        int len = strlen(nome);
        if (len > 4 && strcmp(nome + len - 4, ".log") == 0) {
            if (total_ficheiros == capacidade) {
                capacidade *= 2;
                ficheiros = realloc(ficheiros, capacidade * sizeof(char *));
                if (!ficheiros) { perror("realloc"); exit(1); }
            }
            char caminho[512];
            snprintf(caminho, sizeof(caminho), "%s/%s", diretorio, nome);
            ficheiros[total_ficheiros++] = strdup(caminho);
            if (verbose) printf("Encontrei: %s\n", ficheiros[total_ficheiros-1]);
        }
    }
    closedir(dir);

    printf("Total de ficheiros .log encontrados: %d\n", total_ficheiros);

    if (total_ficheiros == 0) {
        printf("Nenhum ficheiro .log encontrado.\n");
        exit(0);
    }
    if (num_processos > total_ficheiros) {
        num_processos = total_ficheiros;
        printf("Ajustado para %d processo(s)\n", num_processos);
    }

    /* ---- CRIAR SOCKET SERVIDOR ---- */
    int server_fd = create_server_socket();
    if (server_fd < 0) exit(1);

    /* ---- INICIALIZAR DASHBOARD ---- */
    g_num_workers = num_processos;
    g_start_time  = time(NULL);
    memset(worker_status, 0, sizeof(worker_status));

    /* ---- CRIAR PROCESSOS FILHO ---- */
    int ficheiros_por_processo = total_ficheiros / num_processos;

    for (int i = 0; i < num_processos; i++) {
        pid_t pid = fork();
        if (pid < 0) { perror("fork"); exit(1); }

        if (pid == 0) {
            /* ---- FILHO ---- */
            close(server_fd);   /* filho não precisa do servidor */

            int inicio = i * ficheiros_por_processo;
            int fim    = (i == num_processos - 1)
                         ? total_ficheiros
                         : inicio + ficheiros_por_processo;

            run_worker(ficheiros, inicio, fim, i, verbose);
            exit(0);
        }
    }

    /* ---- PAI: aceitar ligações e ler mensagens ---- */
    for (int i = 0; i < g_num_workers + 5; i++) printf("\n");

    signal(SIGALRM, sigalrm_handler);
    alarm(1);

    WorkerResult total = {0};
    int workers_done = 0;

    /* Aceitar uma ligação por filho */
    while (workers_done < num_processos) {
        int client_fd = accept_client(server_fd);
        if (client_fd < 0) continue;

        /* Ler mensagens deste filho até fechar a ligação */
        MsgHeader hdr;
        while (readn(client_fd, &hdr, sizeof(hdr)) == (ssize_t)sizeof(hdr)) {

            if (hdr.type == MSG_PROGRESS) {
                ProgressUpdate pu;
                if (readn(client_fd, &pu, sizeof(pu)) == (ssize_t)sizeof(pu)) {
                    int idx = pu.worker_index;
                    if (idx >= 0 && idx < g_num_workers) {
                        worker_status[idx].pid        = pu.pid;
                        worker_status[idx].lines_done  = pu.lines_done;
                        worker_status[idx].lines_total = pu.lines_total;
                    }
                }
            } else if (hdr.type == MSG_RESULT) {
                WorkerResult r;
                if (readn(client_fd, &r, sizeof(r)) == (ssize_t)sizeof(r)) {
                    if (verbose)
                        printf("[Pai] Recebi do filho %d: LINHAS=%ld ERRORS=%ld TOP_IP=%s\n",
                               r.pid, r.total_lines, r.count_error, r.top_ip);

                    total.total_lines    += r.total_lines;
                    total.count_debug    += r.count_debug;
                    total.count_info     += r.count_info;
                    total.count_warn     += r.count_warn;
                    total.count_error    += r.count_error;
                    total.count_critical += r.count_critical;
                    total.count_4xx      += r.count_4xx;
                    total.count_5xx      += r.count_5xx;
                    g_total_errors        = total.count_error;
                }
            }
        }

        close(client_fd);
        workers_done++;
    }

    alarm(0);
    draw_dashboard();

    close(server_fd);
    unlink(SOCKET_PATH);

    for (int i = 0; i < num_processos; i++) wait(NULL);

    printf("\n=== RELATORIO FINAL ===\n");
    printf("Total de linhas : %ld\n", total.total_lines);
    printf("DEBUG           : %ld\n", total.count_debug);
    printf("INFO            : %ld\n", total.count_info);
    printf("WARNINGS        : %ld\n", total.count_warn);
    printf("ERRORS          : %ld\n", total.count_error);
    printf("CRITICAL        : %ld\n", total.count_critical);
    printf("4xx             : %ld\n", total.count_4xx);
    printf("5xx             : %ld\n", total.count_5xx);

    for (int i = 0; i < total_ficheiros; i++) free(ficheiros[i]);
    free(ficheiros);

    return 0;
}