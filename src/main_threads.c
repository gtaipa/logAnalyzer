/**
 * @file main_threads.c
 * @brief Ponto de entrada do analisador de logs multithread (modelo data parallelism).
 *
 * @details
 * Orquestra o pipeline de análise paralela de logs com N threads worker POSIX:
 *
 *  1. Validar argumentos e configurar o modo do parser.
 *  2. Varrer o diretório e recolher os caminhos dos ficheiros .log/.json.
 *  3. Calcular o total de bytes e dividir o espaço de endereçamento virtual em N fatias.
 *  4. Inicializar a estrutura de métricas global e o mutex de fusão.
 *  5. Criar a thread monitor (se stdout for um TTY) e as N threads worker.
 *  6. Aguardar o fim das workers com pthread_join(3) (barreira de sincronização).
 *  7. Sinalizar a thread monitor, aguardá-la e destruir o mutex.
 *  8. Imprimir o relatório final e libertar a memória dinâmica.
 *
 * As threads worker utilizam o padrão **data parallelism**: cada thread processa uma fatia
 * [byte_inicio, byte_fim) do espaço virtual formado pela concatenação lógica dos ficheiros.
 * A fusão final das métricas é serializada por um mutex POSIX.
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

/** @brief Número máximo de threads worker suportadas em simultâneo. */
#define MAX_THREADS 64

/*
 * Arrays de monitorização de progresso — um slot por worker, indexado por worker_index.
 * Escritos pelas threads worker e lidos pela thread monitor para calcular percentagens.
 * Não requerem mutex: cada thread worker escreve exclusivamente no seu próprio slot,
 * eliminando data races de escrita simultânea.
 */
static long g_bytes_done[MAX_THREADS];  /**< @brief Bytes já processados por cada worker. */
static long g_bytes_total[MAX_THREADS]; /**< @brief Total de bytes atribuído a cada worker. */

/** @brief Número efectivo de threads worker lançadas nesta execução. */
static int    g_num_workers  = 0;

/** @brief Timestamp de arranque do processamento (epoch Unix), usado pelo dashboard. */
static time_t g_start_time   = 0;

/**
 * @brief Flag de paragem da thread monitor; declarada volatile para impedir que o compilador
 *        cache o valor num registo e não observe a escrita feita pelo main().
 */
static volatile int g_all_done = 0;

/**
 * @brief Flag que indica se o dashboard ANSI deve ser desenhado.
 *        Vale 1 se stdout for um TTY interactivo (isatty(3)); 0 se redireccionado para ficheiro.
 */
static int    g_dashboard_enabled = 0;

/* ─────────────────────────────────────────────────────────────────────────── */
/*  Dashboard                                                                   */
/* ─────────────────────────────────────────────────────────────────────────── */

/**
 * @brief Redesenha o dashboard de progresso no terminal usando sequências de escape ANSI.
 *
 * @details
 * Usa ESC[<n>A para subir o cursor `linhas` linhas e ESC[J para limpar a área a partir
 * daí, redesenhando o painel no mesmo lugar sem causar scroll. Por cada worker é
 * apresentada uma barra de progresso de 20 caracteres e a percentagem calculada como
 * bytes_done[i] / bytes_total[i] * 100.
 *
 * Segurança concorrente: lê g_bytes_done[] e g_bytes_total[] (cada posição é escrita por
 * um único worker) e escreve apenas em STDOUT via posix_writef(), sequencialmente dentro
 * da thread monitor — não requer mutex.
 */
