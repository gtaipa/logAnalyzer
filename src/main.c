#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>

#include "worker.h"
#include "ipc.h"

/* =========================================================
 * Variáveis globais para o dashboard
 * ========================================================= */
#define MAX_WORKERS 64

/* Estado de cada worker — atualizado pelo handler do SIGALRM */
static struct {
    pid_t pid;
    long  lines_done;
    long  lines_total;
} worker_status[MAX_WORKERS];

static int    g_num_workers   = 0;
static int    g_progress_fd   = -1;   /* fd de leitura do pipe de progresso */
static time_t g_start_time    = 0;
static long   g_total_errors  = 0;    /* atualizado com resultados finais   */

/* =========================================================
 * draw_dashboard  —  redesenha o dashboard no terminal
 * ========================================================= */
static void draw_dashboard(void) {
    /* Mover cursor para o início do dashboard */
    /* Limpa N+5 linhas para cima              */
    int linhas = g_num_workers + 5;
    printf("\033[%dA", linhas);   /* cursor N linhas acima    */
    printf("\033[J");             /* apaga do cursor até ao fim */

    time_t elapsed = time(NULL) - g_start_time;
    int hh = elapsed / 3600;
    int mm = (elapsed % 3600) / 60;
    int ss = elapsed % 60;

    /* Calcular progresso total */
    long total_done  = 0;
    long total_total = 0;
    for (int i = 0; i < g_num_workers; i++) {
        total_done  += worker_status[i].lines_done;
        total_total += worker_status[i].lines_total;
    }

    int total_pct = (total_total > 0)
                    ? (int)(total_done * 100 / total_total)
                    : 0;

    /* Cabeçalho */
    printf("╔══════════════════════════════════════════╗\n");
    printf("║       LOG ANALYZER - Real-time Monitor   ║\n");
    printf("╠══════════════════════════════════════════╣\n");

    /* Uma linha por worker */
    for (int i = 0; i < g_num_workers; i++) {
        int pct = (worker_status[i].lines_total > 0)
                  ? (int)(worker_status[i].lines_done * 100 / worker_status[i].lines_total)
                  : 0;
        if (pct > 100) pct = 100;

        /* Barra de 20 blocos */
        int filled = pct / 5;
        char bar[21];
        for (int b = 0; b < 20; b++)
            bar[b] = (b < filled) ? '#' : '.';
        bar[20] = '\0';

        printf("║ Worker %-2d [%s] %3d%%           ║\n",
               i + 1, bar, pct);
    }

    /* Linha de totais */
    printf("╠══════════════════════════════════════════╣\n");

    int tot_filled = total_pct / 5;
    char tot_bar[21];
    for (int b = 0; b < 20; b++)
        tot_bar[b] = (b < tot_filled) ? '#' : '.';
    tot_bar[20] = '\0';

    printf("║ Total     [%s] %3d%%           ║\n", tot_bar, total_pct);
    printf("║ Elapsed: %02d:%02d:%02d | Errors: %-6ld      ║\n",
           hh, mm, ss, g_total_errors);
    printf("╚══════════════════════════════════════════╝\n");
}

/* =========================================================
 * drain_progress_pipe  —  lê todos os updates disponíveis
 * (chamada antes de redesenhar)
 * ========================================================= */
static void drain_progress_pipe(void) {
    ProgressUpdate pu;
    ssize_t n;

    /* Leitura não bloqueante */
    while ((n = read(g_progress_fd, &pu, sizeof(pu))) == (ssize_t)sizeof(pu)) {
        int idx = pu.worker_index;
        if (idx >= 0 && idx < g_num_workers) {
            worker_status[idx].pid        = pu.pid;
            worker_status[idx].lines_done  = pu.lines_done;
            worker_status[idx].lines_total = pu.lines_total;
        }
    }
}

/* =========================================================
 * SIGALRM handler  —  atualiza dashboard a cada 1 segundo
 * ========================================================= */
static void sigalrm_handler(int sig) {
    (void)sig;
    drain_progress_pipe();
    draw_dashboard();
    alarm(1);   /* reagendar para daqui a 1 segundo */
}

/* =========================================================
 * main
 * ========================================================= */
