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
    /*
     * isatty: verifica se o descritor STDOUT está ligado a um terminal real
     * (TTY). Se a saída for redireccionada para um ficheiro ou pipe,
     * isatty retorna 0 e o dashboard ANSI é desativado para não poluir
     * o ficheiro com sequências de escape ininteligíveis.
     */
    g_dashboard_enabled = isatty(STDOUT_FILENO);

    /* Validação mínima de argumentos obrigatórios — programa não arranca sem eles */
    if (argc < 4) {
        posix_writef(STDOUT_FILENO, "Uso: %s <diretorio> <num_threads> <modo> [--verbose] [--output=ficheiro.txt]\n", argv[0]);
        exit(1);
    }

    char *diretorio = argv[1];
    int num_threads = atoi(argv[2]); /* converter string "N" para inteiro */
    char *modo      = argv[3];
    int verbose     = 0;
    char *output_file = NULL;

    /* Processar flags opcionais: --verbose e --output=<caminho> */
    for (int i = 4; i < argc; i++) {
        if (strcmp(argv[i], "--verbose") == 0) verbose = 1;
        /* strncmp com 9: compara apenas o prefixo "--output="; o resto é o caminho */
        else if (strncmp(argv[i], "--output=", 9) == 0) output_file = argv[i] + 9;
    }

    /* Configurar o parser com o modo escolhido; abortar com mensagem se inválido */
    if (parser_set_mode_from_string(modo) != 0) {
        posix_writef(STDERR_FILENO, "Modo invalido: %s (use security|performance|traffic|full)\n", modo);
        exit(1);
    }

    /* ── 1. Descobrir ficheiros ── */
    /*
     * Usar um array dinâmico (capacidade inicial = 10) que duplica quando
     * necessário — padrão clássico de lista dinâmica em C.
     * opendir / readdir são funções POSIX para iterar entradas de diretório.
     */
    int capacidade = 10, total_ficheiros = 0;
    char **ficheiros = malloc(capacidade * sizeof(char *));
    DIR *dir = opendir(diretorio);
    if (!dir) { perror("opendir"); exit(1); }

    /* Iterar as entradas do diretório e recolher apenas .log e .json */
    struct dirent *entrada;
    while ((entrada = readdir(dir)) != NULL) {
        int len = strlen(entrada->d_name);
        /* Filtrar por extensão: comparar os últimos 4 ou 5 caracteres do nome */
        if ((len > 4 && strcmp(entrada->d_name + len - 4, ".log")  == 0) ||
            (len > 5 && strcmp(entrada->d_name + len - 5, ".json") == 0)) {
            /* Crescer o array dinamicamente se necessário (duplicar capacidade) */
            if (total_ficheiros == capacidade) {
                capacidade *= 2;
                ficheiros = realloc(ficheiros, capacidade * sizeof(char *));
            }
            /* Construir o caminho completo: "<diretorio>/<nome>" */
            char caminho[512];
            snprintf(caminho, sizeof(caminho), "%s/%s", diretorio, entrada->d_name);
            /* strdup: aloca memória para uma cópia própria do caminho */
            ficheiros[total_ficheiros++] = strdup(caminho);
        }
    }
    closedir(dir); /* libertar o handle do diretório após a iteração */

    if (total_ficheiros == 0) {
        posix_writef(STDOUT_FILENO, "Nenhum ficheiro .log ou .json encontrado.\n");
        exit(0);
    }

    /* Garantir que não excedemos o limite de threads definido em MAX_THREADS */
    if (num_threads > MAX_THREADS) num_threads = MAX_THREADS;

    /* ── 2. Calcular total de bytes e dividir em fatias iguais ── */
    /*
     * stat: obtém metadados do ficheiro (incluindo st_size — tamanho em bytes)
     * sem abrir o ficheiro. Somar todos os tamanhos dá o espaço total a dividir.
     */
    struct stat st;
    off_t total_bytes = 0;
    for (int i = 0; i < total_ficheiros; i++) {
        if (stat(ficheiros[i], &st) == 0)
            total_bytes += st.st_size;
    }

    /* Não faz sentido ter mais threads do que ficheiros (algumas ficariam sem trabalho) */
    if (num_threads > total_ficheiros) num_threads = total_ficheiros;

    /*
     * Divisão do espaço de endereçamento de bytes em fatias consecutivas:
     *   Thread 0  → [0,                  bytes_por_thread)
     *   Thread 1  → [bytes_por_thread,   2*bytes_por_thread)
     *   ...
     *   Thread N-1→ [(N-1)*bytes_por_thread, total_bytes)  ← última absorve o resto
     *
     * Este padrão garante que todos os bytes são processados exatamente uma
     * vez, sem sobreposição entre threads. A última thread recebe o resto da
     * divisão inteira (pode ser ligeiramente maior que as outras).
     */
    off_t bytes_por_thread = total_bytes / num_threads;

    /* ── 3. Inicializar estruturas ── */
    Metrics global_metrics;
    init_metrics(&global_metrics); /* zerar todos os contadores antes de qualquer thread escrever */

    /*
     * Criar o mutex que protege `global_metrics` durante a fase de fusão.
     * pthread_mutex_init com NULL usa atributos por omissão (mutex normal,
     * não recursivo). Sem mutex, N threads a incrementar os mesmos campos
     * simultaneamente causariam race conditions: leituras e escritas
     * intercaladas perderiam atualizações silenciosamente.
     *
     * Este mutex é partilhado por todas as threads worker (passado via
     * ThreadArgs.mutex) — todas usam o mesmo objeto para exclusão mútua.
     */
    pthread_mutex_t metrics_mutex;
    pthread_mutex_init(&metrics_mutex, NULL);

    /*
     * Alocar dinamicamente os arrays de identificadores de thread e de
     * argumentos, pois `num_threads` só é conhecido em tempo de execução.
     */
    pthread_t  *threads = malloc(num_threads * sizeof(pthread_t));
    ThreadArgs *args    = malloc(num_threads * sizeof(ThreadArgs));
    pthread_t   monitor_thread; /* identificador da thread monitor do dashboard */

    /* Inicializar variáveis globais de progresso antes de criar as threads */
    g_num_workers = num_threads;
    g_start_time  = time(NULL);   /* registar instante de início para o cronómetro */
    memset(g_bytes_done,  0, sizeof(g_bytes_done));   /* progresso inicial = 0 bytes */
    memset(g_bytes_total, 0, sizeof(g_bytes_total));  /* será preenchido por cada worker */
    g_all_done = 0; /* flag de término: 0 = workers ainda a correr */

    /*
     * Criar a thread monitor ANTES das workers para que o dashboard apareça
     * imediatamente. Primeiro imprime N+7 linhas em branco para reservar a
     * área do terminal que será reutilizada pelos redesenhos ANSI subsequentes.
     *
     * pthread_create — assinatura:
     *   int pthread_create(pthread_t *tid,              ← identificador (saída)
     *                      const pthread_attr_t *attr,  ← atributos (NULL = padrão)
     *                      void *(*start)(void *),      ← função de entrada
     *                      void *arg);                  ← argumento à função
     * Retorna 0 em caso de sucesso; qualquer valor positivo indica erro.
     */
    if (g_dashboard_enabled) {
        /* Reservar espaço em branco no terminal para o dashboard */
        for (int i = 0; i < g_num_workers + 7; i++) posix_writef(STDOUT_FILENO, "\n");
        /* Lançar a thread monitor; corre imediatamente em paralelo com o main() */
        pthread_create(&monitor_thread, NULL, run_monitor_thread, NULL);
    }

    /* ── 4. Lançar threads com fatias de bytes ── */
    for (int i = 0; i < num_threads; i++) {
        /* Preencher os argumentos específicos desta thread worker */
        args[i].ficheiros       = ficheiros;         /* lista partilhada de caminhos (só leitura após init) */
        args[i].total_ficheiros = total_ficheiros;
        args[i].byte_inicio     = (off_t)i * bytes_por_thread;        /* início da fatia (inclusivo) */
        /*
         * A última thread vai até ao fim real do espaço de bytes para absorver
         * o resto da divisão inteira (total_bytes % num_threads bytes extra).
         */
        args[i].byte_fim        = (i == num_threads - 1) ? total_bytes : (off_t)(i + 1) * bytes_por_thread;
        args[i].worker_index    = i;                 /* índice único para o dashboard e para g_bytes_done[] */
        args[i].verbose         = verbose;
        args[i].global_metrics  = &global_metrics;   /* estrutura partilhada — acedida via mutex na fusão */
        args[i].mutex           = &metrics_mutex;    /* mesmo mutex para todas as threads */
        args[i].bytes_done      = &g_bytes_done[i];  /* cada thread escreve só na sua posição — sem conflito */
        args[i].bytes_total     = &g_bytes_total[i]; /* quota desta thread, lida pelo dashboard */

        /*
         * pthread_create: lançar a thread i.
         * A partir desta chamada, run_worker_thread corre concorrentemente
         * com o main() e com as threads já criadas.
         * Cada thread recebe &args[i] — ponteiros para posições distintas
         * do array, sem partilha de estrutura ThreadArgs entre threads.
         */
        if (pthread_create(&threads[i], NULL, run_worker_thread, &args[i]) != 0) {
            perror("Erro ao criar thread");
            exit(1);
        }
    }

    /* ── 5. Esperar pelas threads ── */
    /*
     * pthread_join: bloqueia a thread chamante (main) até que a thread
     * identificada por `threads[i]` termine (chamou pthread_exit ou retornou).
     *
     * Porque é necessário pthread_join aqui:
     *  - Garante que todas as N workers terminaram e que as suas fusões de
     *    métricas em global_metrics foram completamente executadas.
     *  - Sem pthread_join, o main() poderia ler global_metrics ainda
     *    incompleta (dados parciais de threads que ainda não terminaram).
     *  - Também liberta os recursos internos (stack, TLS) da thread terminada.
     *
     * O segundo argumento (NULL) indica que não queremos o valor de retorno
     * da thread (que seria o argumento de pthread_exit).
     */
    for (int i = 0; i < num_threads; i++)
        pthread_join(threads[i], NULL);

    /*
     * Após todos os pthread_join das workers, global_metrics está completa e
     * consistente — não há mais threads a escrever nela.
     * Sinalizar à thread monitor que pode fazer o redesenho final e terminar.
     */
    if (g_dashboard_enabled) {
        g_all_done = 1;                        /* setar flag: workers terminaram */
        pthread_join(monitor_thread, NULL);    /* aguardar o redesenho final do dashboard */
    }

    /*
     * pthread_mutex_destroy: libertar os recursos internos do mutex.
     * Só é seguro chamar depois de garantir que nenhuma thread o usa.
     * Aqui é seguro porque todos os pthread_join já retornaram.
     */
    pthread_mutex_destroy(&metrics_mutex);

    /* ── 6. Relatório e limpeza ── */
    long elapsed = (long)(time(NULL) - g_start_time); /* tempo total de processamento */
    gerar_relatorio_threads(&global_metrics, modo, output_file);
    posix_writef(STDOUT_FILENO, "Tempo de processamento: %ldmin %02lds\n",
                 elapsed / 60, elapsed % 60);

    /* Libertar toda a memória dinâmica alocada para caminhos e estruturas */
    for (int i = 0; i < total_ficheiros; i++) free(ficheiros[i]); /* strings duplicadas com strdup */
    free(ficheiros);  /* array de ponteiros */
    free(threads);    /* array de identificadores de thread */
    free(args);       /* array de argumentos das threads */

    return 0;
}
