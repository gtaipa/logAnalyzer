/**
 * @file worker_sockets.c
 * @brief Processo filho para análise paralela de logs com comunicação via Unix Domain Sockets.
 *
 * @details
 * Cada processo filho conecta-se ao servidor pai via Unix Domain Socket, recebe a
 * configuração da sua fatia [byte_inicio, byte_fim) e envia progresso e resultado
 * de volta pelo mesmo socket (comunicação bidirecional).
 *
 * Fluxo de execução do processo filho:
 *  1. Ligar ao servidor pai via connect_to_server() (com retry automático).
 *  2. Ler MSG_CONFIG: int(tipo) + WorkerConfig com byte_inicio, byte_fim, worker_index.
 *  3. Processar ficheiros no intervalo recebido via processar_por_bytes().
 *  4. Enviar MSG_PROGRESSO a cada 100 linhas.
 *  5. Enviar MSG_RESULTADO com métricas finais e top-10 IPs.
 *  6. Fechar o socket para sinalizar EOF ao pai.
 *
 * Protocolo de mensagens (bidirecional):
 *  - pai → filho: MSG_CONFIG  (tipo 0) + WorkerConfig
 *  - filho → pai: MSG_PROGRESSO (tipo 1) + ProgressUpdate
 *  - filho → pai: MSG_RESULTADO (tipo 2) + WorkerResult
 */

#include "worker.h"
#include "parser.h"
#include "ipc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>

/** @brief Dimensão do buffer de leitura por invocação de read(2) (4 KiB — alinhado ao tamanho de página). */
#define BUF_SIZE  4096

/** @brief Comprimento máximo de uma linha de log aceite pelo parser (caracteres incluindo '\0'). */
#define LINHA_MAX  512

/**
 * @brief Envia uma mensagem de progresso ao processo pai via socket.
 *
 * @details Idêntico em semântica ao enviar_progresso de worker_pipes.c, mas escreve
 * no socket bidirecional em vez de num pipe unidireccional. As escritas não usam
 * writen() porque a perda ocasional de uma actualização de progresso não é fatal.
 *
 * @param sock         Descritor do socket ligado ao servidor pai.
 * @param worker_index Índice deste worker, usado pelo pai para indexar o dashboard.
 * @param bytes_done   Bytes já processados nesta quota.
 * @param bytes_total  Total de bytes desta quota.
 */
static void enviar_progresso(int sock, int worker_index, long bytes_done, long bytes_total) {
    int tipo = MSG_PROGRESSO;
    write(sock, &tipo, sizeof(tipo));

    ProgressUpdate pu;
    pu.pid          = getpid();
    pu.worker_index = worker_index;
    pu.bytes_done   = bytes_done;
    pu.bytes_total  = bytes_total;
    write(sock, &pu, sizeof(pu));
}

/**
 * @brief Ordena os IPs por frequência decrescente e prepara o resultado final.
 *
 * @details Usa bubble sort sobre os arrays paralelos ips[] e counts[] para ordenar
 * por contagem decrescente. O algoritmo O(n²) é aceitável porque MAX_IPS é pequeno.
 * Os 10 IPs mais frequentes são copiados para o resultado a enviar ao pai.
 *
 * @param m Métricas acumuladas durante o processamento da fatia.
 * @param r Estrutura de resultado a preencher.
 */
