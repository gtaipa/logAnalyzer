#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "posix_io.h"
#include "worker_threads.h"

#define MAX_THREADS 64

static long   g_bytes_done[MAX_THREADS];
static long   g_bytes_total[MAX_THREADS];
static int    g_num_workers  = 0;
static time_t g_start_time   = 0;
static volatile int g_all_done = 0;
static int    g_dashboard_enabled = 0;

static void draw_dashboard(void) {
    int linhas = g_num_workers + 7;
    posix_writef(STDOUT_FILENO, "\033[%dA", linhas);
    posix_writef(STDOUT_FILENO, "\033[J");

    time_t elapsed = time(NULL) - g_start_time;
    int hh = elapsed / 3600;
    int mm = (elapsed % 3600) / 60;
    int ss = elapsed % 60;

    long total_done  = 0;
    long total_total = 0;
    for (int i = 0; i < g_num_workers; i++) {
        total_done  += g_bytes_done[i];
        total_total += g_bytes_total[i];
    }
    int total_pct = (total_total > 0) ? (int)(total_done * 100 / total_total) : 0;
    if (total_pct > 100) total_pct = 100;

    posix_writef(STDOUT_FILENO, "╔══════════════════════════════════════════╗\n");
    posix_writef(STDOUT_FILENO, "║    LOG ANALYZER - THREADS MONITOR        ║\n");
    posix_writef(STDOUT_FILENO, "╠══════════════════════════════════════════╣\n");

    for (int i = 0; i < g_num_workers; i++) {
        int pct = (g_bytes_total[i] > 0) ? (int)(g_bytes_done[i] * 100 / g_bytes_total[i]) : 0;
        if (pct > 100) pct = 100;

        char bar[21];
        int filled = pct / 5;
        for (int b = 0; b < 20; b++) bar[b] = (b < filled) ? '#' : '.';
        bar[20] = '\0';

        posix_writef(STDOUT_FILENO, "║ Thread %-2d [%s] %3d%%           ║\n", i + 1, bar, pct);
    }

    posix_writef(STDOUT_FILENO, "╠══════════════════════════════════════════╣\n");

    char tot_bar[21];
    int tot_filled = total_pct / 5;
    for (int b = 0; b < 20; b++) tot_bar[b] = (b < tot_filled) ? '#' : '.';
    tot_bar[20] = '\0';

    posix_writef(STDOUT_FILENO, "║ Total     [%s] %3d%%           ║\n", tot_bar, total_pct);
    posix_writef(STDOUT_FILENO, "║ Elapsed: %02d:%02d:%02d                      ║\n", hh, mm, ss);
    posix_writef(STDOUT_FILENO, "╚══════════════════════════════════════════╝\n");
}

void *run_monitor_thread(void *arg) {
    (void)arg;
    while (!g_all_done) {
        draw_dashboard();
        usleep(100000);
    }
    draw_dashboard();
    pthread_exit(NULL);
}

void gerar_relatorio_threads(Metrics *total, char *modo, char *output_file) {
    int fd_out = STDOUT_FILENO;
    int fd_file = -1;

    if (output_file != NULL) {
        fd_file = open(output_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd_file >= 0) {
            fd_out = fd_file;
            posix_writef(STDOUT_FILENO, "\n[INFO] A gravar relatorio no ficheiro: %s\n", output_file);
        }
    }

    char buffer[4096];
    int len = 0;

    len += snprintf(buffer + len, sizeof(buffer) - len, "\n=== RELATORIO FINAL THREADS (%s) ===\n", modo);
    len += snprintf(buffer + len, sizeof(buffer) - len, "Total de linhas : %ld\n", total->total_lines);

    if (strcmp(modo, "security") == 0 || strcmp(modo, "full") == 0) {
        len += snprintf(buffer + len, sizeof(buffer) - len, "\n--- ALERTAS DE SEGURANCA ---\n");
        len += snprintf(buffer + len, sizeof(buffer) - len, "WARNINGS        : %ld\n", total->count_warn);
        len += snprintf(buffer + len, sizeof(buffer) - len, "ERRORS          : %ld\n", total->count_error);
        len += snprintf(buffer + len, sizeof(buffer) - len, "CRITICAL        : %ld\n", total->count_critical);
    }

    if (strcmp(modo, "traffic") == 0 || strcmp(modo, "full") == 0) {
        len += snprintf(buffer + len, sizeof(buffer) - len, "\n--- ESTATISTICAS DE TRAFEGO ---\n");
        len += snprintf(buffer + len, sizeof(buffer) - len, "INFO            : %ld\n", total->count_info);
        len += snprintf(buffer + len, sizeof(buffer) - len, "HTTP 4xx/5xx    : %ld\n", total->count_4xx + total->count_5xx);
    }

    len += snprintf(buffer + len, sizeof(buffer) - len, "=================================\n\n");

    if (write(fd_out, buffer, len) < 0) perror("Erro ao escrever relatorio");
    if (fd_file >= 0) close(fd_file);
}

