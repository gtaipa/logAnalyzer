/**
 * @file main_threads.c
 * @brief Ponto de entrada do analisador de logs com suporte a múltiplas threads.
 *
 * @details
 * Este ficheiro contém o `main()` do programa e a lógica de orquestração de threads.
 * O fluxo geral é:
 *  1. Descobrir todos os ficheiros `.log` e `.json` no diretório fornecido.
 *  2. Calcular o tamanho total em bytes e dividir esse espaço em N fatias iguais,
 *     uma por thread worker.
 *  3. Lançar N threads worker (via `pthread_create`) — cada uma processa a sua
 *     fatia de bytes e funde as métricas locais nas globais usando um mutex.
 *  4. Lançar 1 thread monitor que redesenha o dashboard no terminal a cada 100 ms.
 *  5. Aguardar o fim de todas as threads (via `pthread_join`) e imprimir o
 *     relatório final.
 *
 * Padrão de concorrência utilizado:
 *  - **Worker threads**: processam dados em paralelo; cada thread tem as suas
 *    próprias métricas locais, evitando contenção durante o processamento.
 *    Só no momento de fusão é que acedem à estrutura global, protegida por mutex.
 *  - **Monitor thread**: lê apenas os contadores de progresso (`g_bytes_done` /
 *    `g_bytes_total`), que são atualizados atomicamente por cada worker. A thread
 *    monitor não precisa de mutex porque lê valores individuais de longa
 *    dimensão que, na prática, são atualizados de forma atómica em x86-64.
 *
 * Ciclo de vida das threads neste programa:
 * @code
 *   main()
 *     │
 *     ├─ pthread_create → monitor_thread  (1 thread)
 *     │       │
 *     │       └─ loop: draw_dashboard() + usleep(100ms) até g_all_done
 *     │
 *     ├─ pthread_create → worker_thread[0]  ─┐
 *     ├─ pthread_create → worker_thread[1]   │ correm em paralelo
 *     │  ...                                 │
 *     └─ pthread_create → worker_thread[N-1]─┘
 *             │
 *             └─ cada uma: processa fatia → mutex_lock → funde → mutex_unlock
 *                          → pthread_exit(NULL)
 *
 *   main() bloqueia em pthread_join(worker[0..N-1])
 *   main() seta g_all_done = 1
 *   main() bloqueia em pthread_join(monitor_thread)
 *   main() → gerar_relatorio_threads() → exit
 * @endcode
 */

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

/** Número máximo de threads worker suportadas pelo programa. */
#define MAX_THREADS 64

/**
 * @brief Bytes processados por cada thread worker (índice = worker_index).
 *
 * Escrito pelas threads worker e lido pela thread monitor para calcular
 * a percentagem de progresso de cada thread. O acesso é feito sem mutex
 * porque cada thread worker escreve apenas na sua própria posição do array,
 * eliminando conflitos de escrita simultânea.
 */
static long   g_bytes_done[MAX_THREADS];

/**
 * @brief Total de bytes atribuídos a cada thread worker (quota da fatia).
 *
 * Preenchido pelo `main()` antes de criar as threads e lido (só para leitura)
 * pela thread monitor. Não há escrita concorrente depois da inicialização.
 */
static long   g_bytes_total[MAX_THREADS];

/** Número de threads worker efetivamente criadas nesta execução. */
static int    g_num_workers  = 0;

/** Instante (epoch) em que o processamento foi iniciado — usado pelo dashboard. */
static time_t g_start_time   = 0;

/**
 * @brief Sinaliza às threads que todo o processamento terminou.
 *
 * Declarado `volatile` para que o compilador não otimize a leitura do valor
 * dentro do ciclo da thread monitor: a variável é escrita pelo `main()` e lida
 * pela thread monitor em contextos de execução diferentes.
 */
static volatile int g_all_done = 0;

/**
 * @brief Indica se o terminal suporta a renderização do dashboard ANSI.
 *
 * Vale 1 se `STDOUT` for um TTY (verificado com `isatty`). Se a saída for
 * redireccionada para um ficheiro, o dashboard é desativado.
 */
static int    g_dashboard_enabled = 0;