int main(int argc, char *argv[]) {

    if (argc < 4) {
        printf("Uso: %s <diretorio> <num_processos> <modo> [--verbose]\n", argv[0]);
        printf("Exemplo: %s datasets/ 4 security --verbose\n", argv[0]);
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
    int capacidade = 10;
    int total_ficheiros = 0;
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
            if (verbose) printf("Encontrei: %s\n", ficheiros[total_ficheiros - 1]);
        }
    }
    closedir(dir);

    printf("Total de ficheiros .log encontrados: %d\n", total_ficheiros);

    if (total_ficheiros == 0) {
        printf("Nenhum ficheiro .log encontrado em %s\n", diretorio);
        exit(0);
    }

    if (num_processos > total_ficheiros) {
        num_processos = total_ficheiros;
        printf("Ajustado para %d processo(s)\n", num_processos);
    }

    /* ---- CRIAR PIPES ---- */
    int pipe_result[2];    /* resultados finais filho → pai  */
    int pipe_progress[2];  /* progresso filho → pai          */

    if (pipe(pipe_result)   < 0) { perror("pipe result");   exit(1); }
    if (pipe(pipe_progress) < 0) { perror("pipe progress"); exit(1); }

    /* Tornar leitura do pipe de progresso não bloqueante */
    int flags = fcntl(pipe_progress[0], F_GETFL, 0);
    fcntl(pipe_progress[0], F_SETFL, flags | O_NONBLOCK);

    /* Inicializar estado global do dashboard */
    g_num_workers = num_processos;
    g_progress_fd = pipe_progress[0];
    g_start_time  = time(NULL);
    memset(worker_status, 0, sizeof(worker_status));

    /* ---- CRIAR PROCESSOS FILHO ---- */
    int ficheiros_por_processo = total_ficheiros / num_processos;

    for (int i = 0; i < num_processos; i++) {
        pid_t pid = fork();

        if (pid < 0) { perror("fork"); exit(1); }

        if (pid == 0) {
            /* ---- FILHO ---- */
            close(pipe_result[0]);
            close(pipe_progress[0]);

            int inicio = i * ficheiros_por_processo;
            int fim    = (i == num_processos - 1)
                         ? total_ficheiros
                         : inicio + ficheiros_por_processo;

            run_worker(ficheiros, inicio, fim,
                       pipe_result[1], pipe_progress[1], i, verbose);

            close(pipe_result[1]);
            close(pipe_progress[1]);
            exit(0);
        }
    }

    /* ---- PAI ---- */
    close(pipe_result[1]);
    close(pipe_progress[1]);

    /* Imprimir linhas em branco para o dashboard ter espaço */
    for (int i = 0; i < g_num_workers + 5; i++) printf("\n");

    /* Instalar handler e arrancar o timer */
    signal(SIGALRM, sigalrm_handler);
    alarm(1);

    /* ---- LER RESULTADOS FINAIS ---- */
    WorkerResult resultado;
    WorkerResult total = {0};

    for (int i = 0; i < num_processos; i++) {
        ssize_t lidos = readn(pipe_result[0], &resultado, sizeof(resultado));
        if (lidos != (ssize_t)sizeof(resultado)) break;

        total.total_lines    += resultado.total_lines;
        total.count_debug    += resultado.count_debug;
        total.count_info     += resultado.count_info;
        total.count_warn     += resultado.count_warn;
        total.count_error    += resultado.count_error;
        total.count_critical += resultado.count_critical;
        total.count_4xx      += resultado.count_4xx;
        total.count_5xx      += resultado.count_5xx;
        g_total_errors        = total.count_error;
    }

    close(pipe_result[0]);

    /* Cancelar o alarm e fazer desenho final */
    alarm(0);
    drain_progress_pipe();
    close(pipe_progress[0]);
    draw_dashboard();

    /* Esperar pelos filhos */
    for (int i = 0; i < num_processos; i++) wait(NULL);

    /* Relatório final */
    printf("\n=== RELATORIO FINAL ===\n");
    printf("Total de linhas : %ld\n", total.total_lines);
    printf("DEBUG           : %ld\n", total.count_debug);
    printf("INFO            : %ld\n", total.count_info);
    printf("WARNINGS        : %ld\n", total.count_warn);
    printf("ERRORS          : %ld\n", total.count_error);
    printf("CRITICAL        : %ld\n", total.count_critical);
    printf("4xx             : %ld\n", total.count_4xx);
    printf("5xx             : %ld\n", total.count_5xx);

    /* Libertar memória */
    for (int i = 0; i < total_ficheiros; i++) free(ficheiros[i]);
    free(ficheiros);

    return 0;
}