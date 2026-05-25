/* main_prodcons.c
 *
 * ALTERAÇÃO: Este ficheiro foi reescrito quase na totalidade.
 * O original tinha as seguintes falhas críticas:
 *   1. main() não criava nenhuma thread — inicializava mutex/semáforos e terminava.
 *   2. main() não tinha return, causando comportamento indefinido.
 *   3. producer() e consumer_routine() estavam definidos DEPOIS do main()
 *      sem protótipos, o que é inválido em C99/C11.
 *   4. O produtor lia em blocos com read() em vez de linha a linha,
 *      corrompendo os dados no buffer.
 *   5. O consumidor escrevia para "results.txt" hardcoded em vez de
 *      acumular métricas como o resto do projecto.
 *   6. A pílula venenosa só funcionava com 1 consumidor.
 *
 * Este main segue a mesma estrutura de main_threads.c:
 *   - CLI idêntica aos outros executáveis do projecto.
 *   - Descoberta de ficheiros com opendir/readdir.
 *   - Divisão de ficheiros entre produtores (igual à divisão de threads).
 *   - Lançamento de N produtores + M consumidores.
 *   - Join em todos, depois relatório final.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>

#include "parser.h"
#include "posix_io.h"
#include "worker_prodcons.h"

/* ALTERAÇÃO: Número de consumidores fixo em 2.
 * O enunciado não especifica — usamos 2 para demonstrar que o padrão
 * funciona com múltiplos consumidores em simultâneo. */
#define NUM_CONSUMERS 2

/* =========================================================
 * imprimir_relatorio — igual ao de main_threads.c
 * ========================================================= */
static void imprimir_relatorio(Metrics *total, char *modo) {
    posix_writef(STDOUT_FILENO, "\n=== RELATORIO FINAL PRODCONS (%s) ===\n", modo);
    posix_writef(STDOUT_FILENO, "Total de linhas  : %ld\n", total->total_lines);
    posix_writef(STDOUT_FILENO, "DEBUG            : %ld\n", total->count_debug);
    posix_writef(STDOUT_FILENO, "INFO             : %ld\n", total->count_info);
    posix_writef(STDOUT_FILENO, "WARNINGS         : %ld\n", total->count_warn);
    posix_writef(STDOUT_FILENO, "ERRORS           : %ld\n", total->count_error);
    posix_writef(STDOUT_FILENO, "CRITICAL         : %ld\n", total->count_critical);
    posix_writef(STDOUT_FILENO, "HTTP 4xx         : %ld\n", total->count_4xx);
    posix_writef(STDOUT_FILENO, "HTTP 5xx         : %ld\n", total->count_5xx);

    /* ALTERAÇÃO: Metrics usa ip_list/ip_count (não top_ips/top_ips_counts,
     * que são campos de WorkerResult). Ordenamos aqui para mostrar os top 10. */
    posix_writef(STDOUT_FILENO, "\n--- TOP 10 IPs ---\n");
    for (int i = 0; i < total->ip_num - 1; i++) {
        for (int j = 0; j < total->ip_num - i - 1; j++) {
            if (total->ip_count[j] < total->ip_count[j + 1]) {
                long tmp_c = total->ip_count[j];
                total->ip_count[j] = total->ip_count[j + 1];
                total->ip_count[j + 1] = tmp_c;
                char tmp_ip[IP_LEN];
                strncpy(tmp_ip, total->ip_list[j], IP_LEN);
                strncpy(total->ip_list[j], total->ip_list[j + 1], IP_LEN);
                strncpy(total->ip_list[j + 1], tmp_ip, IP_LEN);
            }
        }
    }
    int limite = total->ip_num < 10 ? total->ip_num : 10;
    for (int i = 0; i < limite; i++) {
        posix_writef(STDOUT_FILENO, "%2d. %s (%ld pedidos)\n",
                     i + 1, total->ip_list[i], total->ip_count[i]);
    }

    posix_writef(STDOUT_FILENO, "\n--- ALERTAS CRITICOS ---\n");
    if (total->num_alerts == 0) {
        posix_writef(STDOUT_FILENO, "Sem alertas criticos.\n");
    } else {
        for (int i = 0; i < total->num_alerts; i++)
            posix_writef(STDOUT_FILENO, "%2d. %s\n", i + 1, total->alerts[i]);
    }
    posix_writef(STDOUT_FILENO, "=================================\n");
}

/* =========================================================
 * main
 * ========================================================= */