/* ─────────────────────────────────────────────────────────────────────────── */
/*  Dashboard                                                                   */
/* ─────────────────────────────────────────────────────────────────────────── */

/**
 * @brief Redesenha o painel de progresso (dashboard) no terminal.
 *
 * @details
 * A função usa sequências de escape ANSI para subir o cursor `linhas` linhas
 * (`\033[<N>A`) e apagar tudo a partir daí (`\033[J`), redesenhando assim o
 * painel no mesmo lugar sem criar scroll. Para cada thread worker é apresentada
 * uma barra de progresso com 20 caracteres e a percentagem calculada como
 * `bytes_done / bytes_total * 100`.
 *
 * Esta função é chamada exclusivamente pela thread monitor, não havendo
 * necessidade de proteção por mutex pois:
 *  - lê `g_bytes_done[]` e `g_bytes_total[]` — cada posição é escrita por uma
 *    única thread worker (sem concorrência de escrita).
 *  - escreve apenas em `STDOUT` com `posix_writef`, chamada sequencial dentro
 *    da thread monitor.
 */
static void draw_dashboard(void) {
    /* Número de linhas que o dashboard ocupa: uma por worker + 7 linhas de moldura */
    int linhas = g_num_workers + 7;

    /* Subir o cursor para o topo do dashboard e limpar a área */
    posix_writef(STDOUT_FILENO, "\033[%dA", linhas);
    posix_writef(STDOUT_FILENO, "\033[J");

    /* Calcular tempo decorrido desde o início do processamento */
    time_t elapsed = time(NULL) - g_start_time;
    int hh = elapsed / 3600;
    int mm = (elapsed % 3600) / 60;
    int ss = elapsed % 60;

    /* Acumular bytes globais para a barra total */
    long total_done  = 0;
    long total_total = 0;
    for (int i = 0; i < g_num_workers; i++) {
        total_done  += g_bytes_done[i];
        total_total += g_bytes_total[i];
    }
    /* Calcular percentagem global; proteger contra divisão por zero */
    int total_pct = (total_total > 0) ? (int)(total_done * 100 / total_total) : 0;
    if (total_pct > 100) total_pct = 100;

    /* Cabeçalho do dashboard */
    posix_writef(STDOUT_FILENO, "╔══════════════════════════════════════════╗\n");
    posix_writef(STDOUT_FILENO, "║    LOG ANALYZER - THREADS MONITOR        ║\n");
    posix_writef(STDOUT_FILENO, "╠══════════════════════════════════════════╣\n");

    /* Uma linha por worker com a barra de progresso individual */
    for (int i = 0; i < g_num_workers; i++) {
        /* Percentagem desta thread; clampar a [0, 100] */
        int pct = (g_bytes_total[i] > 0) ? (int)(g_bytes_done[i] * 100 / g_bytes_total[i]) : 0;
        if (pct > 100) pct = 100;

        /* Construir a barra: '#' para preenchido, '.' para vazio (20 colunas) */
        char bar[21];
        int filled = pct / 5;  /* cada '#' representa 5% */
        for (int b = 0; b < 20; b++) bar[b] = (b < filled) ? '#' : '.';
        bar[20] = '\0';

        posix_writef(STDOUT_FILENO, "║ Thread %-2d [%s] %3d%%           ║\n", i + 1, bar, pct);
    }

    posix_writef(STDOUT_FILENO, "╠══════════════════════════════════════════╣\n");

    /* Barra de progresso global (soma de todas as threads) */
    char tot_bar[21];
    int tot_filled = total_pct / 5;
    for (int b = 0; b < 20; b++) tot_bar[b] = (b < tot_filled) ? '#' : '.';
    tot_bar[20] = '\0';

    posix_writef(STDOUT_FILENO, "║ Total     [%s] %3d%%           ║\n", tot_bar, total_pct);
    posix_writef(STDOUT_FILENO, "║ Elapsed: %02d:%02d:%02d                      ║\n", hh, mm, ss);
    posix_writef(STDOUT_FILENO, "╚══════════════════════════════════════════╝\n");
}

