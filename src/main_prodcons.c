/**
 * @file main_prodcons.c
 * @brief Ponto de entrada do analisador de logs com o padrão Produtor-Consumidor.
 *
 * @details
 * Este ficheiro implementa a orquestração completa do padrão
 * **Produtor-Consumidor com bounded buffer**:
 *
 *  - **Produtores** (threads `run_producer`): cada produtor recebe uma fatia
 *    (intervalo de bytes) dos ficheiros de log e insere `LogEntry` no buffer
 *    circular partilhado, usando o semáforo `empty` para garantir que não
 *    escreve quando o buffer está cheio.
 *
 *  - **Consumidores** (threads `run_consumer`): retiram entradas do buffer e
 *    actualizam as métricas globais, usando o semáforo `full` para bloquear
 *    enquanto o buffer estiver vazio.
 *
 *  - O buffer circular tem capacidade fixa `BUFFER_SIZE = 30`.
 *    Os índices `in`, `out` e o contador `count` avançam em módulo
 *    `BUFFER_SIZE`, garantindo a reutilização cíclica das posições.
 *
 *  - A variável global `produtores_ativos` (protegida pelo mutex do buffer)
 *    permite que o último produtor a terminar sinalize a todos os consumidores
 *    que não chegará mais nenhuma entrada, evitando que fiquem bloqueados
 *    indefinidamente em `sem_wait(&buffer.full)`.
 *
 *  - Um monitor dedicado (`run_monitor_thread`) actualiza o dashboard no
 *    terminal a cada 100 ms enquanto os produtores trabalham.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "parser.h"
#include "posix_io.h"
#include "worker_prodcons.h"

/* Número máximo de produtores suportados */
#define MAX_WORKERS 64

/* Arrays de progresso — um slot por produtor, indexado pelo worker_index */
static long   g_bytes_done[MAX_WORKERS];   /**< Bytes já processados por cada produtor */
static long   g_bytes_total[MAX_WORKERS];  /**< Total de bytes atribuído a cada produtor */
static int    g_num_workers  = 0;          /**< Número efectivo de produtores lançados */
static time_t g_start_time   = 0;          /**< Instante em que main() iniciou os threads */
static volatile int g_all_done = 0;        /**< Flag: 1 quando todos os produtores terminaram (sinaliza o monitor para parar) */

/* Número fixo de threads consumidoras */
#define NUM_CONSUMERS 5

/**
 * @brief Imprime o relatório final de métricas para o stdout.
 *
 * @details
 * A secção a imprimir depende do argumento `modo`:
 *  - `"security"`   → contagens de níveis DEBUG/INFO/WARN/ERROR/CRITICAL
 *  - `"performance"` → contagens de HTTP 4xx/5xx
 *  - `"traffic"`    → contagens de HTTP 4xx/5xx (reutiliza os mesmos campos)
 *  - `"full"`       → todas as secções anteriores
 *
 * O top-10 de IPs é ordenado por bubble sort directamente sobre os arrays
 * dentro de `m`, sem alocações adicionais.
 *
 * @param m    Apontador para a estrutura `Metrics` com os totais acumulados
 *             por todos os consumidores.
 * @param modo String que selecciona que secções do relatório são impressas.
 */
