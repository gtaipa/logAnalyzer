/**
 * @file main_basic.c
 * @brief Fase 1B — Arquitectura Multi-Processo Básica (sem IPC)
 *
 * @details
 * Implementação do Requisito B do enunciado (15% da Fase 1).
 *
 * ARQUITECTURA:
 *   - O PAI descobre todos os ficheiros .log/.json no directório indicado.
 *   - Divide a lista de ficheiros equitativamente entre N processos filho
 *     usando distribuição por blocos.
 *   - Cria N processos filho com fork().
 *   - Aguarda a conclusão de todos com waitpid().
 *
 * CADA FILHO (independente, sem IPC):
 *   - Recebe o seu subconjunto de ficheiros via variáveis locais após fork().
 *   - Para cada ficheiro: abre com open(), lê com read(), parseia e conta.
 *   - No final escreve os resultados em results_<pid>.txt com write().
 *   - Não existe NENHUMA comunicação entre processos durante a execução.
 *
 * FORMATO DE SAÍDA (uma linha por ficheiro em results_<pid>.txt):
 *   PID:<pid>;FICHEIRO:<nome>;LINHAS:<n>;ERRORS:<n>;WARNINGS:<n>
 *
 * DIFERENÇA face aos outros binários:
 *   - logAnalyzer_pipes    usa pipes anónimos (Req. C)
 *   - logAnalyzer_sockets  usa Unix Domain Sockets (Req. E)
 *   - logAnalyzer_basic    não usa IPC nenhum (este ficheiro, Req. B)
 *
 * USO:
 *   ./logAnalyzer_basic <diretorio_logs> <num_processos> <modo> [--verbose]
 *
 * EXEMPLO:
 *   ./logAnalyzer_basic datasets/apache 4 security --verbose
 */

/* ── Cabeçalhos POSIX obrigatórios ── */
#include <dirent.h>   /* opendir, readdir, closedir                      */
#include <errno.h>    /* errno, para validar strtol                       */
#include <fcntl.h>    /* open(), O_RDONLY, O_WRONLY, O_CREAT              */
#include <stdio.h>    /* snprintf, perror, fflush (auxiliares de formato) */
#include <stdlib.h>   /* malloc, free, exit, strtol                       */
#include <string.h>   /* memset, strncpy, strcmp, strrchr                 */
#include <sys/stat.h> /* stat(), struct stat                               */
#include <sys/types.h>/* pid_t, off_t, ssize_t                            */
#include <sys/wait.h> /* waitpid(), WIFEXITED, WEXITSTATUS                */
#include <unistd.h>   /* fork(), getpid(), read(), write(), close()       */

/* ── Cabeçalhos do projecto ── */
#include "parser.h"   /* LogEntry, Metrics, parse_line, detect_format, update_metrics */
#include "posix_io.h" /* posix_writef() — printf seguro via write()                   */

/** @brief Tamanho do buffer de leitura em bytes (lemos blocos de 4 KB de cada vez). */
#define BUF_SIZE        4096

/** @brief Comprimento máximo de uma linha de log (linhas mais longas são truncadas). */
#define LINE_MAX_BASIC  512

/* ═══════════════════════════════════════════════════════════════════════════
 * processar_ficheiro
 *
 * Lê e parseia um único ficheiro de log usando exclusivamente chamadas POSIX.
 * As métricas extraídas são acumuladas em *m.
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Processa um ficheiro de log e acumula as métricas em @p m.
 *
 * @details
 * Fluxo interno:
 *  1. open()   — abre o ficheiro em modo só-leitura (O_RDONLY).
 *  2. read()   — lê blocos de BUF_SIZE bytes (pode devolver menos).
 *  3. Byte-a-byte acumula caracteres numa linha local até encontrar '\\n'.
 *  4. detect_format() — detecta o formato do ficheiro (Apache/JSON/Syslog/Nginx).
 *  5. parse_line()    — interpreta a linha e preenche um LogEntry.
 *  6. update_metrics() — incrementa os contadores em *m.
 *  7. close()  — liberta o file descriptor.
 *
 * @note Usamos read() em vez de fread() porque o enunciado exige chamadas POSIX.
 *
 * @param caminho         Caminho completo para o ficheiro a processar.
 * @param m               Acumulador de métricas a preencher.
 * @param verbose         1 para imprimir diagnóstico por linha, 0 para silencioso.
 * @param pid             PID do processo filho (usado nas mensagens verbose).
 * @return 0 em sucesso, -1 se open() falhar.
 */