/* ─────────────────────────────────────────────────────────────────────────── */
/*  Thread monitor                                                              */
/* ─────────────────────────────────────────────────────────────────────────── */

/**
 * @brief Função de entrada da thread monitor do dashboard.
 *
 * @param arg Não utilizado (NULL). Declarado como `(void)arg` para suprimir
 *            aviso do compilador.
 * @return NULL — a thread termina com `pthread_exit(NULL)`, o que é equivalente
 *         a retornar NULL mas torna explícita a terminação de uma thread POSIX.
 *
 * @details
 * Esta thread corre em paralelo com as N threads worker. O seu único objetivo
 * é redesenhar o dashboard a cada 100 ms (`usleep(100000)`) enquanto
 * `g_all_done` for 0. Quando o `main()` define `g_all_done = 1` (após o
 * `pthread_join` de todos os workers), a thread monitor sai do ciclo, faz um
 * último redesenho para mostrar 100% e termina.
 *
 * Padrão de thread monitor:
 * @code
 *   while (!flag_de_termino) {
 *       atualizar_ui();
 *       dormir_um_intervalo();
 *   }
 *   atualizar_ui();  // atualização final
 *   pthread_exit(NULL);
 * @endcode
 */
void *run_monitor_thread(void *arg) {
    (void)arg;  /* parâmetro não utilizado nesta thread */

    /* Ciclo principal: redesenhar enquanto as workers ainda estiverem a correr */
    while (!g_all_done) {
        draw_dashboard();
        usleep(100000);  /* esperar 100 ms antes de redesenhar (10 fps) */
    }

    /* Redesenho final para garantir que o dashboard mostra 100% */
    draw_dashboard();

    /*
     * pthread_exit: termina a thread corrente de forma controlada.
     * Permite que pthread_join no main() desbloqueie após esta chamada.
     * Diferença para return: pthread_exit chama os destruidores de chaves
     * de thread-local storage antes de terminar.
     */
    pthread_exit(NULL);
}

/* ─────────────────────────────────────────────────────────────────────────── */
/*  Relatório final                                                             */
/* ─────────────────────────────────────────────────────────────────────────── */

/**
 * @brief Gera o relatório final de análise e escreve-o no stdout ou num ficheiro.
 *
 * @param total       Ponteiro para as métricas globais já fundidas por todas
 *                    as threads worker.
 * @param modo        String com o modo de análise: "security", "traffic",
 *                    "performance" ou "full".
 * @param output_file Caminho para o ficheiro de saída. Se NULL, o relatório é
 *                    escrito em `STDOUT`.
 *
 * @details
 * A função constrói o relatório em memória (buffer de 4 KB) e escreve-o de
 * uma vez com `write()`. Se `output_file` não for NULL e puder ser aberto,
 * o relatório é redireccionado para esse ficheiro; caso contrário, cai no
 * stdout. As secções de alertas e tráfego só são incluídas se o `modo`
 * corresponder.
 */