static void imprimir_relatorio(Metrics *m, char *modo) {
    posix_writef(STDOUT_FILENO, "\n=== RELATORIO FINAL PRODCONS (%s) ===\n", modo);
    posix_writef(STDOUT_FILENO, "Total de linhas  : %ld\n", m->total_lines);

    /* Secção de segurança: mostra a distribuição de níveis de severidade */
    if (strcmp(modo, "security") == 0 || strcmp(modo, "full") == 0) {
        posix_writef(STDOUT_FILENO, "\n--- EVENTOS DE SEGURANCA ---\n");
        posix_writef(STDOUT_FILENO, "DEBUG            : %ld\n", m->count_debug);
        posix_writef(STDOUT_FILENO, "INFO             : %ld\n", m->count_info);
        posix_writef(STDOUT_FILENO, "WARNINGS         : %ld\n", m->count_warn);
        posix_writef(STDOUT_FILENO, "ERRORS           : %ld\n", m->count_error);
        posix_writef(STDOUT_FILENO, "CRITICAL         : %ld\n", m->count_critical);
    }

    /* Secção de performance: mostra erros HTTP do lado do cliente e do servidor */
    if (strcmp(modo, "performance") == 0 || strcmp(modo, "full") == 0) {
        posix_writef(STDOUT_FILENO, "\n--- PERFORMANCE ---\n");
        posix_writef(STDOUT_FILENO, "HTTP 4xx         : %ld\n", m->count_4xx);
        posix_writef(STDOUT_FILENO, "HTTP 5xx         : %ld\n", m->count_5xx);
    }

    /* Secção de tráfego: mostra os mesmos contadores de HTTP */
    if (strcmp(modo, "traffic") == 0 || strcmp(modo, "full") == 0) {
        posix_writef(STDOUT_FILENO, "\n--- TRAFEGO ---\n");
        posix_writef(STDOUT_FILENO, "HTTP 4xx         : %ld\n", m->count_4xx);
        posix_writef(STDOUT_FILENO, "HTTP 5xx         : %ld\n", m->count_5xx);
    }

    posix_writef(STDOUT_FILENO, "\n--- TOP 10 IPs ---\n");

    /*
     * Ordenação por bubble sort dos IPs por contagem descendente.
     * O sort é feito in-place nos arrays ip_count[] e ip_list[] da estrutura
     * Metrics — troca simultaneamente o contador e a string correspondente.
     */
    for (int i = 0; i < m->ip_num - 1; i++) {
        for (int j = 0; j < m->ip_num - i - 1; j++) {
            if (m->ip_count[j] < m->ip_count[j + 1]) {
                /* Trocar contadores */
                long tmp_c = m->ip_count[j];
                m->ip_count[j] = m->ip_count[j + 1];
                m->ip_count[j + 1] = tmp_c;
                /* Trocar strings de IP correspondentes */
                char tmp_ip[IP_LEN];
                strncpy(tmp_ip, m->ip_list[j], IP_LEN);
                strncpy(m->ip_list[j], m->ip_list[j + 1], IP_LEN);
                strncpy(m->ip_list[j + 1], tmp_ip, IP_LEN);
            }
        }
    }

    /* Limitar a saída aos primeiros 10 (ou menos, se houver menos IPs únicos) */
    int limite = m->ip_num < 10 ? m->ip_num : 10;
    if (limite == 0) {
        posix_writef(STDOUT_FILENO, "Nenhum IP encontrado.\n");
    } else {
        for (int i = 0; i < limite; i++)
            posix_writef(STDOUT_FILENO, "%2d. %-16s (%ld pedidos)\n",
                         i + 1, m->ip_list[i], m->ip_count[i]);
    }

    posix_writef(STDOUT_FILENO, "\n--- ALERTAS CRITICOS ---\n");
    if (m->num_alerts == 0) {
        posix_writef(STDOUT_FILENO, "Sem alertas criticos.\n");
    } else {
        for (int i = 0; i < m->num_alerts; i++)
            posix_writef(STDOUT_FILENO, "%2d. %s\n", i + 1, m->alerts[i]);
    }

    posix_writef(STDOUT_FILENO, "=================================\n");
}

/**
 * @brief Redesenha o dashboard de progresso no terminal.
 *
 * @details
 * Usa sequências de escape ANSI para subir `linhas` linhas e limpar a área,
 * depois imprime uma barra de progresso por produtor e uma barra global.
 * É chamada periodicamente pelo thread monitor e uma última vez depois de
 * `g_all_done = 1` para mostrar 100% antes do relatório.
 *
 * Não recebe parâmetros — lê directamente as variáveis globais
 * `g_bytes_done`, `g_bytes_total`, `g_num_workers` e `g_start_time`.
 */
