#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>

#include "worker_threads.h"

#define MAX_THREADS 64

/* Variáveis Globais para o Dashboard Partilhado */
static long   g_lines_done[MAX_THREADS];
static long   g_lines_total[MAX_THREADS];
static int    g_num_workers  = 0;
static time_t g_start_time   = 0;
static volatile int g_all_done = 0; // Flag para parar a thread monitora
static int    g_dashboard_enabled = 0;

/* Função que desenha a interface (idêntica aos sockets) */
static void draw_dashboard(void) {
    int linhas = g_num_workers + 7;
    printf("\033[%dA", linhas); // Move o cursor para cima
    printf("\033[J");           // Limpa o ecrã abaixo do cursor

    time_t elapsed = time(NULL) - g_start_time;
    int hh = elapsed / 3600;
    int mm = (elapsed % 3600) / 60;
    int ss = elapsed % 60;

    long total_done  = 0;
    long total_total = 0;
    for (int i = 0; i < g_num_workers; i++) {
        total_done  += g_lines_done[i];
        total_total += g_lines_total[i];
    }
    int total_pct = (total_total > 0) ? (int)(total_done * 100 / total_total) : 0;
    if (total_pct > 100) total_pct = 100;

    printf("╔══════════════════════════════════════════╗\n");
    printf("║    LOG ANALYZER - THREADS MONITOR        ║\n");
    printf("╠══════════════════════════════════════════╣\n");

    for (int i = 0; i < g_num_workers; i++) {
        int pct = (g_lines_total[i] > 0) ? (int)(g_lines_done[i] * 100 / g_lines_total[i]) : 0;
        if (pct > 100) pct = 100;

        char bar[21];
        int filled = pct / 5;
        for (int b = 0; b < 20; b++) bar[b] = (b < filled) ? '#' : '.';
        bar[20] = '\0';

        printf("║ Thread %-2d [%s] %3d%%           ║\n", i + 1, bar, pct);
    }

    printf("╠══════════════════════════════════════════╣\n");

    char tot_bar[21];
    int tot_filled = total_pct / 5;
    for (int b = 0; b < 20; b++) tot_bar[b] = (b < tot_filled) ? '#' : '.';
    tot_bar[20] = '\0';

    printf("║ Total     [%s] %3d%%           ║\n", tot_bar, total_pct);
    printf("║ Elapsed: %02d:%02d:%02d                      ║\n", hh, mm, ss);
    printf("╚══════════════════════════════════════════╝\n");
}

/* Thread Monitora: Fica em loop a desenhar o dashboard até os workers acabarem */
void *run_monitor_thread(void *arg) {
    (void)arg;
    while (!g_all_done) {
        draw_dashboard();
        usleep(100000);
    }
    draw_dashboard(); // Desenha a última vez (aos 100%)
    pthread_exit(NULL);
}

void gerar_relatorio_threads(Metrics *total, char *modo, char *output_file) {
    int fd_out = STDOUT_FILENO;
    int fd_file = -1;

    if (output_file != NULL) {
        fd_file = open(output_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd_file >= 0) {
            fd_out = fd_file;
            printf("\n[INFO] A gravar relatorio no ficheiro: %s\n", output_file);
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

    if (write(fd_out, buffer, len) < 0) {
        perror("Erro ao escrever relatorio");
    }

    if (fd_file >= 0) close(fd_file);
}

int main(int argc, char *argv[]) {
    /* Dashboard usa ANSI + printf; sem TTY alguns runners deixam stdout fully-buffered. */
    setvbuf(stdout, NULL, _IONBF, 0);
    g_dashboard_enabled = isatty(STDOUT_FILENO);

    if (argc < 4) {
        printf("Uso: %s <diretorio> <num_threads> <modo> [--verbose] [--output=ficheiro.txt]\n", argv[0]);
        exit(1);
    }

    char *diretorio   = argv[1];
    int num_threads   = atoi(argv[2]);
    char *modo        = argv[3];

    int verbose = 0;
    char *output_file = NULL;

    for (int i = 4; i < argc; i++) {
        if (strcmp(argv[i], "--verbose") == 0) verbose = 1;
        else if (strncmp(argv[i], "--output=", 9) == 0) output_file = argv[i] + 9;
    }

    if (parser_set_mode_from_string(modo) != 0) {
        fprintf(stderr, "Modo invalido: %s (use security|performance|traffic|full)\n", modo);
        exit(1);
    }

    int capacidade = 10, total_ficheiros = 0;
    char **ficheiros = malloc(capacidade * sizeof(char *));
    DIR *dir = opendir(diretorio);
    if (!dir) { perror("opendir"); exit(1); }

    struct dirent *entrada;
    while ((entrada = readdir(dir)) != NULL) {
        int len = strlen(entrada->d_name);
        if (len > 4 && strcmp(entrada->d_name + len - 4, ".log") == 0) {
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
        printf("Nenhum ficheiro .log encontrado.\n");
        exit(0);
    }
    if (num_threads > total_ficheiros) num_threads = total_ficheiros;
    if (num_threads > MAX_THREADS) num_threads = MAX_THREADS;

    Metrics global_metrics;
    init_metrics(&global_metrics);
    pthread_mutex_t metrics_mutex;
    pthread_mutex_init(&metrics_mutex, NULL);

    pthread_t *threads = malloc(num_threads * sizeof(pthread_t));
    ThreadArgs *args = malloc(num_threads * sizeof(ThreadArgs));
    pthread_t monitor_thread; // A nossa Thread Extra para desenhar a interface!

    int ficheiros_por_thread = total_ficheiros / num_threads;

    g_num_workers = num_threads;
    g_start_time = time(NULL);
    memset(g_lines_done, 0, sizeof(g_lines_done));
    memset(g_lines_total, 0, sizeof(g_lines_total));
    g_all_done = 0;

    if (g_dashboard_enabled) {
        // Imprime linhas em branco suficientes para o dashboard não sobrescrever prints anteriores
        for (int i = 0; i < g_num_workers + 7; i++) printf("\n");

        /* 1. Lançar a Thread Monitora */
        pthread_create(&monitor_thread, NULL, run_monitor_thread, NULL);
    }

    /* 2. Lançar as Worker Threads */
    for (int i = 0; i < num_threads; i++) {
        args[i].ficheiros = ficheiros;
        args[i].inicio = i * ficheiros_por_thread;
        args[i].fim = (i == num_threads - 1) ? total_ficheiros : args[i].inicio + ficheiros_por_thread;
        args[i].worker_index = i; // Array começa no 0
        args[i].verbose = verbose;
        args[i].global_metrics = &global_metrics;
        args[i].mutex = &metrics_mutex;
        args[i].lines_done = &g_lines_done[i];     // Ponteiro para o partilhado
        args[i].lines_total = &g_lines_total[i];   // Ponteiro para o partilhado

        if (pthread_create(&threads[i], NULL, run_worker_thread, &args[i]) != 0) {
            perror("Erro ao criar thread");
            exit(1);
        }
    }

    /* 3. Esperar pelos Trabalhadores */
    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
    }

    if (g_dashboard_enabled) {
        /* 4. Trabalhadores acabaram! Avisar a Monitora e esperar por ela */
        g_all_done = 1;
        pthread_join(monitor_thread, NULL);
    }

    /* 5. Destruir o trinco e gerar relatório */
    pthread_mutex_destroy(&metrics_mutex);
    gerar_relatorio_threads(&global_metrics, modo, output_file);

    /* 6. Limpezas */
    for (int i = 0; i < total_ficheiros; i++) free(ficheiros[i]);
    free(ficheiros);
    free(threads);
    free(args);

    return 0;
}
