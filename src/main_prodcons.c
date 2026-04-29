#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <pthread.h>
#include <semaphore.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>

#include "worker_prodcons.h"
#include "parser.h"

#define MAX_FICHEIROS 100
#define MAX_CAMINHO 256
#define MAX_PRODUTORES 64
#define MAX_CONSUMIDORES 64

static char ficheiros_storage[MAX_FICHEIROS][MAX_CAMINHO];
char *ficheiros[MAX_FICHEIROS];
int total_ficheiros = 0;
int num_produtores = 0;
int verbose = 0;

LogEntry buffer[BUFFER_SIZE];
int prodptr = 0;
int consptr = 0;
int buffer_count = 0;
int produtores_ativos = 1;

Metrics global_metrics;
pthread_mutex_t trinco;
pthread_mutex_t metrics_mutex;
sem_t vagas;
sem_t itens;

static pthread_t producer_threads[MAX_PRODUTORES];
static pthread_t consumer_threads[MAX_CONSUMIDORES];
static pthread_t alert_thread;
static int producer_ids[MAX_PRODUTORES];
static int consumer_ids[MAX_CONSUMIDORES];

void gerar_relatorio_prodcons(Metrics *total, char *modo, char *output_file) {
    int fd_out = STDOUT_FILENO; 
    int fd_file = -1;

    if (output_file != NULL) {
        // Abre (ou cria) o ficheiro usando a system call POSIX
        fd_file = open(output_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd_file >= 0) {
            fd_out = fd_file; // Passamos a escrever para este descritor
            printf("\n[INFO] A gravar relatorio no ficheiro: %s\n", output_file);
        } else {
            perror("Erro ao abrir ficheiro de output");
        }
    }

    char report_buffer[4096]; 
    int len = 0;

    // Constrói a string no buffer na memória
    len += snprintf(report_buffer + len, sizeof(report_buffer) - len, "\n=== RELATORIO FINAL PRODUTOR-CONSUMIDOR (%s) ===\n", modo);
    len += snprintf(report_buffer + len, sizeof(report_buffer) - len, "Total de linhas : %ld\n", total->total_lines);

    if (strcmp(modo, "security") == 0 || strcmp(modo, "full") == 0) {
        len += snprintf(report_buffer + len, sizeof(report_buffer) - len, "\n--- ALERTAS DE SEGURANCA ---\n");
        len += snprintf(report_buffer + len, sizeof(report_buffer) - len, "DEBUG           : %ld\n", total->count_debug);
        len += snprintf(report_buffer + len, sizeof(report_buffer) - len, "INFO            : %ld\n", total->count_info);
        len += snprintf(report_buffer + len, sizeof(report_buffer) - len, "WARNINGS        : %ld\n", total->count_warn);
        len += snprintf(report_buffer + len, sizeof(report_buffer) - len, "ERRORS          : %ld\n", total->count_error);
        len += snprintf(report_buffer + len, sizeof(report_buffer) - len, "CRITICAL        : %ld\n", total->count_critical);
    }
    
    if (strcmp(modo, "traffic") == 0 || strcmp(modo, "full") == 0) {
        len += snprintf(report_buffer + len, sizeof(report_buffer) - len, "\n--- ESTATISTICAS DE TRAFEGO ---\n");
        len += snprintf(report_buffer + len, sizeof(report_buffer) - len, "HTTP 4xx        : %ld\n", total->count_4xx);
        len += snprintf(report_buffer + len, sizeof(report_buffer) - len, "HTTP 5xx        : %ld\n", total->count_5xx);
    }

    if (strcmp(modo, "full") == 0) {
        len += snprintf(report_buffer + len, sizeof(report_buffer) - len, "\n--- TOP IPs ---\n");
        for (int i = 0; i < total->ip_num && i < 10; i++) {
            len += snprintf(report_buffer + len, sizeof(report_buffer) - len, "%s : %ld accesos\n", total->ip_list[i], total->ip_count[i]);
        }
    }

    len += snprintf(report_buffer + len, sizeof(report_buffer) - len, "================================================\n");

    // Faz um único write com todo o conteúdo!
    if (write(fd_out, report_buffer, len) < 0) {
        perror("Erro ao escrever relatorio");
    }

    if (fd_file >= 0) close(fd_file);
}