static void draw_dashboard(void) {
    /* Subir o cursor o número de linhas ocupado pelo dashboard anterior */
    int linhas = g_num_workers + 8;
    posix_writef(STDOUT_FILENO, "\033[%dA", linhas);  /* ESC[<n>A — cursor up */
    posix_writef(STDOUT_FILENO, "\033[J");             /* ESC[J   — limpar até ao fim do ecrã */

    /* Calcular progresso global somando bytes de todos os produtores */
    long total_done  = 0;
    long total_total = 0;
    for (int i = 0; i < g_num_workers; i++) {
        total_done  += g_bytes_done[i];
        total_total += g_bytes_total[i];
    }
    int total_pct = (total_total > 0) ? (int)(total_done * 100 / total_total) : 0;
    if (total_pct > 100) total_pct = 100;  /* Garantir que não ultrapassa 100% */

    posix_writef(STDOUT_FILENO, "╔══════════════════════════════════════════╗\n");
    posix_writef(STDOUT_FILENO, "║   LOG ANALYZER - PRODCONS MONITOR        ║\n");
    posix_writef(STDOUT_FILENO, "╠══════════════════════════════════════════╣\n");

    /* Uma linha por produtor com barra de progresso individual */
    for (int i = 0; i < g_num_workers; i++) {
        int pct = (g_bytes_total[i] > 0)
                  ? (int)(g_bytes_done[i] * 100 / g_bytes_total[i]) : 0;
        if (pct > 100) pct = 100;

        /* Barra de 20 caracteres: '#' preenchido, '.' vazio */
        char bar[21];
        int filled = pct / 5;  /* Cada '#' representa 5% */
        for (int b = 0; b < 20; b++) bar[b] = (b < filled) ? '#' : '.';
        bar[20] = '\0';

        posix_writef(STDOUT_FILENO, "║ Produtor %-2d [%s] %3d%%        ║\n",
                     i + 1, bar, pct);
    }

    posix_writef(STDOUT_FILENO, "╠══════════════════════════════════════════╣\n");

    /* Barra de progresso global (soma de todos os produtores) */
    char tot_bar[21];
    int tot_filled = total_pct / 5;
    for (int b = 0; b < 20; b++) tot_bar[b] = (b < tot_filled) ? '#' : '.';
    tot_bar[20] = '\0';

    /* Calcular tempo decorrido desde o início */
    time_t elapsed = time(NULL) - g_start_time;
    int hh = (int)(elapsed / 3600);
    int mm = (int)((elapsed % 3600) / 60);
    int ss = (int)(elapsed % 60);

    posix_writef(STDOUT_FILENO, "║ Total     [%s] %3d%%           ║\n",
                 tot_bar, total_pct);
    posix_writef(STDOUT_FILENO, "║ Elapsed: %02d:%02d:%02d                      ║\n",
                 hh, mm, ss);
    posix_writef(STDOUT_FILENO, "╚══════════════════════════════════════════╝\n");
}

/**
 * @brief Thread dedicado à actualização periódica do dashboard.
 *
 * @details
 * Corre em loop enquanto `g_all_done` for 0, redesenhando o dashboard a cada
 * 100 ms (100 000 µs). Quando `g_all_done` passa a 1 (após todos os
 * produtores terminarem), faz um último redesenho a 100% e termina.
 *
 * @param arg  Não utilizado (NULL).
 * @return     NULL (convenção pthread).
 */
void *run_monitor_thread(void *arg) {
    (void)arg;  /* Suprimir aviso de parâmetro não usado */
    while (!g_all_done) {
        draw_dashboard();
        usleep(100000);  /* Pausa de 100 ms entre redesenhos */
    }
    /* Redesenho final para mostrar 100% após todos os produtores terminarem */
    draw_dashboard();
    pthread_exit(NULL);
}

/**
 * @brief Ponto de entrada do programa.
 *
 * @details
 * Fluxo de execução:
 *  1. Validar argumentos e configurar o modo de análise.
 *  2. Varrer o directório e recolher todos os ficheiros `.log` e `.json`.
 *  3. Calcular o total de bytes e dividir em fatias iguais (uma por produtor).
 *  4. Inicializar o **bounded buffer** (`init_bounded_buffer`) e definir
 *     `produtores_ativos = num_prod`.
 *  5. Lançar o thread monitor e os threads produtores.
 *  6. Lançar os threads consumidores.
 *  7. Aguardar (`pthread_join`) todos os produtores; sinalizar o monitor para
 *     parar; aguardar todos os consumidores.
 *  8. Destruir o buffer, imprimir o relatório e libertar memória.
 *
 * @param argc  Número de argumentos da linha de comandos.
 * @param argv  Vetor de argumentos:
 *              `argv[1]` — directório com os ficheiros de log,
 *              `argv[2]` — número de produtores,
 *              `argv[3]` — modo (`security` | `performance` | `traffic` | `full`),
 *              `argv[4]` — (opcional) `--verbose`.
 * @return 0 em caso de sucesso; 1 em caso de erro.
 */