static void draw_dashboard(void) {
    int linhas = g_num_workers + 7; /* linhas ocupadas pelo dashboard: N workers + moldura */

    /* Subir o cursor para o topo do dashboard e limpar a área inferior */
    posix_writef(STDOUT_FILENO, "\033[%dA", linhas); /* ESC[<n>A: cursor up */
    posix_writef(STDOUT_FILENO, "\033[J");            /* ESC[J: apagar até ao fim do ecrã */

    /* Calcular tempo decorrido desde o início do processamento */
    time_t elapsed = time(NULL) - g_start_time;
    int hh = elapsed / 3600;        /* horas inteiras */
    int mm = (elapsed % 3600) / 60; /* minutos */
    int ss = elapsed % 60;          /* segundos */

    /* Acumular bytes globais para o cálculo da barra de progresso total */
    long total_done  = 0;
    long total_total = 0;
    for (int i = 0; i < g_num_workers; i++) {
        total_done  += g_bytes_done[i];
        total_total += g_bytes_total[i];
    }
    /* Percentagem global; proteger contra divisão por zero */
    int total_pct = (total_total > 0) ? (int)(total_done * 100 / total_total) : 0;
    if (total_pct > 100) total_pct = 100; /* clamp superior */

    /* Cabeçalho do dashboard */
    posix_writef(STDOUT_FILENO, "╔══════════════════════════════════════════╗\n");
    posix_writef(STDOUT_FILENO, "║    LOG ANALYZER - THREADS MONITOR        ║\n");
    posix_writef(STDOUT_FILENO, "╠══════════════════════════════════════════╣\n");

    /* Uma linha por worker com barra de progresso individual */
    for (int i = 0; i < g_num_workers; i++) {
        /* Percentagem individual; clampada a [0, 100] */
        int pct = (g_bytes_total[i] > 0) ? (int)(g_bytes_done[i] * 100 / g_bytes_total[i]) : 0;
        if (pct > 100) pct = 100;

        /* Construir barra de 20 caracteres: '#' preenchido, '.' vazio */
        char bar[21];
        int filled = pct / 5; /* cada '#' representa 5 % */
        for (int b = 0; b < 20; b++) bar[b] = (b < filled) ? '#' : '.';
        bar[20] = '\0'; /* terminador de string C */

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
 * @brief Ponto de entrada da thread monitor do dashboard.
 *
 * @details
 * Corre em paralelo com as N threads worker. Redesenha o dashboard a cada 100 ms
 * (usleep(100000)) enquanto g_all_done for 0. Quando main() define g_all_done = 1
 * (após pthread_join de todos os workers), a thread monitor executa um último redesenho
 * para mostrar 100 % e termina com pthread_exit(3).
 *
 * @param arg Não utilizado (NULL); declarado como (void)arg para suprimir aviso do compilador.
 * @return NULL — terminação via pthread_exit(3) é explícita para invocar os destruidores TLS.
 */
void *run_monitor_thread(void *arg) {
    (void)arg; /* parâmetro não utilizado; cast para void suprime aviso de compilação */

    /* Loop principal: redesenhar o dashboard enquanto os workers estiverem a correr */
    while (!g_all_done) {
        draw_dashboard();
        usleep(100000); /* 100 ms entre redesenhos (~10 fps) */
    }

    /* Redesenho final para garantir que o dashboard apresenta 100 % */
    draw_dashboard();

    /*
     * pthread_exit(3): terminar esta thread de forma controlada, executando os destruidores
     * de thread-local storage (TLS). O pthread_join correspondente no main() desbloqueia
     * imediatamente após esta chamada.
     */
    pthread_exit(NULL);
}

/* ─────────────────────────────────────────────────────────────────────────── */
/*  Relatório final                                                             */
/* ─────────────────────────────────────────────────────────────────────────── */

/**
 * @brief Gera o relatório final de análise e escreve-o no stdout ou num ficheiro.
 *
 * @param total       Ponteiro para a estrutura Metrics global com os totais já fundidos
 *                    por todas as threads worker.
 * @param modo        String com o modo de análise: "security", "traffic", "performance" ou "full".
 * @param output_file Caminho para o ficheiro de saída. Se NULL, o relatório é escrito em stdout.
 *
 * @details
 * Constrói o relatório em memória (buffer de 4 KiB na stack) e escreve-o de uma vez com
 * write(2). Uma única chamada a write(2) é atómica para tamanhos < PIPE_BUF (4 KiB no Linux),
 * evitando intercalação com output de outras fontes (e.g., stderr). As secções de alertas e
 * tráfego são incluídas condicionalmente conforme o parâmetro modo.
 */
void gerar_relatorio_threads(Metrics *total, char *modo, char *output_file) {
    int fd_out  = STDOUT_FILENO; /* destino por omissão: stdout */
    int fd_file = -1;            /* descritor de ficheiro de saída; -1 = não aberto */

    /*
     * Se foi especificado um ficheiro de saída, tentar criá-lo/truncá-lo:
     *   O_WRONLY: abertura apenas para escrita.
     *   O_CREAT:  criar o ficheiro se não existir.
     *   O_TRUNC:  truncar o conteúdo se o ficheiro já existir.
     *   0644:     permissões Unix (dono: rw; grupo e outros: r).
     * Se open(2) falhar, fd_out mantém-se STDOUT_FILENO.
     */
    if (output_file != NULL) {
        fd_file = open(output_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd_file >= 0) {
            fd_out = fd_file; /* redirecionar saída para o ficheiro */
            posix_writef(STDOUT_FILENO, "\n[INFO] A gravar relatorio no ficheiro: %s\n", output_file);
        }
    }

    /*
     * Construir o relatório em memória com snprintf(3) antes de escrever.
     * Evita múltiplas syscalls write(2) e reduz a fragmentação da saída.
     * `buffer + len` avança o ponteiro de escrita a cada snprintf, de forma segura.
     */
    char buffer[4096];
    int len = 0;

    /* Cabeçalho do relatório */
    len += snprintf(buffer + len, sizeof(buffer) - len,
                    "\n=== RELATORIO FINAL THREADS (%s) ===\n", modo);
    len += snprintf(buffer + len, sizeof(buffer) - len,
                    "Total de linhas : %ld\n", total->total_lines);

    /*
     * Secção de segurança: contagens de severidade WARN, ERROR e CRITICAL.
     * Presente nos modos "security" e "full".
     */
    if (strcmp(modo, "security") == 0 || strcmp(modo, "full") == 0) {
        len += snprintf(buffer + len, sizeof(buffer) - len, "\n--- ALERTAS DE SEGURANCA ---\n");
        len += snprintf(buffer + len, sizeof(buffer) - len,
                        "WARNINGS        : %ld\n", total->count_warn);
        len += snprintf(buffer + len, sizeof(buffer) - len,
                        "ERRORS          : %ld\n", total->count_error);
        len += snprintf(buffer + len, sizeof(buffer) - len,
                        "CRITICAL        : %ld\n", total->count_critical);
    }

    /*
     * Secção de tráfego: contagens de logs INFO e erros HTTP 4xx/5xx.
     * Presente nos modos "traffic" e "full".
     */
    if (strcmp(modo, "traffic") == 0 || strcmp(modo, "full") == 0) {
        len += snprintf(buffer + len, sizeof(buffer) - len, "\n--- ESTATISTICAS DE TRAFEGO ---\n");
        len += snprintf(buffer + len, sizeof(buffer) - len,
                        "INFO            : %ld\n", total->count_info);
        len += snprintf(buffer + len, sizeof(buffer) - len,
                        "HTTP 4xx/5xx    : %ld\n", total->count_4xx + total->count_5xx);
    }

    len += snprintf(buffer + len, sizeof(buffer) - len, "=================================\n\n");

    /*
     * Escrever o relatório completo de uma só vez com write(2) POSIX.
     * Atómico para tamanhos < PIPE_BUF; evita intercalação com stderr de outras threads.
     */
    if (write(fd_out, buffer, len) < 0) perror("Erro ao escrever relatorio");

    /* Fechar o descritor do ficheiro de saída se foi aberto nesta função */
    if (fd_file >= 0) close(fd_file);
}

/* ─────────────────────────────────────────────────────────────────────────── */
/*  main                                                                        */
/* ─────────────────────────────────────────────────────────────────────────── */

/**
 * @brief Ponto de entrada do programa — orquestra o pipeline multithread de análise de logs.
 *
 * @param argc Número de argumentos da linha de comandos.
 * @param argv Vetor de strings:
 *             - argv[1]: diretório com os ficheiros de log
 *             - argv[2]: número de threads worker a criar
 *             - argv[3]: modo de análise (security|performance|traffic|full)
 *             - argv[4..]: flags opcionais --verbose e --output=<ficheiro>
 * @return 0 em caso de sucesso; 1 em caso de erro.
 *
 * @details
 * Fluxo de execução:
 *  1. Validar argumentos e configurar o modo do parser.
 *  2. Varrer o diretório e recolher caminhos de ficheiros .log/.json.
 *  3. Calcular total de bytes e determinar a fatia de cada thread.
 *  4. Inicializar métricas globais e o mutex de fusão.
 *  5. Criar a thread monitor (se em TTY) e as N threads worker.
 *  6. Aguardar o fim das workers com pthread_join(3).
 *  7. Sinalizar a thread monitor, aguardá-la e destruir o mutex.
 *  8. Imprimir o relatório e libertar memória dinâmica.
 */
int main(int argc, char *argv[]) {
    /* Verificar se stdout é um TTY para activar o dashboard ANSI */
    g_dashboard_enabled = isatty(STDOUT_FILENO);

    /* Validação mínima dos argumentos obrigatórios */
    if (argc < 4) {
        posix_writef(STDOUT_FILENO,
                     "Uso: %s <diretorio> <num_threads> <modo> [--verbose] [--output=ficheiro.txt]\n",
                     argv[0]);
        exit(1);
    }

    char *diretorio   = argv[1];       /* caminho do diretório com os ficheiros de log */
    int   num_threads = atoi(argv[2]); /* número de threads worker a criar */
    char *modo        = argv[3];       /* modo de análise: security|performance|traffic|full */
    int   verbose     = 0;             /* flag de modo verboso; 0 = desactivado por omissão */
    char *output_file = NULL;          /* caminho do ficheiro de saída; NULL = stdout */

    /* Processar flags opcionais: --verbose e --output=<caminho> */
    for (int i = 4; i < argc; i++) {
        if (strcmp(argv[i], "--verbose") == 0) verbose = 1;
        else if (strncmp(argv[i], "--output=", 9) == 0) output_file = argv[i] + 9;
    }

    /* Configurar o parser com o modo escolhido; abortar se a string for inválida */
    if (parser_set_mode_from_string(modo) != 0) {
        posix_writef(STDERR_FILENO,
                     "Modo invalido: %s (use security|performance|traffic|full)\n", modo);
        exit(1);
    }

    /* ── 1. Descobrir ficheiros ── */
    int capacidade = 10, total_ficheiros = 0;
    char **ficheiros = malloc(capacidade * sizeof(char *)); /* vetor dinâmico de caminhos */
    DIR *dir = opendir(diretorio);                          /* abrir diretório para travessia */
    if (!dir) { perror("opendir"); exit(1); }

    /* Iterar entradas do diretório e recolher apenas .log e .json */
    struct dirent *entrada;
    while ((entrada = readdir(dir)) != NULL) {
        int len = strlen(entrada->d_name);
        if ((len > 4 && strcmp(entrada->d_name + len - 4, ".log")  == 0) ||
            (len > 5 && strcmp(entrada->d_name + len - 5, ".json") == 0)) {
            /* Crescimento elástico do vetor: duplicar capacidade quando necessário */
            if (total_ficheiros == capacidade) {
                capacidade *= 2;
                ficheiros = realloc(ficheiros, capacidade * sizeof(char *));
            }
            char caminho[512];
            snprintf(caminho, sizeof(caminho), "%s/%s", diretorio, entrada->d_name);
            ficheiros[total_ficheiros++] = strdup(caminho); /* cópia própria na heap */
        }
    }
    closedir(dir);

    if (total_ficheiros == 0) {
        posix_writef(STDOUT_FILENO, "Nenhum ficheiro .log ou .json encontrado.\n");
        exit(0);
    }

    /* Limitar o número de threads ao máximo suportado pelo array estático */
    if (num_threads > MAX_THREADS) num_threads = MAX_THREADS;

    /* ── 2. Calcular total de bytes e dividir em fatias iguais ── */
    struct stat st;
    off_t total_bytes = 0;
    for (int i = 0; i < total_ficheiros; i++) {
        if (stat(ficheiros[i], &st) == 0)
            total_bytes += st.st_size; /* acumular tamanho físico de cada ficheiro */
    }

    /* Não faz sentido ter mais threads do que ficheiros */
    if (num_threads > total_ficheiros) num_threads = total_ficheiros;

    /*
     * Divisão do espaço de endereçamento virtual em N fatias consecutivas:
     *   Thread 0   → [0,                  bytes_por_thread)
     *   Thread 1   → [bytes_por_thread,   2*bytes_por_thread)
     *   ...
     *   Thread N-1 → [(N-1)*bytes_por_thread, total_bytes)   ← absorve o resto da divisão inteira
     */
    off_t bytes_por_thread = total_bytes / num_threads;

    /* ── 3. Inicializar estruturas ── */
    Metrics global_metrics;
    init_metrics(&global_metrics); /* zerrar todos os contadores acumulados */

    /*
     * Mutex que serializa a fusão de métricas locais nas globais.
     * Sem este trinco, incrementos concorrentes (e.g., count_error += N) seriam
     * não-atómicos em C, causando lost updates e dados corrompidos.
     */
    pthread_mutex_t metrics_mutex;
    pthread_mutex_init(&metrics_mutex, NULL); /* atributos por omissão (mutex normal) */

    pthread_t  *threads = malloc(num_threads * sizeof(pthread_t)); /* IDs das threads worker */
    ThreadArgs *args    = malloc(num_threads * sizeof(ThreadArgs)); /* argumentos por worker */
    pthread_t   monitor_thread; /* identificador da thread do dashboard */

    /* Inicializar variáveis globais de progresso antes de criar as threads */
    g_num_workers = num_threads;
    g_start_time  = time(NULL);
    memset(g_bytes_done,  0, sizeof(g_bytes_done));
    memset(g_bytes_total, 0, sizeof(g_bytes_total));
    g_all_done = 0;

    /*
     * Criar a thread monitor antes das workers para que o dashboard apareça imediatamente.
     * Imprimir N+7 linhas em branco primeiro para reservar o espaço que os redesenhos
     * ANSI irão reutilizar, evitando scroll indesejado no terminal.
     */
    if (g_dashboard_enabled) {
        for (int i = 0; i < g_num_workers + 7; i++) posix_writef(STDOUT_FILENO, "\n");
        pthread_create(&monitor_thread, NULL, run_monitor_thread, NULL);
    }

    /* ── 4. Lançar threads worker com as fatias de bytes calculadas ── */
    for (int i = 0; i < num_threads; i++) {
        args[i].ficheiros       = ficheiros;       /* lista partilhada de caminhos (só leitura) */
        args[i].total_ficheiros = total_ficheiros;
        args[i].byte_inicio     = (off_t)i * bytes_por_thread;
        /* A última thread absorve o resto da divisão inteira */
        args[i].byte_fim        = (i == num_threads - 1) ? total_bytes
                                                          : (off_t)(i + 1) * bytes_por_thread;
        args[i].worker_index    = i;
        args[i].verbose         = verbose;
        args[i].global_metrics  = &global_metrics;  /* métricas globais partilhadas (via mutex) */
        args[i].mutex           = &metrics_mutex;   /* mesmo mutex para todas as threads */
        args[i].bytes_done      = &g_bytes_done[i]; /* slot de progresso exclusivo desta thread */
        args[i].bytes_total     = &g_bytes_total[i];

        /*
         * pthread_create(3): lançar a thread i com a função run_worker_thread.
         * As threads correm em paralelo a partir deste ponto.
         */
        if (pthread_create(&threads[i], NULL, run_worker_thread, &args[i]) != 0) {
            perror("Erro ao criar thread");
            exit(1);
        }
    }

    /* ── 5. Aguardar o fim de todas as threads worker ── */
    /*
     * pthread_join(3): bloqueia o main() até que a thread indicada termine.
     * Garante que todas as métricas locais foram fundidas nas globais antes de
     * acedermos a global_metrics para o relatório. Sem este join, o main() poderia
     * ler global_metrics ainda incompleta (data race entre main e workers).
     */
    for (int i = 0; i < num_threads; i++)
        pthread_join(threads[i], NULL); /* aguardar a thread i; ignorar valor de retorno */

    /* Sinalizar a thread monitor para parar e aguardar o redesenho final */
    if (g_dashboard_enabled) {
        g_all_done = 1;                     /* activar flag de paragem do monitor */
        pthread_join(monitor_thread, NULL); /* aguardar redesenho final a 100 % */
    }

    /* Destruir o mutex — já não há threads a aceder às métricas globais */
    pthread_mutex_destroy(&metrics_mutex);

    /* ── 6. Relatório final e limpeza ── */
    long elapsed = (long)(time(NULL) - g_start_time); /* duração total do processamento */
    gerar_relatorio_threads(&global_metrics, modo, output_file);
    posix_writef(STDOUT_FILENO, "Tempo de processamento: %ldmin %02lds\n",
                 elapsed / 60, elapsed % 60);

    /* Libertar memória dinâmica alocada para caminhos e estruturas */
    for (int i = 0; i < total_ficheiros; i++) free(ficheiros[i]);
    free(ficheiros);
    free(threads);
    free(args);

    return 0; /* devolver código de sucesso ao sistema operativo */
}