int main(int argc, char *argv[]) {
    if (argc < 5) {
        printf("Uso: %s <diretorio> <num_produtores> <num_consumidores> <modo> [--verbose] [--output=ficheiro.txt]\n",
               argv[0]);
        exit(1);
    }

    char *diretorio      = argv[1];
    num_produtores       = atoi(argv[2]);
    int num_consumidores = atoi(argv[3]);
    char *modo           = argv[4];

    if (num_produtores < 1 || num_consumidores < 1) {
        fprintf(stderr, "Erro: num_produtores e num_consumidores tem de ser >= 1.\n");
        fprintf(stderr, "Uso: %s <diretorio> <num_produtores> <num_consumidores> <modo> [--verbose] [--output=ficheiro.txt]\n",
                argv[0]);
        exit(1);
    }

    verbose = 0;
    char *output_file = NULL;

    for (int i = 5; i < argc; i++) {
        if (strcmp(argv[i], "--verbose") == 0) verbose = 1;
        else if (strncmp(argv[i], "--output=", 9) == 0) output_file = argv[i] + 9;
    }

    if (parser_set_mode_from_string(modo) != 0) {
        fprintf(stderr, "Modo invalido: %s (use security|performance|traffic|full)\n", modo);
        exit(1);
    }

    if (num_produtores > MAX_PRODUTORES) {
        printf("[MAIN] Numero de produtores ajustado para %d (limite fixo).\n", MAX_PRODUTORES);
        num_produtores = MAX_PRODUTORES;
    }

    if (num_consumidores > MAX_CONSUMIDORES) {
        printf("[MAIN] Numero de consumidores ajustado para %d (limite fixo).\n", MAX_CONSUMIDORES);
        num_consumidores = MAX_CONSUMIDORES;
    }

    total_ficheiros = 0;
    DIR *dir = opendir(diretorio);
    if (!dir) {
        perror("opendir");
        exit(1);
    }

    struct dirent *entrada;
    while ((entrada = readdir(dir)) != NULL) {
        int len = strlen(entrada->d_name);
        if (len > 4 && strcmp(entrada->d_name + len - 4, ".log") == 0) {
            if (total_ficheiros == MAX_FICHEIROS) {
                fprintf(stderr, "[WARN] Limite de %d ficheiros atingido; restantes .log ignorados.\n", MAX_FICHEIROS);
                break;
            }

            int n = snprintf(
                ficheiros_storage[total_ficheiros],
                sizeof(ficheiros_storage[total_ficheiros]),
                "%s/%s",
                diretorio,
                entrada->d_name
            );

            if (n < 0 || n >= (int)sizeof(ficheiros_storage[total_ficheiros])) {
                fprintf(stderr, "[WARN] Caminho demasiado longo, ignorado: %s/%s\n", diretorio, entrada->d_name);
                continue;
            }

            ficheiros[total_ficheiros] = ficheiros_storage[total_ficheiros];
            total_ficheiros++;
        }
    }
    closedir(dir);

    if (total_ficheiros == 0) {
        printf("Nenhum ficheiro .log encontrado em %s.\n", diretorio);
        exit(0);
    }

    if (num_produtores > total_ficheiros) num_produtores = total_ficheiros;

    printf("[MAIN] Descobertos %d ficheiros de log.\n", total_ficheiros);
    printf("[MAIN] A usar %d produtores e %d consumidores.\n",
           num_produtores, num_consumidores);

    init_bounded_buffer();
    init_alert_buffer();

    init_metrics(&global_metrics);

    if (pthread_create(&alert_thread, NULL, run_alert_monitor, NULL) != 0) {
        perror("Erro ao criar thread de alertas");
        destroy_alert_buffer();
        destroy_bounded_buffer();
        exit(1);
    }
    
    printf("[MAIN] A lançar %d threads produtoras...\n", num_produtores);

    produtores_ativos = 1;

    for (int i = 0; i < num_produtores; i++) {
        producer_ids[i] = i + 1;

        if (pthread_create(&producer_threads[i], NULL, run_producer, &producer_ids[i]) != 0) {
            perror("Erro ao criar thread produtora");
            exit(1);
        }
    }

    printf("[MAIN] A lançar %d threads consumidoras...\n", num_consumidores);

    for (int i = 0; i < num_consumidores; i++) {
        consumer_ids[i] = i + 1;

        if (pthread_create(&consumer_threads[i], NULL, run_consumer, &consumer_ids[i]) != 0) {
            perror("Erro ao criar thread consumidora");
            exit(1);
        }
    }

    printf("[MAIN] Aguardando que os produtores terminem...\n");

    for (int i = 0; i < num_produtores; i++) {
        pthread_join(producer_threads[i], NULL);
    }

    printf("[MAIN] Todos os produtores terminaram!\n");

    pthread_mutex_lock(&trinco);
    produtores_ativos = 0;
    pthread_mutex_unlock(&trinco);

    for (int i = 0; i < num_consumidores; i++) {
        sem_post(&itens);
    }

    printf("[MAIN] Sinalizou aos consumidores: fim de turno dos produtores!\n");

    printf("[MAIN] Aguardando que os consumidores terminem...\n");

    for (int i = 0; i < num_consumidores; i++) {
        pthread_join(consumer_threads[i], NULL);
    }

    printf("[MAIN] Todos os consumidores terminaram!\n");

    pthread_mutex_lock(&alert_trinco);
    consumidores_ativos = 0;
    pthread_mutex_unlock(&alert_trinco);
    sem_post(&alert_itens);

    pthread_join(alert_thread, NULL);

    destroy_alert_buffer();
    destroy_bounded_buffer();

    gerar_relatorio_prodcons(&global_metrics, modo, output_file);

    printf("[MAIN] Programa terminou com sucesso!\n");
    return 0;
}