int main(int argc, char *argv[]) {

    /* ALTERAÇÃO: CLI igual aos outros executáveis.
     * O original não recebia argumentos nenhuns. */
    if (argc < 4) {
        posix_writef(STDOUT_FILENO,
                     "Uso: %s <diretorio> <num_produtores> <modo> [--verbose]\n",
                     argv[0]);
        exit(1);
    }

    char *diretorio    = argv[1];
    int   num_prod     = atoi(argv[2]);
    char *modo         = argv[3];
    int   verbose      = 0;

    for (int i = 4; i < argc; i++)
        if (strcmp(argv[i], "--verbose") == 0) verbose = 1;

    /* Configurar modo do parser (security / traffic / performance / full) */
    if (parser_set_mode_from_string(modo) != 0) {
        posix_writef(STDERR_FILENO,
                     "Modo invalido: %s (use security|performance|traffic|full)\n", modo);
        exit(1);
    }

    /* ── 1. Descobrir ficheiros .log e .json no directório ── */
    int   capacidade = 10, total_ficheiros = 0;
    char **ficheiros = malloc(capacidade * sizeof(char *));
    if (!ficheiros) { perror("malloc"); exit(1); }

    DIR *dir = opendir(diretorio);
    if (!dir) { perror("opendir"); exit(1); }

    struct dirent *entrada;
    while ((entrada = readdir(dir)) != NULL) {
        int len = strlen(entrada->d_name);
        int e_log  = (len > 4 && strcmp(entrada->d_name + len - 4, ".log")  == 0);
        int e_json = (len > 5 && strcmp(entrada->d_name + len - 5, ".json") == 0);
        if (!e_log && !e_json) continue;

        if (total_ficheiros == capacidade) {
            capacidade *= 2;
            ficheiros = realloc(ficheiros, capacidade * sizeof(char *));
            if (!ficheiros) { perror("realloc"); exit(1); }
        }
        char caminho[512];
        snprintf(caminho, sizeof(caminho), "%s/%s", diretorio, entrada->d_name);
        ficheiros[total_ficheiros++] = strdup(caminho);
    }
    closedir(dir);

    if (total_ficheiros == 0) {
        posix_writef(STDOUT_FILENO, "Nenhum ficheiro .log ou .json encontrado.\n");
        free(ficheiros);
        exit(0);
    }

    /* Não faz sentido ter mais produtores do que ficheiros */
    if (num_prod > total_ficheiros) num_prod = total_ficheiros;

    posix_writef(STDOUT_FILENO,
                 "Ficheiros: %d | Produtores: %d | Consumidores: %d | Modo: %s\n\n",
                 total_ficheiros, num_prod, NUM_CONSUMERS, modo);

    /* ── 2. Inicializar o buffer circular e as métricas globais ── */

    /* ALTERAÇÃO: init_bounded_buffer() inicializa mutex e semáforos.
     * produtores_ativos é inicializado AQUI, depois de sabermos num_prod,
     * em vez de dentro de init_bounded_buffer(). */
    init_bounded_buffer();
    produtores_ativos = num_prod;   /* consumidores usam este valor para saber quando parar */

    Metrics global_metrics;
    init_metrics(&global_metrics);

    /* ── 3. Preparar argumentos e lançar threads produtoras ── */

    pthread_t    *prod_threads = malloc(num_prod * sizeof(pthread_t));
    ProducerArgs *prod_args    = malloc(num_prod * sizeof(ProducerArgs));
    if (!prod_threads || !prod_args) { perror("malloc"); exit(1); }

    /* Dividir ficheiros igualmente entre produtores (igual à divisão de threads) */
    int fich_por_prod = total_ficheiros / num_prod;

    for (int i = 0; i < num_prod; i++) {
        prod_args[i].ficheiros    = ficheiros;
        prod_args[i].inicio       = i * fich_por_prod;
        prod_args[i].fim          = (i == num_prod - 1) ? total_ficheiros
                                                        : prod_args[i].inicio + fich_por_prod;
        prod_args[i].worker_index = i;
        prod_args[i].verbose      = verbose;

        if (pthread_create(&prod_threads[i], NULL, run_producer, &prod_args[i]) != 0) {
            perror("pthread_create produtor");
            exit(1);
        }
    }

    /* ── 4. Preparar argumentos e lançar threads consumidoras ── */

    pthread_t    *cons_threads = malloc(NUM_CONSUMERS * sizeof(pthread_t));
    ConsumerArgs *cons_args    = malloc(NUM_CONSUMERS * sizeof(ConsumerArgs));
    if (!cons_threads || !cons_args) { perror("malloc"); exit(1); }

    for (int i = 0; i < NUM_CONSUMERS; i++) {
        cons_args[i].global_metrics = &global_metrics;
        cons_args[i].metrics_mutex  = &metrics_mutex;
        cons_args[i].worker_index   = i;

        if (pthread_create(&cons_threads[i], NULL, run_consumer, &cons_args[i]) != 0) {
            perror("pthread_create consumidor");
            exit(1);
        }
    }

    /* ── 5. Esperar que todos os produtores terminem ──
     * Os produtores terminam quando esgotam os seus ficheiros.
     * Ao terminar, cada um decrementa produtores_ativos e acorda os consumidores. */
    for (int i = 0; i < num_prod; i++)
        pthread_join(prod_threads[i], NULL);

    /* ── 6. Esperar que todos os consumidores terminem ──
     * Os consumidores saem quando produtores_ativos == 0 e buffer.count == 0.
     * O último produtor acorda-os (ver run_producer em worker_prodcons.c). */
    for (int i = 0; i < NUM_CONSUMERS; i++)
        pthread_join(cons_threads[i], NULL);

    /* ── 7. Destruir buffer e imprimir relatório ── */
    destroy_bounded_buffer();
    imprimir_relatorio(&global_metrics, modo);

    /* ── 8. Limpeza de memória ── */
    for (int i = 0; i < total_ficheiros; i++) free(ficheiros[i]);
    free(ficheiros);
    free(prod_threads);
    free(prod_args);
    free(cons_threads);
    free(cons_args);

    return 0;
}