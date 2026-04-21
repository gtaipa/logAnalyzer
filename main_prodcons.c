#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>

#include "worker_prodcons.h"
#include "parser.h"

int produtores_ativos = 1;

void gerar_relatorio_prodcons(Metrics *total, char *modo, char *output_file) {
    FILE *out = stdout;
    FILE *f = NULL;

    if (output_file != NULL) {
        f = fopen(output_file, "w");
        if (f) {
            out = f;
            printf("\n[INFO] A gravar relatorio no ficheiro: %s\n", output_file);
        }
    }

    fprintf(out, "\n=== RELATORIO FINAL PRODUTOR-CONSUMIDOR (%s) ===\n", modo);
    fprintf(out, "Total de linhas : %ld\n", total->total_lines);

    if (strcmp(modo, "security") == 0 || strcmp(modo, "full") == 0) {
        fprintf(out, "\n--- ALERTAS DE SEGURANCA ---\n");
        fprintf(out, "DEBUG           : %ld\n", total->count_debug);
        fprintf(out, "INFO            : %ld\n", total->count_info);
        fprintf(out, "WARNINGS        : %ld\n", total->count_warn);
        fprintf(out, "ERRORS          : %ld\n", total->count_error);
        fprintf(out, "CRITICAL        : %ld\n", total->count_critical);
    }
    
    if (strcmp(modo, "traffic") == 0 || strcmp(modo, "full") == 0) {
        fprintf(out, "\n--- ESTATISTICAS DE TRAFEGO ---\n");
        fprintf(out, "HTTP 4xx        : %ld\n", total->count_4xx);
        fprintf(out, "HTTP 5xx        : %ld\n", total->count_5xx);
    }

    if (strcmp(modo, "full") == 0) {
        fprintf(out, "\n--- TOP IPs ---\n");
        for (int i = 0; i < total->ip_num && i < 10; i++) {
            fprintf(out, "%s : %ld accesos\n", total->ip_list[i], total->ip_count[i]);
        }
    }

    fprintf(out, "================================================\n");
    if (f) fclose(f);
}

int main(int argc, char *argv[]) {
    if (argc < 4) {
        printf("Uso: %s <diretorio> <num_produtores> <num_consumidores> <modo> [--verbose] [--output=ficheiro.txt]\n",
               argv[0]);
        exit(1);
    }

    char *diretorio      = argv[1];
    int num_produtores   = atoi(argv[2]);
    int num_consumidores = atoi(argv[3]);
    char *modo           = argv[4];

    int verbose = 0;
    char *output_file = NULL;

    for (int i = 5; i < argc; i++) {
        if (strcmp(argv[i], "--verbose") == 0) verbose = 1;
        else if (strncmp(argv[i], "--output=", 9) == 0) output_file = argv[i] + 9;
    }

    int capacidade = 10, total_ficheiros = 0;
    char **ficheiros = malloc(capacidade * sizeof(char *));
    DIR *dir = opendir(diretorio);
    if (!dir) {
        perror("opendir");
        exit(1);
    }

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
        printf("Nenhum ficheiro .log encontrado em %s.\n", diretorio);
        free(ficheiros);
        exit(0);
    }

    if (num_produtores > total_ficheiros) num_produtores = total_ficheiros;

    printf("[MAIN] Descobertos %d ficheiros de log.\n", total_ficheiros);
    printf("[MAIN] A usar %d produtores e %d consumidores.\n",
           num_produtores, num_consumidores);

    BoundedBuffer buffer;
    init_bounded_buffer(&buffer);

    Metrics global_metrics;
    init_metrics(&global_metrics);
    
    pthread_mutex_t metrics_mutex;
    pthread_mutex_init(&metrics_mutex, NULL);

    pthread_t *producer_threads = malloc(num_produtores * sizeof(pthread_t));
    pthread_t *consumer_threads = malloc(num_consumidores * sizeof(pthread_t));

    ProducerConsumerArgs *prod_args = malloc(num_produtores * sizeof(ProducerConsumerArgs));
    ProducerConsumerArgs *cons_args = malloc(num_consumidores * sizeof(ProducerConsumerArgs));

    printf("[MAIN] A lançar %d threads produtoras...\n", num_produtores);

    int ficheiros_por_produtor = total_ficheiros / num_produtores;
    produtores_ativos = 1;

    for (int i = 0; i < num_produtores; i++) {
        prod_args[i].ficheiros = ficheiros;
        prod_args[i].inicio = i * ficheiros_por_produtor;
        prod_args[i].fim = (i == num_produtores - 1)
            ? total_ficheiros
            : prod_args[i].inicio + ficheiros_por_produtor;
        prod_args[i].worker_index = i + 1;
        prod_args[i].verbose = verbose;
        prod_args[i].buffer = &buffer;
        prod_args[i].global_metrics = NULL;
        prod_args[i].metrics_mutex = NULL;

        if (pthread_create(&producer_threads[i], NULL, run_producer, &prod_args[i]) != 0) {
            perror("Erro ao criar thread produtora");
            exit(1);
        }
    }

    printf("[MAIN] A lançar %d threads consumidoras...\n", num_consumidores);

    for (int i = 0; i < num_consumidores; i++) {
        cons_args[i].ficheiros = NULL;
        cons_args[i].inicio = 0;
        cons_args[i].fim = 0;
        cons_args[i].worker_index = i + 1;
        cons_args[i].verbose = verbose;
        cons_args[i].buffer = &buffer;
        cons_args[i].global_metrics = &global_metrics;
        cons_args[i].metrics_mutex = &metrics_mutex;

        if (pthread_create(&consumer_threads[i], NULL, run_consumer, &cons_args[i]) != 0) {
            perror("Erro ao criar thread consumidora");
            exit(1);
        }
    }

    printf("[MAIN] Aguardando que os produtores terminem...\n");

    for (int i = 0; i < num_produtores; i++) {
        pthread_join(producer_threads[i], NULL);
    }

    printf("[MAIN] Todos os produtores terminaram!\n");

    pthread_mutex_lock(&buffer.mutex);
    produtores_ativos = 0;
    pthread_cond_broadcast(&buffer.cond_not_empty);
    pthread_mutex_unlock(&buffer.mutex);

    printf("[MAIN] Sinalizou aos consumidores: fim de turno dos produtores!\n");

    printf("[MAIN] Aguardando que os consumidores terminem...\n");

    for (int i = 0; i < num_consumidores; i++) {
        pthread_join(consumer_threads[i], NULL);
    }

    printf("[MAIN] Todos os consumidores terminaram!\n");

    pthread_mutex_destroy(&metrics_mutex);
    destroy_bounded_buffer(&buffer);

    gerar_relatorio_prodcons(&global_metrics, modo, output_file);

    for (int i = 0; i < total_ficheiros; i++) free(ficheiros[i]);
    free(ficheiros);
    free(producer_threads);
    free(consumer_threads);
    free(prod_args);
    free(cons_args);

    printf("[MAIN] Programa terminou com sucesso!\n");
    return 0;
}
