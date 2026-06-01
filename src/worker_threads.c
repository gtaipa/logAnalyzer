/**
 * @file worker_threads.c
 * @brief Implementação da thread worker do analisador de logs multithread.
 *
 * @details
 * Cada thread worker recebe uma fatia de bytes dos ficheiros de log — definida
 * pelos campos `byte_inicio` e `byte_fim` de `ThreadArgs` — e processa apenas
 * as linhas que caem nessa fatia. Este padrão de divisão de trabalho é chamado
 * de **data parallelism**: o mesmo código corre em paralelo sobre partes
 * diferentes dos dados, sem necessidade de coordenação durante o processamento.
 *
 * Fluxo de execução de cada thread worker:
 *  1. Inicializar métricas locais (privadas à thread, sem partilha).
 *  2. Iterar os ficheiros de log e calcular qual a porção de cada ficheiro que
 *     pertence à fatia desta thread.
 *  3. Posicionar o cursor de leitura (`lseek`) no início da fatia e avançar
 *     até ao próximo `\n` para não começar a meio de uma linha alheia.
 *  4. Ler e analisar linha a linha, acumulando contagens nas métricas locais.
 *  5. No final, fundir as métricas locais nas globais protegendo o acesso com
 *     `pthread_mutex_lock` / `pthread_mutex_unlock` para evitar race conditions.
 *
 * Porque se usam métricas **locais** durante o processamento:
 *  - Evita contenção no mutex: cada thread escreve nos seus próprios contadores
 *    durante toda a fase de leitura; o mutex só é adquirido uma vez, no final.
 *  - Melhor desempenho de cache: os contadores locais estão na stack da thread
 *    (ou na sua cache L1), sem falhas de cache provocadas por outras threads.
 *
 * Race condition que o mutex previne:
 *  - Sem mutex, duas threads poderiam executar simultaneamente, por exemplo,
 *    `global->count_error += local->count_error`. Esta operação não é atómica
 *    em C: envolve uma leitura, uma adição e uma escrita. Se ambas lerem o mesmo
 *    valor antes de qualquer escrita, uma das adições é perdida (lost update).
 *  - O mutex garante exclusão mútua: apenas uma thread executa a secção crítica
 *    de fusão de cada vez.
 */

#include "worker_threads.h"
#include "parser.h"
#include "posix_io.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>

/** Tamanho do buffer de leitura em bytes (4 KiB — alinhado com páginas de memória). */
#define BUF_SIZE 4096

/** Comprimento máximo de uma linha de log aceite pelo parser. */
#define LINE_MAX 512

/**
 * @brief Função de entrada de cada thread worker — processa uma fatia de bytes
 *        dos ficheiros de log e funde as métricas locais nas globais.
 *
 * @param arg Ponteiro para a estrutura `ThreadArgs` preenchida pelo `main()`.
 *            Contém a fatia de bytes a processar, os descritores dos ficheiros,
 *            o ponteiro para as métricas globais, o mutex de proteção e os
 *            ponteiros de progresso (`bytes_done` / `bytes_total`).
 * @return NULL — a thread termina com `pthread_exit(NULL)`. O valor de retorno
 *         não é utilizado pelo `main()` (segundo argumento de `pthread_join` é
 *         NULL), mas a assinatura `void *` é obrigatória pela API POSIX Threads.
 *
 * @details
 * Padrão de thread worker (data parallelism por fatia de bytes):
 * @code
 *   inicializar métricas locais
 *   para cada ficheiro:
 *       calcular [local_start, local_end) dentro deste ficheiro
 *       lseek para local_start
 *       se local_start > 0: avançar até ao próximo '\n'   ← alinhamento de linha
 *       ler blocos de BUF_SIZE bytes e analisar linha a linha
 *       acumular resultados em local_metrics               ← sem contenção
 *   pthread_mutex_lock(mutex)                              ← secção crítica
 *       fundir local_metrics em global_metrics
 *   pthread_mutex_unlock(mutex)                            ← fim da secção crítica
 *   pthread_exit(NULL)
 * @endcode
 *
 * A thread não necessita de mutex durante a fase de leitura porque `local_metrics`
 * é uma variável local na stack — invisível a todas as outras threads.
 */
