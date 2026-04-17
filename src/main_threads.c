#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>

#include "worker_threads.h"

void gerar_relatorio_threads(Metrics *total, char *modo, char *output_file) {
    FILE *out = stdout;
    FILE *f = NULL;

    if (output_file != NULL) {
        f = fopen(output_file, "w");
        if (f) {
            out = f;
            printf("\n[INFO] A gravar relatorio no ficheiro: %s\n", output_file);
        }
    }

    fprintf(out, "\n=== RELATORIO FINAL THREADS (%s) ===\n", modo);
    fprintf(out, "Total de linhas : %ld\n", total->total_lines);

    if (strcmp(modo, "security") == 0 || strcmp(modo, "full") == 0) {
        fprintf(out, "\n--- ALERTAS DE SEGURANCA ---\n");
        fprintf(out, "WARNINGS        : %ld\n", total->count_warn);
        fprintf(out, "ERRORS          : %ld\n", total->count_error);
        fprintf(out, "CRITICAL        : %ld\n", total->count_critical);
    }
    
    if (strcmp(modo, "traffic") == 0 || strcmp(modo, "full") == 0) {
        fprintf(out, "\n--- ESTATISTICAS DE TRAFEGO ---\n");
        fprintf(out, "INFO            : %ld\n", total->count_info);
        fprintf(out, "HTTP 4xx/5xx    : %ld\n", total->count_4xx + total->count_5xx);
    }

    fprintf(out, "=================================\n");
    if (f) fclose(f);
}

int main(int argc, char *argv[]) {
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

    /* 1. Descobrir ficheiros */
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

    /* 2. Preparar Variáveis Partilhadas (O Quadro Branco na Cozinha) */
    Metrics global_metrics;
    init_metrics(&global_metrics);
    
    pthread_mutex_t metrics_mutex;
    pthread_mutex_init(&metrics_mutex, NULL); // Inicializar o trinco

    pthread_t *threads = malloc(num_threads * sizeof(pthread_t));
    ThreadArgs *args = malloc(num_threads * sizeof(ThreadArgs));

    int ficheiros_por_thread = total_ficheiros / num_threads;

    /* 3. Lançar as Threads (pthread_create) */
    printf("[MAIN] A lançar %d worker threads...\n", num_threads);
    for (int i = 0; i < num_threads; i++) {
        args[i].ficheiros = ficheiros;
        args[i].inicio = i * ficheiros_por_thread;
        args[i].fim = (i == num_threads - 1) ? total_ficheiros : args[i].inicio + ficheiros_por_thread;
        args[i].worker_index = i + 1;
        args[i].verbose = verbose;
        args[i].global_metrics = &global_metrics;
        args[i].mutex = &metrics_mutex;

        if (pthread_create(&threads[i], NULL, run_worker_thread, &args[i]) != 0) {
            perror("Erro ao criar thread");
            exit(1);
        }
    }

    /* 4. Esperar que todas terminem (pthread_join em vez de waitpid) */
    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
    }

    printf("[MAIN] Todas as threads terminaram!\n");

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