int main(int argc, char *argv[]) {
    if (argc < 4) {
        posix_writef(STDOUT_FILENO,
                     "Uso: %s <diretorio> <num_produtores> <modo> [--verbose]\n",
                     argv[0]);
        exit(1);
    }

    char *diretorio = argv[1];
    int   num_prod  = atoi(argv[2]);
    char *modo      = argv[3];
    int   verbose   = 0;

    /* Verificar se o utilizador pediu saída detalhada */
    for (int i = 4; i < argc; i++)
        if (strcmp(argv[i], "--verbose") == 0) verbose = 1;

    /* Configurar o parser com o modo escolhido; abortar se for inválido */
    if (parser_set_mode_from_string(modo) != 0) {
        posix_writef(STDERR_FILENO,
                     "Modo invalido: %s (use security|performance|traffic|full)\n", modo);
        exit(1);
    }

    /* ── 1. Descobrir ficheiros ── */
    int   capacidade = 10, total_ficheiros = 0;
    char **ficheiros = malloc(capacidade * sizeof(char *));
    if (!ficheiros) { perror("malloc"); exit(1); }

    DIR *dir = opendir(diretorio);
    if (!dir) { perror("opendir"); exit(1); }

    /* Iterar as entradas do directório e guardar os caminhos completos */
    struct dirent *entrada;
    while ((entrada = readdir(dir)) != NULL) {
        int len = strlen(entrada->d_name);
        int e_log  = (len > 4 && strcmp(entrada->d_name + len - 4, ".log")  == 0);
        int e_json = (len > 5 && strcmp(entrada->d_name + len - 5, ".json") == 0);
        if (!e_log && !e_json) continue;  /* Ignorar ficheiros que não sejam .log ou .json */

        /* Redimensionar o array dinamicamente se necessário (crescimento x2) */
        if (total_ficheiros == capacidade) {
            capacidade *= 2;
            ficheiros = realloc(ficheiros, capacidade * sizeof(char *));
            if (!ficheiros) { perror("realloc"); exit(1); }
        }
        char caminho[512];
        snprintf(caminho, sizeof(caminho), "%s/%s", diretorio, entrada->d_name);
        ficheiros[total_ficheiros++] = strdup(caminho);  /* Cópia própria do caminho */
    }
    closedir(dir);

    if (total_ficheiros == 0) {
        posix_writef(STDOUT_FILENO, "Nenhum ficheiro .log ou .json encontrado.\n");
        free(ficheiros);
        exit(0);
    }

    /* Limitar o número de produtores ao máximo suportado */
    if (num_prod > MAX_WORKERS) num_prod = MAX_WORKERS;

    /* ── 2. Calcular total de bytes e dividir em fatias iguais ── */
    /*
     * Cada produtor fica responsável por uma fatia de [byte_inicio, byte_fim[
     * do espaço de endereçamento virtual dos ficheiros concatenados.
     * O produtor trata de mapear essa fatia para os ficheiros reais.
     */
    struct stat st;
    off_t total_bytes = 0;
    for (int i = 0; i < total_ficheiros; i++) {
        if (stat(ficheiros[i], &st) == 0)
            total_bytes += st.st_size;
    }

    off_t bytes_por_prod = total_bytes / num_prod;  /* Fatia de bytes por produtor */

    posix_writef(STDOUT_FILENO,
                 "Ficheiros: %d | Produtores: %d | Consumidores: %d | Modo: %s\n\n",
                 total_ficheiros, num_prod, NUM_CONSUMERS, modo);

    /* ── 3. Inicializar buffer e métricas ── */
    /*
     * init_bounded_buffer() inicializa:
     *   - buffer.in = buffer.out = buffer.count = 0
     *   - pthread_mutex_init(&buffer.mutex)      → exclusão mútua no acesso ao buffer
     *   - sem_init(&buffer.empty, 0, BUFFER_SIZE) → conta espaços livres (começa cheio de espaços)
     *   - sem_init(&buffer.full,  0, 0)           → conta entradas disponíveis (começa a zero)
     *
     * produtores_ativos é usado pelos consumidores para saber quando parar:
     * quando chega a 0 e o buffer está vazio, não chegará mais nenhuma entrada.
     */
    init_bounded_buffer();
    produtores_ativos = num_prod;  /* Número de produtores em execução */

    Metrics global_metrics;
    init_metrics(&global_metrics);  /* Zerrar todos os contadores das métricas */

    g_num_workers = num_prod;
    g_start_time  = time(NULL);
    g_all_done    = 0;
    memset(g_bytes_done,  0, sizeof(g_bytes_done));
    memset(g_bytes_total, 0, sizeof(g_bytes_total));

    /* Reservar linhas no terminal para o dashboard poder redesenhar no lugar */
    for (int i = 0; i < g_num_workers + 8; i++)
        posix_writef(STDOUT_FILENO, "\n");

    /* Lançar o thread monitor que actualizará o dashboard a cada 100 ms */
    pthread_t monitor_thread;
    pthread_create(&monitor_thread, NULL, run_monitor_thread, NULL);

    /* ── 4. Lançar produtores com fatias de bytes ── */
    /*
     * Cada ProducerArgs define a fatia [byte_inicio, byte_fim[ que o produtor
     * deve processar. O último produtor recebe os bytes restantes para cobrir
     * eventuais erros de arredondamento da divisão inteira.
     */
    pthread_t    *prod_threads = malloc(num_prod * sizeof(pthread_t));
    ProducerArgs *prod_args    = malloc(num_prod * sizeof(ProducerArgs));
    if (!prod_threads || !prod_args) { perror("malloc"); exit(1); }

    for (int i = 0; i < num_prod; i++) {
        prod_args[i].ficheiros       = ficheiros;
        prod_args[i].total_ficheiros = total_ficheiros;
        prod_args[i].byte_inicio     = (off_t)i * bytes_por_prod;
        /* O último produtor estende a fatia até ao fim para cobrir o resto */
        prod_args[i].byte_fim        = (i == num_prod - 1) ? total_bytes
                                                            : (off_t)(i + 1) * bytes_por_prod;
        prod_args[i].worker_index    = i;
        prod_args[i].verbose         = verbose;
        prod_args[i].bytes_done      = &g_bytes_done[i];   /* Ponteiro para o slot de progresso */
        prod_args[i].bytes_total     = &g_bytes_total[i];

        if (pthread_create(&prod_threads[i], NULL, run_producer, &prod_args[i]) != 0) {
            perror("pthread_create produtor");
            exit(1);
        }
    }

    /* ── 5. Lançar consumidores ── */
    /*
     * Todos os consumidores partilham a mesma estrutura global_metrics,
     * protegida por metrics_mutex para evitar condições de corrida nas
     * actualizações dos contadores.
     */
    pthread_t    *cons_threads = malloc(NUM_CONSUMERS * sizeof(pthread_t));
    ConsumerArgs *cons_args    = malloc(NUM_CONSUMERS * sizeof(ConsumerArgs));
    if (!cons_threads || !cons_args) { perror("malloc"); exit(1); }

    for (int i = 0; i < NUM_CONSUMERS; i++) {
        cons_args[i].global_metrics = &global_metrics;
        cons_args[i].metrics_mutex  = &metrics_mutex;  /* Mutex partilhado para as métricas */
        cons_args[i].worker_index   = i;

        if (pthread_create(&cons_threads[i], NULL, run_consumer, &cons_args[i]) != 0) {
            perror("pthread_create consumidor");
            exit(1);
        }
    }

    /* ── 6. Esperar produtores, parar dashboard, esperar consumidores ── */
    /*
     * Ordem de join deliberada:
     *  a) Esperar todos os produtores → garantir que inseriram tudo no buffer.
     *  b) Sinalizar g_all_done = 1 e esperar o monitor → dashboard final a 100%.
     *  c) Esperar consumidores → garantir que processaram todas as entradas.
     *     (Os consumidores terminam sozinhos quando vêem buffer.count==0 &&
     *      produtores_ativos==0, sinalizado pelo último produtor.)
     */
    for (int i = 0; i < num_prod; i++)
        pthread_join(prod_threads[i], NULL);

    g_all_done = 1;                          /* Avisar o monitor para parar */
    pthread_join(monitor_thread, NULL);

    for (int i = 0; i < NUM_CONSUMERS; i++)
        pthread_join(cons_threads[i], NULL);

    /* ── 7. Relatório final ── */
    destroy_bounded_buffer();  /* Libertar mutex e semáforos do buffer */

    long elapsed = (long)(time(NULL) - g_start_time);
    imprimir_relatorio(&global_metrics, modo);
    posix_writef(STDOUT_FILENO, "Tempo de processamento: %ldmin %02lds\n",
                 elapsed / 60, elapsed % 60);

    /* Libertar toda a memória alocada dinamicamente */
    for (int i = 0; i < total_ficheiros; i++) free(ficheiros[i]);
    free(ficheiros);
    free(prod_threads);
    free(prod_args);
    free(cons_threads);
    free(cons_args);

    return 0;
}