void gerar_relatorio_threads(Metrics *total, char *modo, char *output_file) {
    int fd_out = STDOUT_FILENO;  /* destino por omissão: stdout */
    int fd_file = -1;

    /*
     * Se foi especificado um ficheiro de saída, tentar criá-lo/truncá-lo.
     * O_WRONLY: abrir apenas para escrita.
     * O_CREAT:  criar se não existir.
     * O_TRUNC:  truncar (apagar conteúdo) se já existir.
     * 0644:     permissões — dono lê+escreve, grupo e outros só lêem.
     */
    if (output_file != NULL) {
        fd_file = open(output_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd_file >= 0) {
            fd_out = fd_file;  /* redirecionar saída para o ficheiro */
            posix_writef(STDOUT_FILENO, "\n[INFO] A gravar relatorio no ficheiro: %s\n", output_file);
        }
        /* Se open falhar (disco cheio, permissões, …), fd_out mantém-se STDOUT */
    }

    /*
     * Construir o relatório em memória (buffer estático de 4 KiB) antes de
     * escrever. Esta abordagem evita múltiplas chamadas a write() e reduz a
     * fragmentação da saída. snprintf com `buffer + len` vai preenchendo o
     * buffer de forma segura, nunca escrevendo além dos limites.
     */
    char buffer[4096];
    int len = 0;

    /* Cabeçalho do relatório com o modo de análise usado */
    len += snprintf(buffer + len, sizeof(buffer) - len, "\n=== RELATORIO FINAL THREADS (%s) ===\n", modo);
    len += snprintf(buffer + len, sizeof(buffer) - len, "Total de linhas : %ld\n", total->total_lines);

    /*
     * Secção de alertas de segurança.
     * Presente nos modos "security" e "full" — filtra contagens de severidade
     * WARN, ERROR e CRITICAL encontradas nos logs.
     * `total` já contém a fusão de todas as threads (protegida pelo mutex
     * durante a fase de fusão em worker_threads.c).
     */
    if (strcmp(modo, "security") == 0 || strcmp(modo, "full") == 0) {
        len += snprintf(buffer + len, sizeof(buffer) - len, "\n--- ALERTAS DE SEGURANCA ---\n");
        len += snprintf(buffer + len, sizeof(buffer) - len, "WARNINGS        : %ld\n", total->count_warn);
        len += snprintf(buffer + len, sizeof(buffer) - len, "ERRORS          : %ld\n", total->count_error);
        len += snprintf(buffer + len, sizeof(buffer) - len, "CRITICAL        : %ld\n", total->count_critical);
    }

    /*
     * Secção de estatísticas de tráfego HTTP.
     * Presente nos modos "traffic" e "full" — apresenta mensagens INFO e
     * erros de cliente/servidor (HTTP 4xx + 5xx somados).
     */
    if (strcmp(modo, "traffic") == 0 || strcmp(modo, "full") == 0) {
        len += snprintf(buffer + len, sizeof(buffer) - len, "\n--- ESTATISTICAS DE TRAFEGO ---\n");
        len += snprintf(buffer + len, sizeof(buffer) - len, "INFO            : %ld\n", total->count_info);
        len += snprintf(buffer + len, sizeof(buffer) - len, "HTTP 4xx/5xx    : %ld\n", total->count_4xx + total->count_5xx);
    }

    len += snprintf(buffer + len, sizeof(buffer) - len, "=================================\n\n");

    /*
     * Escrever o relatório completo de uma só vez com write() POSIX.
     * Uma única chamada a write() é preferível a várias printf() porque:
     *  - é atómica para tamanhos inferiores a PIPE_BUF (4 KiB no Linux);
     *  - evita intercalação com output de outras fontes (e.g., stderr).
     */
    if (write(fd_out, buffer, len) < 0) perror("Erro ao escrever relatorio");

    /* Fechar o descritor do ficheiro de saída se foi aberto nesta função */
    if (fd_file >= 0) close(fd_file);
}

/* ─────────────────────────────────────────────────────────────────────────── */
/*  main                                                                        */
/* ─────────────────────────────────────────────────────────────────────────── */

/**
 * @brief Ponto de entrada do programa — orquestra todo o pipeline multithread.
 *
 * @param argc Número de argumentos da linha de comandos.
 * @param argv Vetor de argumentos:
 *             - argv[1]: diretório com os ficheiros de log
 *             - argv[2]: número de threads worker a criar
 *             - argv[3]: modo de análise (security|performance|traffic|full)
 *             - argv[4..]: flags opcionais --verbose e --output=<ficheiro>
 * @return 0 em caso de sucesso; 1 em caso de erro.
 *
 * @details
 * Fluxo de execução:
 *  1. Validar argumentos e configurar modo do parser.
 *  2. Varrer o diretório e recolher caminhos de ficheiros `.log`/`.json`.
 *  3. Calcular total de bytes e determinar a fatia de cada thread.
 *  4. Inicializar métricas globais e o mutex que as protege.
 *  5. Criar a thread monitor (se em TTY) e as N threads worker.
 *  6. Aguardar o fim das workers com `pthread_join`.
 *  7. Sinalizar a thread monitor, aguardá-la e destruir o mutex.
 *  8. Imprimir o relatório e libertar memória.
 */