static int processar_ficheiro(const char *caminho, Metrics *m, int verbose, pid_t pid) {

    /*
     * open() — chamada POSIX para abrir ficheiros.
     * O_RDONLY : abrir apenas para leitura.
     * Retorna um file descriptor (inteiro ≥ 0) ou -1 em erro.
     * NÃO usar fopen() — o enunciado §8.1 proíbe as funções da stdlib C.
     */
    int fd = open(caminho, O_RDONLY);
    if (fd < 0) {
        perror("open");   /* perror() imprime a mensagem de erro do SO */
        return -1;
    }

    if (verbose)
        posix_writef(STDOUT_FILENO, "[PID %d] A processar: %s\n", (int)pid, caminho);

    /* Buffer onde read() deposita os dados lidos do disco */
    char buf[BUF_SIZE];

    /* Linha corrente em construção (acumulada byte a byte) */
    char linha[LINE_MAX_BASIC];
    int  len = 0;   /* número de bytes acumulados na linha actual */

    /*
     * Formato do ficheiro: detectado na primeira linha válida.
     * FORMAT_UNKNOWN obriga detect_format() a ser chamado uma vez.
     */
    LogFormat fmt = FORMAT_UNKNOWN;

    ssize_t n;  /* número de bytes devolvidos por read() */

    /*
     * Ciclo de leitura com read().
     * read() pode devolver MENOS bytes que BUF_SIZE (leitura parcial),
     * por isso processamos cada byte individualmente dentro do bloco.
     * Quando read() devolve 0 chegámos ao fim do ficheiro (EOF).
     */
    while ((n = read(fd, buf, BUF_SIZE)) > 0) {
        for (ssize_t b = 0; b < n; b++) {
            char c = buf[b];

            if (c == '\n') {
                /* Fim de linha — processar o que acumulámos */
                if (len > 0) {
                    linha[len] = '\0';   /* terminar a string C */

                    /* Detectar formato apenas uma vez por ficheiro */
                    if (fmt == FORMAT_UNKNOWN)
                        fmt = detect_format(linha);

                    /* Parsear a linha e obter um LogEntry normalizado */
                    LogEntry entry;
                    if (parse_line(linha, fmt, &entry) == 0)
                        update_metrics(m, &entry);  /* incrementar contadores */

                    len = 0;   /* reiniciar para a próxima linha */
                }
            } else if (c != '\r') {
                /* Acumular o byte (ignorar \r de ficheiros Windows) */
                if (len < LINE_MAX_BASIC - 1)
                    linha[len++] = c;
                /* Se a linha for mais longa que LINE_MAX_BASIC, os bytes extra
                 * são descartados silenciosamente (truncagem segura). */
            }
        }
    }

    /* Verificar erro real de read() (n < 0) */
    if (n < 0)
        perror("read");

    /* Processar a última linha caso o ficheiro não termine em '\n' */
    if (len > 0) {
        linha[len] = '\0';
        if (fmt == FORMAT_UNKNOWN)
            fmt = detect_format(linha);
        LogEntry entry;
        if (parse_line(linha, fmt, &entry) == 0)
            update_metrics(m, &entry);
    }

    /*
     * close() — liberta o file descriptor no núcleo do SO.
     * Não fechar um fd é uma fuga de recursos (fd leak).
     */
    if (close(fd) < 0)
        perror("close");

    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * escrever_resultados
 *
 * Cria results_<pid>.txt e regista uma linha por ficheiro processado.
 * Usa open()/write() — sem fprintf(), sem fwrite().
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Escreve os resultados de cada ficheiro em results_\<pid\>.txt.
 *
 * @details
 * Formato de cada linha (conforme enunciado §3.2.2):
 * @code
 *   PID:1234;FICHEIRO:access.log;LINHAS:50000;ERRORS:234;WARNINGS:1205
 * @endcode
 *
 * Chamadas POSIX usadas:
 *  - open()  com O_WRONLY|O_CREAT|O_TRUNC para criar/substituir o ficheiro.
 *  - write() para escrever cada linha.
 *  - close() para fechar o descritor.
 *
 * @param pid                   PID do processo filho (determina o nome do ficheiro).
 * @param ficheiros             Array de caminhos dos ficheiros processados.
 * @param total_ficheiros       Número de ficheiros no array.
 * @param metricas_por_ficheiro Array paralelo com as métricas de cada ficheiro.
 */
static void escrever_resultados(pid_t pid,
                                char **ficheiros, int total_ficheiros,
                                const Metrics *metricas_por_ficheiro) {
    /* Construir o nome "results_<pid>.txt" */
    char caminho_resultado[64];
    snprintf(caminho_resultado, sizeof(caminho_resultado), "results_%d.txt", (int)pid);

    /*
     * open() com flags de criação:
     *   O_WRONLY  — abrir apenas para escrita
     *   O_CREAT   — criar o ficheiro se não existir
     *   O_TRUNC   — apagar conteúdo anterior se já existir
     * 0644 — permissões: dono lê+escreve, grupo e outros só lêem
     */
    int fd = open(caminho_resultado, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        perror("open results");
        return;
    }

    /* Escrever uma linha por ficheiro processado */
    for (int i = 0; i < total_ficheiros; i++) {
        /* Extrair apenas o nome base (sem a parte do directório) */
        const char *nome = strrchr(ficheiros[i], '/');
        nome = (nome != NULL) ? nome + 1 : ficheiros[i];

        /*
         * Formato obrigatório conforme enunciado §3.2.2:
         *   PID:<pid>;FICHEIRO:<nome>;LINHAS:<n>;ERRORS:<n>;WARNINGS:<n>
         */
        char linha[512];
        int len = snprintf(linha, sizeof(linha),
                           "PID:%d;FICHEIRO:%s;LINHAS:%ld;ERRORS:%ld;WARNINGS:%ld\n",
                           (int)pid,
                           nome,
                           metricas_por_ficheiro[i].total_lines,
                           metricas_por_ficheiro[i].count_error,
                           metricas_por_ficheiro[i].count_warn);

        /*
         * write() — escreve exactamente len bytes no fd.
         * Verificar o retorno é obrigatório (o disco pode estar cheio).
         */
        if (write(fd, linha, (size_t)len) < 0)
            perror("write results");
    }

    if (close(fd) < 0)
        perror("close results");

    posix_writef(STDOUT_FILENO,
                 "[PID %d] Resultados escritos em %s\n", (int)pid, caminho_resultado);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * run_worker_basic
 *
 * Código do processo filho após fork().
 * Opera de forma completamente independente — sem IPC, sem shared memory.
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Ponto de entrada do processo filho.
 *
 * @details
 * Cada filho recebe a sua lista de ficheiros, processa-os um a um
 * e escreve os resultados finais em results_\<pid\>.txt.
 * Esta função chama exit() no final — nunca retorna.
 *
 * @note Não existe nenhum mecanismo de IPC neste fluxo.
 *       Os processos filho nunca comunicam com o pai durante a execução.
 *
 * @param ficheiros       Array de caminhos dos ficheiros a processar.
 * @param total_ficheiros Número de ficheiros no array.
 * @param verbose         1 para modo verboso, 0 para silencioso.
 */
static void run_worker_basic(char **ficheiros, int total_ficheiros, int verbose) {
    pid_t pid = getpid();   /* getpid() devolve o PID deste processo */

    /*
     * Alocar um array de Metrics — uma entrada por ficheiro.
     * calloc() inicializa tudo a zero (equivalente a malloc + memset(0)).
     */
    Metrics *metricas = calloc((size_t)total_ficheiros, sizeof(Metrics));
    if (!metricas) {
        perror("calloc");
        exit(EXIT_FAILURE);
    }

    /* Processar cada ficheiro de forma independente */
    for (int i = 0; i < total_ficheiros; i++) {
        init_metrics(&metricas[i]);   /* garantir zeros antes de começar */
        processar_ficheiro(ficheiros[i], &metricas[i], verbose, pid);
    }

    /* Escrever todos os resultados num único ficheiro results_<pid>.txt */
    escrever_resultados(pid, ficheiros, total_ficheiros, metricas);

    free(metricas);

    /*
     * exit() — termina este processo filho.
     * O pai vai recolher o status com waitpid() para evitar zombies.
     */
    exit(EXIT_SUCCESS);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * libertar_ficheiros  —  utilitário de limpeza
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Liberta a memória alocada para o array de caminhos de ficheiros.
 *
 * @param ficheiros Array de strings alocadas com strdup().
 * @param total     Número de entradas no array.
 */
static void libertar_ficheiros(char **ficheiros, int total) {
    if (!ficheiros) return;
    for (int i = 0; i < total; i++) free(ficheiros[i]);
    free(ficheiros);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * MAIN
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Ponto de entrada — Fase 1B: multi-processo sem IPC.
 *
 * @param argc Número de argumentos.
 * @param argv argv[1] = directório, argv[2] = num_processos, argv[3] = modo.
 * @return EXIT_SUCCESS (0) ou EXIT_FAILURE (1).
 */
int main(int argc, char *argv[]) {

    /* Validar número mínimo de argumentos */
    if (argc < 4) {
        posix_writef(STDOUT_FILENO,
                     "Uso: %s <diretorio_logs> <num_processos> <modo> [--verbose]\n",
                     argv[0]);
        exit(EXIT_FAILURE);
    }

    char *diretorio   = argv[1];

    /*
     * Validar num_processos com strtol() em vez de atoi().
     * atoi() não reporta erros; strtol() usa errno e o ponteiro fim
     * para distinguir "4" de "abc" ou de "0".
     */
    errno = 0;
    char *fim = NULL;
    long  val = strtol(argv[2], &fim, 10);
    if (errno != 0 || fim == argv[2] || *fim != '\0' || val <= 0) {
        posix_writef(STDERR_FILENO, "Numero de processos invalido: %s\n", argv[2]);
        exit(EXIT_FAILURE);
    }
    int num_processos = (int)val;

    char *modo  = argv[3];
    int verbose = 0;

    /* Percorrer todos os argumentos opcionais */
    for (int i = 4; i < argc; i++) {
        if (strcmp(argv[i], "--verbose") == 0) verbose = 1;
    }

    /*
     * Configurar o modo de análise no parser global.
     * Só eventos relevantes para o modo passam em parse_line().
     */
    if (parser_set_mode_from_string(modo) != 0) {
        posix_writef(STDERR_FILENO,
                     "Modo invalido: %s (use security|performance|traffic|full)\n", modo);
        exit(EXIT_FAILURE);
    }

    /* ── 1. Descobrir ficheiros .log e .json no directório ── */

    int   capacidade      = 16;   /* começa com 16; duplica quando necessário */
    int   total_ficheiros = 0;
    char **ficheiros = malloc((size_t)capacidade * sizeof(char *));
    if (!ficheiros) { perror("malloc"); exit(EXIT_FAILURE); }

    /*
     * opendir() / readdir() / closedir() — API POSIX para iterar directórios.
     * Cada struct dirent representa uma entrada (ficheiro, sub-directório, etc.).
     */
    DIR *dir = opendir(diretorio);
    if (!dir) { perror("opendir"); exit(EXIT_FAILURE); }

    struct dirent *entrada;
    while ((entrada = readdir(dir)) != NULL) {
        int len   = (int)strlen(entrada->d_name);
        int e_log  = (len > 4 && strcmp(entrada->d_name + len - 4, ".log")  == 0);
        int e_json = (len > 5 && strcmp(entrada->d_name + len - 5, ".json") == 0);

        if (!e_log && !e_json) continue;   /* ignorar ficheiros de outro tipo */

        /* Crescer o array com realloc() se atingirmos a capacidade actual */
        if (total_ficheiros == capacidade) {
            capacidade *= 2;
            ficheiros = realloc(ficheiros, (size_t)capacidade * sizeof(char *));
            if (!ficheiros) { perror("realloc"); exit(EXIT_FAILURE); }
        }

        /* Guardar o caminho completo "directório/ficheiro.log" */
        char caminho[512];
        snprintf(caminho, sizeof(caminho), "%s/%s", diretorio, entrada->d_name);
        ficheiros[total_ficheiros++] = strdup(caminho);   /* cópia própria */
    }
    closedir(dir);

    if (total_ficheiros == 0) {
        posix_writef(STDOUT_FILENO,
                     "Nenhum ficheiro .log ou .json encontrado em: %s\n", diretorio);
        free(ficheiros);
        exit(EXIT_SUCCESS);
    }

    /* Não criar mais processos do que ficheiros existentes */
    if (num_processos > total_ficheiros)
        num_processos = total_ficheiros;

    posix_writef(STDOUT_FILENO,
                 "Ficheiros: %d | Workers: %d | Modo: %s\n",
                 total_ficheiros, num_processos, modo);
    posix_writef(STDOUT_FILENO,
                 "[Req. B] Sem IPC — cada filho escreve results_<pid>.txt de forma independente.\n\n");

    /* ── 2. Lançar N processos filho com fork() ── */

    pid_t *pids = malloc((size_t)num_processos * sizeof(pid_t));
    if (!pids) { perror("malloc pids"); exit(EXIT_FAILURE); }

    /*
     * fflush(NULL) garante que os buffers do stdio estão vazios ANTES do fork().
     * Sem isto, mensagens impressas antes do fork() poderiam ser duplicadas
     * (o filho herda o conteúdo dos buffers não descarregados do pai).
     */
    fflush(NULL);

    for (int i = 0; i < num_processos; i++) {

        /*
         * Distribuição por blocos:
         * Filho i processa os ficheiros do índice inicio ao fim (bloco contíguo).
         * Alternativa: round-robin (ficheiros i, i+N, i+2N) — igualmente válida.
         */
        int por_worker = total_ficheiros / num_processos;
        int extra      = total_ficheiros % num_processos;   /* ficheiros a mais */
        int inicio     = i * por_worker + (i < extra ? i       : extra);
        int fim_bloco  = inicio + por_worker   + (i < extra ? 1 : 0);
        int count      = fim_bloco - inicio;

        /*
         * fork() — chamada fundamental de criação de processos em UNIX.
         * Duplica o processo actual (espaço de endereçamento, descritores, etc.).
         * Retorna:
         *   0   → estamos no processo FILHO
         *   >0  → estamos no processo PAI; o valor é o PID do filho criado
         *   -1  → erro (não foi criado nenhum processo filho)
         */
        pid_t pid = fork();
        if (pid < 0) { perror("fork"); exit(EXIT_FAILURE); }

        if (pid == 0) {
            /* ═══ CÓDIGO DO FILHO ═══
             *
             * O filho tem acesso aos ponteiros do array `ficheiros` porque
             * fork() faz uma cópia (copy-on-write) do espaço de endereçamento.
             * Passamos apenas os ponteiros do nosso bloco — não os copiamos.
             */
            run_worker_basic(&ficheiros[inicio], count, verbose);
            /* run_worker_basic() chama exit() — o filho nunca regressa aqui */
        }

        /* ═══ CÓDIGO DO PAI ═══ (pid > 0) */
        pids[i] = pid;   /* guardar PID para mais tarde usar em waitpid() */

        posix_writef(STDOUT_FILENO,
                     "Worker %d lançado — PID %d — %d ficheiro(s)\n",
                     i, (int)pid, count);
    }

    /* ── 3. Aguardar todos os filhos com waitpid() ── */

    posix_writef(STDOUT_FILENO, "\nA aguardar filhos...\n");

    for (int i = 0; i < num_processos; i++) {
        int status;

        /*
         * waitpid(pid, &status, 0) — bloqueia o pai até o filho terminar.
         * Sem esta chamada os filhos ficam como processos "zombie"
         * (terminados mas com entrada na tabela de processos do kernel).
         *
         * WIFEXITED(status)   — o filho terminou normalmente com exit()?
         * WEXITSTATUS(status) — qual foi o código de saída?
         */
        if (waitpid(pids[i], &status, 0) < 0) {
            perror("waitpid");
        } else if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
            posix_writef(STDERR_FILENO,
                         "Worker PID %d terminou com erro %d\n",
                         (int)pids[i], WEXITSTATUS(status));
        }
    }

    posix_writef(STDOUT_FILENO,
                 "\nTodos os workers terminaram.\n"
                 "Resultados em: results_<pid>.txt (um por worker)\n");

    /* ── Limpeza de memória ── */
    free(pids);
    libertar_ficheiros(ficheiros, total_ficheiros);

    return EXIT_SUCCESS;
}