static void preparar_resultado(const Metrics *m, WorkerResult *r) {
    memset(r, 0, sizeof(*r));
    r->pid            = getpid();
    r->total_lines    = m->total_lines;
    r->count_debug    = m->count_debug;
    r->count_info     = m->count_info;
    r->count_warn     = m->count_warn;
    r->count_error    = m->count_error;
    r->count_critical = m->count_critical;
    r->count_4xx      = m->count_4xx;
    r->count_5xx      = m->count_5xx;

    /* Copiar tabela de IPs para arrays locais antes de ordenar */
    char ips[MAX_IPS][IP_LEN];
    long counts[MAX_IPS];
    int  n = m->ip_num;
    if (n > MAX_IPS) n = MAX_IPS;

    for (int i = 0; i < n; i++) {
        strncpy(ips[i], m->ip_list[i], IP_LEN - 1);
        ips[i][IP_LEN - 1] = '\0';
        counts[i] = m->ip_count[i];
    }

    /* Bubble sort por contagem decrescente; os arrays ips[] e counts[] são co-ordenados */
    for (int i = 0; i < n - 1; i++) {
        for (int j = 0; j < n - i - 1; j++) {
            if (counts[j] < counts[j + 1]) {
                long tmp_count  = counts[j];
                counts[j]       = counts[j + 1];
                counts[j + 1]   = tmp_count;

                char tmp_ip[IP_LEN];
                strncpy(tmp_ip,     ips[j],     IP_LEN);
                strncpy(ips[j],     ips[j + 1], IP_LEN);
                strncpy(ips[j + 1], tmp_ip,     IP_LEN);
            }
        }
    }

    /* Copiar os 10 IPs mais frequentes para o resultado */
    int limite = n < 10 ? n : 10;
    for (int i = 0; i < limite; i++) {
        strncpy(r->top_ips[i], ips[i], IP_LEN - 1);
        r->top_ips[i][IP_LEN - 1] = '\0';
        r->top_ips_counts[i]       = counts[i];
    }

    r->num_alerts = m->num_alerts < MAX_ALERTS ? m->num_alerts : MAX_ALERTS;
    for (int i = 0; i < r->num_alerts; i++) {
        strncpy(r->alerts[i], m->alerts[i], ALERT_LEN - 1);
        r->alerts[i][ALERT_LEN - 1] = '\0';
    }
}

/**
 * @brief Envia o resultado final ao processo pai via socket.
 *
 * @param sock Descritor do socket ligado ao servidor pai.
 * @param m    Métricas acumuladas a serializar.
 */
static void enviar_resultado(int sock, Metrics *m) {
    int tipo = MSG_RESULTADO;
    write(sock, &tipo, sizeof(tipo));

    WorkerResult r;
    preparar_resultado(m, &r);
    write(sock, &r, sizeof(r));
}

/**
 * @brief Processa a fatia [byte_inicio, byte_fim) dos ficheiros e envia progresso via socket.
 *
 * @details Itera sobre os ficheiros calculando a intersecção com o intervalo recebido.
 * Para cada ficheiro, usa lseek(2) para saltar para local_start e alinha à próxima
 * fronteira de linha antes de iniciar o parsing. O progresso é enviado a cada 100 linhas.
 *
 * @param ficheiros       Array de caminhos dos ficheiros de log.
 * @param total_ficheiros Número de ficheiros.
 * @param m               Estrutura de métricas a preencher.
 * @param sock            Socket para envio de progresso ao pai.
 * @param worker_index    Índice deste worker.
 * @param byte_inicio     Offset global inicial (inclusivo).
 * @param byte_fim        Offset global final (exclusivo).
 * @param verbose         Se não-zero, imprime informação de debug no stdout.
 */
static void processar_por_bytes(char **ficheiros, int total_ficheiros, Metrics *m,
                                int sock, int worker_index,
                                off_t byte_inicio, off_t byte_fim, int verbose) {
    off_t quota         = byte_fim - byte_inicio;
    off_t global_offset = 0;
    long  linhas_feitas = 0;

    char buf[BUF_SIZE];
    char linha[LINHA_MAX];

    for (int i = 0; i < total_ficheiros; i++) {
        struct stat st;
        if (stat(ficheiros[i], &st) != 0) continue;
        off_t fsize = st.st_size;

        /* Ficheiro completamente anterior à fatia: avançar offset e continuar */
        if (global_offset + fsize <= byte_inicio) {
            global_offset += fsize;
            continue;
        }
        /* Ficheiro completamente posterior à fatia: não há mais dados a processar */
        if (global_offset >= byte_fim) break;

        off_t local_start = (byte_inicio > global_offset) ? byte_inicio - global_offset : 0;
        off_t local_end   = (byte_fim < global_offset + fsize) ? byte_fim - global_offset : fsize;

        int fd = open(ficheiros[i], O_RDONLY);
        if (fd < 0) { global_offset += fsize; continue; }

        if (verbose)
            printf("[Worker %d] %s local [%lld-%lld]\n",
                   worker_index, ficheiros[i],
                   (long long)local_start, (long long)local_end);

        if (lseek(fd, local_start, SEEK_SET) < 0) {
            perror("lseek");
            close(fd);
            global_offset += fsize;
            continue;
        }

        off_t file_pos = local_start;

        /*
         * Alinhamento de linha: se local_start > 0, o cursor pode estar a meio de
         * uma linha pertencente ao worker anterior. Avançar byte a byte até ao '\n'.
         */
        if (local_start > 0) {
            char    c;
            ssize_t r;
            while ((r = read(fd, &c, 1)) == 1) {
                file_pos++;
                if (c == '\n') break;
            }
            if (r <= 0) {
                close(fd);
                global_offset += fsize;
                continue;
            }
        }

        int len  = 0;
        int done = 0;
        LogFormat fmt = FORMAT_UNKNOWN;

        while (!done) {
            ssize_t n = read(fd, buf, BUF_SIZE);
            if (n <= 0) break;

            for (int b = 0; b < (int)n && !done; b++) {
                char c = buf[b];
                file_pos++;

                if (c == '\n') {
                    if (len > 0) {
                        linha[len] = '\0';
                        if (fmt == FORMAT_UNKNOWN) fmt = detect_format(linha);
                        LogEntry entrada;
                        if (parse_line(linha, fmt, &entrada) == 0)
                            update_metrics(m, &entrada);
                        len = 0;
                    }

                    linhas_feitas++;
                    if (linhas_feitas % 100 == 0) {
                        off_t bytes_done = global_offset + file_pos - byte_inicio;
                        if (bytes_done > quota) bytes_done = quota;
                        enviar_progresso(sock, worker_index,
                                         (long)bytes_done, (long)quota);
                    }

                    if (file_pos >= local_end) done = 1;

                } else if (c != '\r') {
                    if (len < LINHA_MAX - 1) linha[len++] = c;
                }
            }
        }

        /* Tratar última linha sem '\n' */
        if (len > 0) {
            linha[len] = '\0';
            if (fmt == FORMAT_UNKNOWN) fmt = detect_format(linha);
            LogEntry entrada;
            if (parse_line(linha, fmt, &entrada) == 0)
                update_metrics(m, &entrada);
        }

        close(fd);
        global_offset += fsize;
    }
}