int main(int argc, char *argv[]) {
    /* Verificar se o stdout é um terminal (TTY) para ativar o dashboard ANSI */
    g_dashboard_enabled = isatty(STDOUT_FILENO);

    /* Validação mínima de argumentos obrigatórios */
    if (argc < 4) {
        posix_writef(STDOUT_FILENO, "Uso: %s <diretorio> <num_threads> <modo> [--verbose] [--output=ficheiro.txt]\n", argv[0]);
        exit(1);
    }

    char *diretorio = argv[1];
    int num_threads = atoi(argv[2]);
    char *modo      = argv[3];
    int verbose     = 0;
    char *output_file = NULL;

    /* Processar flags opcionais: --verbose e --output=<caminho> */
    for (int i = 4; i < argc; i++) {
        if (strcmp(argv[i], "--verbose") == 0) verbose = 1;
        else if (strncmp(argv[i], "--output=", 9) == 0) output_file = argv[i] + 9;
    }

    /* Configurar o parser com o modo escolhido; abortar se inválido */
    if (parser_set_mode_from_string(modo) != 0) {
        posix_writef(STDERR_FILENO, "Modo invalido: %s (use security|performance|traffic|full)\n", modo);
        exit(1);
    }

    /* ── 1. Descobrir ficheiros ── */
    int capacidade = 10, total_ficheiros = 0;
    char **ficheiros = malloc(capacidade * sizeof(char *));
    DIR *dir = opendir(diretorio);
    if (!dir) { perror("opendir"); exit(1); }

    /* Iterar as entradas do diretório e recolher apenas .log e .json */
    struct dirent *entrada;
    while ((entrada = readdir(dir)) != NULL) {
        int len = strlen(entrada->d_name);
        if ((len > 4 && strcmp(entrada->d_name + len - 4, ".log")  == 0) ||
            (len > 5 && strcmp(entrada->d_name + len - 5, ".json") == 0)) {
            /* Crescer o array dinamicamente se necessário (duplicar capacidade) */
            if (total_ficheiros == capacidade) {
                capacidade *= 2;
                ficheiros = realloc(ficheiros, capacidade * sizeof(char *));
            }
            char caminho[512];
            snprintf(caminho, sizeof(caminho), "%s/%s", diretorio, entrada->d_name);
            ficheiros[total_ficheiros++] = strdup(caminho);  /* cópia própria do caminho */
        }
    }
    closedir(dir);

    if (total_ficheiros == 0) {
        posix_writef(STDOUT_FILENO, "Nenhum ficheiro .log ou .json encontrado.\n");
        exit(0);
    }

    /* Garantir que não excedemos o limite de threads */
    if (num_threads > MAX_THREADS) num_threads = MAX_THREADS;

    /* ── 2. Calcular total de bytes e dividir em fatias iguais ── */
    struct stat st;
    off_t total_bytes = 0;
    for (int i = 0; i < total_ficheiros; i++) {
        if (stat(ficheiros[i], &st) == 0)
            total_bytes += st.st_size;
    }

    /* Não faz sentido ter mais threads do que ficheiros */
    if (num_threads > total_ficheiros) num_threads = total_ficheiros;

    /*
     * Divisão do espaço de endereçamento de bytes em fatias consecutivas:
     *   Thread 0  → [0,            bytes_por_thread)
     *   Thread 1  → [bytes_por_thread, 2*bytes_por_thread)
     *   ...
     *   Thread N-1→ [N-1)*bytes_por_thread, total_bytes)   ← última apanha o resto
     *
     * Este padrão garante que todos os bytes são processados sem sobreposição.
     */
    off_t bytes_por_thread = total_bytes / num_threads;

    /* ── 3. Inicializar estruturas ── */
    Metrics global_metrics;
    init_metrics(&global_metrics);

    /*
     * Mutex para proteger global_metrics durante a fase de fusão.
     * Sem mutex, duas threads a escrever simultaneamente em global_metrics
     * causariam uma race condition: incrementos perdidos e dados corrompidos.
     * pthread_mutex_init com NULL usa atributos por omissão (mutex normal).
     */
    pthread_mutex_t metrics_mutex;
    pthread_mutex_init(&metrics_mutex, NULL);

    pthread_t  *threads = malloc(num_threads * sizeof(pthread_t));
    ThreadArgs *args    = malloc(num_threads * sizeof(ThreadArgs));
    pthread_t   monitor_thread;

    /* Inicializar variáveis globais de progresso antes de criar as threads */
    g_num_workers = num_threads;
    g_start_time  = time(NULL);
    memset(g_bytes_done,  0, sizeof(g_bytes_done));
    memset(g_bytes_total, 0, sizeof(g_bytes_total));
    g_all_done = 0;

    /*
     * Criar a thread monitor antes das workers para que o dashboard apareça
     * imediatamente. Imprime N+7 linhas em branco primeiro para reservar
     * espaço no terminal que será reutilizado pelos redesenhos ANSI.
     *
     * pthread_create: cria uma nova thread de execução.
     *   - arg1: identificador da thread (preenchido pela função)
     *   - arg2: atributos (NULL = padrão)
     *   - arg3: função de entrada da thread
     *   - arg4: argumento passado à função de entrada
     */
    if (g_dashboard_enabled) {
        for (int i = 0; i < g_num_workers + 7; i++) posix_writef(STDOUT_FILENO, "\n");
        pthread_create(&monitor_thread, NULL, run_monitor_thread, NULL);
    }

    /* ── 4. Lançar threads com fatias de bytes ── */
    for (int i = 0; i < num_threads; i++) {
        /* Preencher os argumentos específicos desta thread worker */
        args[i].ficheiros       = ficheiros;         /* lista partilhada de caminhos (só leitura) */
        args[i].total_ficheiros = total_ficheiros;
        args[i].byte_inicio     = (off_t)i * bytes_por_thread;
        /* A última thread vai até ao fim real para absorver o resto da divisão inteira */
        args[i].byte_fim        = (i == num_threads - 1) ? total_bytes : (off_t)(i + 1) * bytes_por_thread;
        args[i].worker_index    = i;
        args[i].verbose         = verbose;
        args[i].global_metrics  = &global_metrics;   /* partilhado — acesso via mutex */
        args[i].mutex           = &metrics_mutex;    /* mesmo mutex para todas as threads */
        args[i].bytes_done      = &g_bytes_done[i];  /* cada thread escreve na sua posição */
        args[i].bytes_total     = &g_bytes_total[i];

        /*
         * pthread_create: lança a thread i com a função run_worker_thread.
         * Cada thread recebe um ponteiro para o seu próprio ThreadArgs.
         * As threads correm em paralelo a partir deste ponto.
         */
        if (pthread_create(&threads[i], NULL, run_worker_thread, &args[i]) != 0) {
            perror("Erro ao criar thread");
            exit(1);
        }
    }

    /* ── 5. Esperar pelas threads ── */
    /*
     * pthread_join: bloqueia o main() até que a thread indicada termine.
     * Garante que todas as métricas locais já foram fundidas nas globais
     * antes de acedermos a global_metrics para o relatório.
     * Sem pthread_join, o main() poderia ler global_metrics incompleta.
     */
    for (int i = 0; i < num_threads; i++)
        pthread_join(threads[i], NULL);

    /* Sinalizar a thread monitor que pode terminar e aguardar a sua saída */
    if (g_dashboard_enabled) {
        g_all_done = 1;                        /* escrever flag de fim */
        pthread_join(monitor_thread, NULL);    /* esperar redesenho final */
    }

    /* Libertar o mutex — já não há threads a aceder às métricas globais */
    pthread_mutex_destroy(&metrics_mutex);

    /* ── 6. Relatório e limpeza ── */
    long elapsed = (long)(time(NULL) - g_start_time);
    gerar_relatorio_threads(&global_metrics, modo, output_file);
    posix_writef(STDOUT_FILENO, "Tempo de processamento: %ldmin %02lds\n",
                 elapsed / 60, elapsed % 60);

    /* Libertar memória dinâmica alocada para caminhos e estruturas */
    for (int i = 0; i < total_ficheiros; i++) free(ficheiros[i]);
    free(ficheiros);
    free(threads);
    free(args);

    return 0;
}