int main(int argc, char *argv[]) {
    g_dashboard_enabled = isatty(STDOUT_FILENO);

    if (argc < 4) {
        posix_writef(STDOUT_FILENO, "Uso: %s <diretorio> <num_threads> <modo> [--verbose] [--output=ficheiro.txt]\n", argv[0]);
        exit(1);
    }

    char *diretorio = argv[1];
    int num_threads = atoi(argv[2]);
    char *modo      = argv[3];
    int verbose     = 0;
    char *output_file = NULL;

    for (int i = 4; i < argc; i++) {
        if (strcmp(argv[i], "--verbose") == 0) verbose = 1;
        else if (strncmp(argv[i], "--output=", 9) == 0) output_file = argv[i] + 9;
    }

    if (parser_set_mode_from_string(modo) != 0) {
        posix_writef(STDERR_FILENO, "Modo invalido: %s (use security|performance|traffic|full)\n", modo);
        exit(1);
    }

    /* ── 1. Descobrir ficheiros ── */
    int capacidade = 10, total_ficheiros = 0;
    char **ficheiros = malloc(capacidade * sizeof(char *));
    DIR *dir = opendir(diretorio);
    if (!dir) { perror("opendir"); exit(1); }

    struct dirent *entrada;
    while ((entrada = readdir(dir)) != NULL) {
        int len = strlen(entrada->d_name);
        if ((len > 4 && strcmp(entrada->d_name + len - 4, ".log")  == 0) ||
            (len > 5 && strcmp(entrada->d_name + len - 5, ".json") == 0)) {
            if (total_ficheiros == capacidade) {
                capacidade *= 2;
                ficheiros = realloc(ficheiros, capacidade * sizeof(char *));
            }
            char caminho[512];
            snprintf(caminho, sizeof(caminho), "%s/%s", diretorio, entrada->d_name);
            ficheiros[total_ficheiros++] = strdup(caminho);
        }
    }
    closedir(dir);

    if (total_ficheiros == 0) {
        posix_writef(STDOUT_FILENO, "Nenhum ficheiro .log ou .json encontrado.\n");
        exit(0);
    }

    if (num_threads > MAX_THREADS) num_threads = MAX_THREADS;

    /* ── 2. Calcular total de bytes e dividir em fatias iguais ── */
    struct stat st;
    off_t total_bytes = 0;
    for (int i = 0; i < total_ficheiros; i++) {
        if (stat(ficheiros[i], &st) == 0)
            total_bytes += st.st_size;
    }

    if (num_threads > total_ficheiros) num_threads = total_ficheiros;

    off_t bytes_por_thread = total_bytes / num_threads;

    /* ── 3. Inicializar estruturas ── */
    Metrics global_metrics;
    init_metrics(&global_metrics);
    pthread_mutex_t metrics_mutex;
    pthread_mutex_init(&metrics_mutex, NULL);

    pthread_t  *threads = malloc(num_threads * sizeof(pthread_t));
    ThreadArgs *args    = malloc(num_threads * sizeof(ThreadArgs));
    pthread_t   monitor_thread;

    g_num_workers = num_threads;
    g_start_time  = time(NULL);
    memset(g_bytes_done,  0, sizeof(g_bytes_done));
    memset(g_bytes_total, 0, sizeof(g_bytes_total));
    g_all_done = 0;

    if (g_dashboard_enabled) {
        for (int i = 0; i < g_num_workers + 7; i++) posix_writef(STDOUT_FILENO, "\n");
        pthread_create(&monitor_thread, NULL, run_monitor_thread, NULL);
    }

    /* ── 4. Lançar threads com fatias de bytes ── */
    for (int i = 0; i < num_threads; i++) {
        args[i].ficheiros       = ficheiros;
        args[i].total_ficheiros = total_ficheiros;
        args[i].byte_inicio     = (off_t)i * bytes_por_thread;
        args[i].byte_fim        = (i == num_threads - 1) ? total_bytes : (off_t)(i + 1) * bytes_por_thread;
        args[i].worker_index    = i;
        args[i].verbose         = verbose;
        args[i].global_metrics  = &global_metrics;
        args[i].mutex           = &metrics_mutex;
        args[i].bytes_done      = &g_bytes_done[i];
        args[i].bytes_total     = &g_bytes_total[i];

        if (pthread_create(&threads[i], NULL, run_worker_thread, &args[i]) != 0) {
            perror("Erro ao criar thread");
            exit(1);
        }
    }

    /* ── 5. Esperar pelas threads ── */
    for (int i = 0; i < num_threads; i++)
        pthread_join(threads[i], NULL);

    if (g_dashboard_enabled) {
        g_all_done = 1;
        pthread_join(monitor_thread, NULL);
    }

    pthread_mutex_destroy(&metrics_mutex);

    long elapsed = (long)(time(NULL) - g_start_time);
    gerar_relatorio_threads(&global_metrics, modo, output_file);
    posix_writef(STDOUT_FILENO, "Tempo de processamento: %ldmin %02lds\n",
                 elapsed / 60, elapsed % 60);

    for (int i = 0; i < total_ficheiros; i++) free(ficheiros[i]);
    free(ficheiros);
    free(threads);
    free(args);

    return 0;
}