void *run_worker_thread(void *arg) {
    /* Fazer cast do argumento genérico (void*) para o tipo concreto ThreadArgs* */
    ThreadArgs *t = (ThreadArgs *)arg;

    /* Extrair os limites da fatia atribuída a esta thread */
    off_t byte_inicio = t->byte_inicio;  /* primeiro byte global a processar (inclusivo) */
    off_t byte_fim    = t->byte_fim;     /* último byte global a processar (exclusivo) */
    off_t quota       = byte_fim - byte_inicio;  /* total de bytes desta fatia */

    /* Comunicar à thread monitor a quota e o progresso inicial (0 bytes feitos) */
    *(t->bytes_total) = (long)quota;
    *(t->bytes_done)  = 0;

    /*
     * Métricas locais: privadas a esta thread durante todo o processamento.
     * Evitam contenção no mutex porque não são partilhadas; apenas no final,
     * na fase de fusão, é que os seus valores são somados às métricas globais.
     */
    Metrics local_metrics;
    init_metrics(&local_metrics);

    /*
     * global_offset: acumulador do deslocamento em bytes considerando todos os
     * ficheiros já visitados. Serve para mapear offsets locais de cada ficheiro
     * para o espaço de endereçamento global (contínuo) de bytes.
     */
    off_t global_offset = 0;
    char  buf[BUF_SIZE];   /* buffer de leitura reutilizável para cada bloco */
    char  linha[LINE_MAX]; /* linha atual em construção, caracter a caracter */

    /* ── Iterar todos os ficheiros para encontrar os que pertencem à nossa fatia ── */
    for (int i = 0; i < t->total_ficheiros; i++) {
        struct stat st;
        if (stat(t->ficheiros[i], &st) != 0) continue; /* ficheiro inacessível */
        off_t fsize = st.st_size;

        /* Ficheiro completamente antes da nossa fatia → ignorar e avançar o offset */
        if (global_offset + fsize <= byte_inicio) {
            global_offset += fsize;
            continue;
        }
        /* Ficheiro completamente depois da nossa fatia → parar a iteração */
        if (global_offset >= byte_fim) break;

        /*
         * Calcular os limites dentro deste ficheiro em coordenadas locais:
         *   local_start: byte do ficheiro a partir do qual esta thread deve ler.
         *   local_end:   byte do ficheiro até ao qual esta thread deve ler.
         * Se a fatia global começa antes deste ficheiro, local_start = 0.
         * Se a fatia global termina depois deste ficheiro, local_end = fsize.
         */
        off_t local_start = (byte_inicio > global_offset) ? byte_inicio - global_offset : 0;
        off_t local_end   = (byte_fim < global_offset + fsize) ? byte_fim - global_offset : fsize;

        /* Abrir o ficheiro em modo só-leitura (sem criação, sem escrita) */
        int fd = open(t->ficheiros[i], O_RDONLY);
        if (fd < 0) { global_offset += fsize; continue; } /* falha de abertura → próximo */

        /* Modo verbose: registar no stderr qual o intervalo processado */
        if (t->verbose)
            posix_writef(STDERR_FILENO, "[Thread %d] %s bytes [%lld-%lld]\n",
                         t->worker_index, t->ficheiros[i],
                         (long long)local_start, (long long)local_end);

        /*
         * lseek: reposicionar o cursor de leitura do descritor `fd` para
         * `local_start` bytes a partir do início do ficheiro (SEEK_SET).
         * Sem esta chamada, a leitura começaria sempre no byte 0 do ficheiro,
         * obrigando a descartar dados que pertencem a outras threads.
         */
        if (lseek(fd, local_start, SEEK_SET) < 0) {
            perror("lseek");
            close(fd);
            global_offset += fsize;
            continue;
        }

        off_t file_pos = local_start; /* posição atual dentro do ficheiro */

        /*
         * Alinhamento de linha: se `local_start > 0`, o cursor pode estar
         * a meio de uma linha que pertence à thread anterior (que processa
         * o byte imediatamente antes de `local_start`). Para evitar parsear
         * uma linha incompleta, avançamos caracter a caracter até encontrar
         * um '\n' — o início do nosso território é sempre o começo de uma
         * linha nova.
         */
        if (local_start > 0) {
            char c;
            ssize_t r;
            /* Ler byte a byte até ao newline que delimita o fim da linha anterior */
            while ((r = read(fd, &c, 1)) == 1) {
                file_pos++;
                if (c == '\n') break; /* encontrámos o início da próxima linha */
            }
            if (r <= 0) {
                /* EOF ou erro antes de encontrar '\n' — não há linhas a processar */
                close(fd);
                global_offset += fsize;
                continue;
            }
        }

        int len  = 0;             /* número de caracteres acumulados em `linha` */
        int done = 0;             /* flag: 1 quando ultrapassámos local_end */
        LogFormat fmt = FORMAT_UNKNOWN; /* formato do log (detetado na 1.ª linha) */

        /* ── Loop principal de leitura em blocos ── */
        while (!done) {
            /*
             * read: lê até BUF_SIZE bytes do ficheiro para `buf`.
             * Retorna o número real de bytes lidos (pode ser < BUF_SIZE no
             * final do ficheiro) ou 0 (EOF) ou -1 (erro).
             */
            ssize_t n = read(fd, buf, BUF_SIZE);
            if (n <= 0) break; /* EOF ou erro → parar processamento deste ficheiro */

            /*
             * Atualizar o progresso desta thread em bytes processados.
             * `pos_na_quota` é o deslocamento dentro da nossa fatia global
             * (começa em 0, termina em `quota`). A thread monitor lê este
             * valor para calcular a percentagem do dashboard.
             * Não há race condition aqui: apenas esta thread escreve nesta
             * posição do array g_bytes_done[worker_index].
             */
            off_t pos_na_quota = (global_offset + file_pos) - byte_inicio;
            if (pos_na_quota < 0)     pos_na_quota = 0;
            if (pos_na_quota > quota) pos_na_quota = quota;
            *(t->bytes_done) = (long)pos_na_quota;

            /* Processar o bloco lido caracter a caracter para montar linhas */
            for (int b = 0; b < (int)n && !done; b++) {
                char c = buf[b];
                file_pos++; /* avançar a posição local no ficheiro */

                if (c == '\n') {
                    /* Fim de linha: analisar a linha acumulada em `linha` */
                    if (len > 0) {
                        linha[len] = '\0'; /* terminar a string C */

                        /*
                         * Detetar o formato do log (Apache, Nginx, syslog, JSON…)
                         * uma única vez por ficheiro: a partir da 1.ª linha
                         * válida, o formato mantém-se constante.
                         */
                        if (fmt == FORMAT_UNKNOWN) fmt = detect_format(linha);

                        /* Parsear a linha e atualizar as métricas locais */
                        LogEntry entry;
                        if (parse_line(linha, fmt, &entry) == 0)
                            update_metrics(&local_metrics, &entry);
                        len = 0; /* reiniciar o buffer de linha */
                    }
                    /*
                     * Verificar se já ultrapassámos o limite da nossa fatia.
                     * A condição é testada depois do '\n' para garantir que
                     * a linha completa é processada antes de parar.
                     */
                    if (file_pos >= local_end) done = 1;
                } else if (c != '\r') {
                    /* Acumular o caracter na linha atual (ignorar '\r' de CRLF) */
                    if (len < LINE_MAX - 1) linha[len++] = c;
                }
            }
        }

        /*
         * Última linha sem '\n': pode acontecer no final de um ficheiro que
         * não termina com newline, ou quando `done` foi ativado no meio de
         * uma linha. Processar o que ficou no buffer `linha`.
         */
        if (len > 0) {
            linha[len] = '\0';
            if (fmt == FORMAT_UNKNOWN) fmt = detect_format(linha);
            LogEntry entry;
            if (parse_line(linha, fmt, &entry) == 0)
                update_metrics(&local_metrics, &entry);
        }

        close(fd);             /* libertar o descritor de ficheiro */
        global_offset += fsize; /* avançar o offset global para o próximo ficheiro */
    }

    /* Forçar progresso a 100% para que o dashboard mostre a thread como concluída */
    *(t->bytes_done) = (long)quota;

    /*
     * ── Secção crítica: fusão das métricas locais nas globais ──
     *
     * pthread_mutex_lock: adquire o mutex exclusivo. Se outra thread worker
     * já estiver a fundir as suas métricas, esta thread bloqueia aqui até
     * que o mutex seja libertado. Isto garante que apenas uma thread de cada
     * vez escreve em `global_metrics`, eliminando a race condition.
     *
     * Sem este mutex, dois incrementos simultâneos (e.g., count_error += N)
     * poderiam sobrepor-se: ambas as threads lêem o mesmo valor antigo,
     * somam as suas contribuições separadamente e escrevem resultados
     * inconsistentes — um dos incrementos seria silenciosamente perdido.
     */
    pthread_mutex_lock(t->mutex);

    /* Somar os contadores simples: operação segura dentro da secção crítica */
    t->global_metrics->total_lines    += local_metrics.total_lines;
    t->global_metrics->count_debug    += local_metrics.count_debug;
    t->global_metrics->count_info     += local_metrics.count_info;
    t->global_metrics->count_warn     += local_metrics.count_warn;
    t->global_metrics->count_error    += local_metrics.count_error;
    t->global_metrics->count_critical += local_metrics.count_critical;
    t->global_metrics->count_4xx      += local_metrics.count_4xx;
    t->global_metrics->count_5xx      += local_metrics.count_5xx;

    /*
     * Fusão da tabela de IPs: para cada IP encontrado localmente, verificar
     * se já existe na tabela global (merge) ou inserir como nova entrada.
     * Esta lógica de pesquisa linear é executada dentro do mutex para que
     * não haja dois workers a modificar ip_list[] / ip_count[] em simultâneo.
     */
    for (int i = 0; i < local_metrics.ip_num; i++) {
        int found = -1;
        /* Procurar se este IP já está registado nas métricas globais */
        for (int j = 0; j < t->global_metrics->ip_num; j++) {
            if (strcmp(t->global_metrics->ip_list[j], local_metrics.ip_list[i]) == 0) {
                found = j; /* IP já existe — guardar o índice para atualizar a contagem */
                break;
            }
        }
        if (found >= 0) {
            /* IP duplicado: somar a contagem local à entrada global já existente */
            t->global_metrics->ip_count[found] += local_metrics.ip_count[i];
        } else if (t->global_metrics->ip_num < MAX_IPS) {
            /* IP novo: inserir na próxima posição livre da tabela global */
            strncpy(t->global_metrics->ip_list[t->global_metrics->ip_num],
                    local_metrics.ip_list[i], IP_LEN - 1);
            t->global_metrics->ip_list[t->global_metrics->ip_num][IP_LEN - 1] = '\0';
            t->global_metrics->ip_count[t->global_metrics->ip_num] = local_metrics.ip_count[i];
            t->global_metrics->ip_num++;
        }
        /* Se MAX_IPS foi atingido, o IP extra é descartado silenciosamente */
    }

    /*
     * Fusão dos alertas: copiar os alertas locais para a lista global,
     * respeitando o limite MAX_ALERTS. Também dentro do mutex para evitar
     * escritas concorrentes no array global de alertas.
     */
    for (int i = 0; i < local_metrics.num_alerts && t->global_metrics->num_alerts < MAX_ALERTS; i++) {
        strncpy(t->global_metrics->alerts[t->global_metrics->num_alerts],
                local_metrics.alerts[i], ALERT_LEN - 1);
        t->global_metrics->alerts[t->global_metrics->num_alerts][ALERT_LEN - 1] = '\0';
        t->global_metrics->num_alerts++;
    }

    /*
     * pthread_mutex_unlock: libertar o mutex para que a próxima thread worker
     * em espera possa entrar na secção crítica e fundir as suas métricas.
     * É fundamental não esquecer esta chamada — um mutex nunca libertado
     * bloquearia o programa indefinidamente (deadlock).
     */
    pthread_mutex_unlock(t->mutex);

    /*
     * pthread_exit: terminar esta thread de forma controlada.
     * O valor NULL é o "valor de retorno" da thread; como o main() chama
     * pthread_join(threads[i], NULL), esse valor é ignorado.
     * Diferença em relação a `return NULL`: pthread_exit executa os
     * destruidores de thread-local storage antes de terminar, garantindo
     * limpeza correta de recursos associados à thread.
     * Após esta chamada, o pthread_join correspondente no main() desbloqueia.
     */
    pthread_exit(NULL);
}