/**
 * @brief Ponto de entrada do processo filho — conecta ao servidor e processa a fatia atribuída.
 *
 * @details O pai já foi forked antes desta chamada. O filho conecta ao servidor pai via
 * Unix Domain Socket, lê a configuração (WorkerConfig com os limites da sua fatia),
 * processa os ficheiros e envia progresso + resultado de volta pelo socket.
 *
 * Os parâmetros num_processos e worker_index_original não são utilizados porque a
 * configuração efectiva (worker_index, byte_inicio, byte_fim) é recebida do servidor
 * via MSG_CONFIG — isto permite que o pai atribua fatias dinamicamente após o fork.
 *
 * @param ficheiros              Array de caminhos dos ficheiros de log.
 * @param total_ficheiros        Número de ficheiros.
 * @param num_processos          Não utilizado (configuração recebida via socket).
 * @param worker_index_original  Não utilizado (configuração recebida via socket).
 * @param verbose                Se não-zero, imprime informação de debug no stdout.
 */
void run_worker(char **ficheiros, int total_ficheiros, int num_processos, int worker_index_original, int verbose) {
    (void)num_processos;        /* configuração recebida via MSG_CONFIG */
    (void)worker_index_original;

    int sock = connect_to_server();
    if (sock < 0) { perror("connect_to_server"); exit(1); }

    /* Ler o tipo da primeira mensagem; deve ser sempre MSG_CONFIG */
    int tipo;
    read(sock, &tipo, sizeof(tipo));

    if (tipo != MSG_CONFIG) {
        fprintf(stderr, "Worker esperava MSG_CONFIG, recebeu tipo %d\n", tipo);
        exit(1);
    }

    WorkerConfig cfg;
    read(sock, &cfg, sizeof(cfg));

    int   worker_index = cfg.worker_index;
    off_t byte_inicio  = cfg.byte_inicio;
    off_t byte_fim     = cfg.byte_fim;
    off_t quota        = byte_fim - byte_inicio;

    if (verbose)
        printf("[Worker %d (PID %d)] intervalo bytes: [%lld, %lld) quota=%lld\n",
               worker_index, (int)getpid(),
               (long long)byte_inicio, (long long)byte_fim, (long long)quota);

    Metrics m;
    init_metrics(&m);

    processar_por_bytes(ficheiros, total_ficheiros, &m,
                        sock, worker_index, byte_inicio, byte_fim, verbose);

    /* Progresso final a 100 % antes de enviar o resultado */
    enviar_progresso(sock, worker_index, (long)quota, (long)quota);
    enviar_resultado(sock, &m);

    /* Fechar o socket sinaliza EOF ao pai, que decrementa o contador de resultados pendentes */
    close(sock);
}
