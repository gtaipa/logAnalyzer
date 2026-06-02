/**
 * @file worker_pipes.c
 * @brief Processo filho para análise paralela de logs com comunicação via pipes POSIX.
 *
 * @details
 * Cada processo filho recebe uma fatia [byte_inicio, byte_fim) do espaço de endereçamento
 * virtual formado pela concatenação lógica de todos os ficheiros de log e comunica
 * o seu progresso e resultado ao pai exclusivamente através de um pipe unidireccional.
 *
 * Fluxo de execução do processo filho:
 *  1. Receber os limites [byte_inicio, byte_fim) como argumentos directos da função.
 *  2. Para cada ficheiro, calcular a intersecção local com o intervalo global.
 *  3. Usar lseek(2) para posicionar o cursor e alinhar à fronteira da linha seguinte.
 *  4. Ler em blocos de BUF_SIZE bytes, parsear e acumular métricas locais.
 *  5. A cada 100 linhas, enviar MSG_PROGRESSO ao pai (int + ProgressUpdate).
 *  6. No fim, enviar MSG_RESULTADO com métricas finais e top-10 IPs (int + WorkerResult).
 *
 * Protocolo de mensagens (unidirecional: filho → pai):
 *  - MSG_PROGRESSO (tipo 1): int + ProgressUpdate
 *  - MSG_RESULTADO (tipo 2): int + WorkerResult
 */

#include "ipc.h"
#include "parser.h"
#include "posix_io.h"
#include "worker.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/** @brief Dimensão do buffer de leitura por invocação de read(2) (4 KiB — alinhado ao tamanho de página). */
#define BUF_SIZE       4096

/** @brief Comprimento máximo de uma linha de log aceite pelo parser (caracteres incluindo '\0'). */
#define LINE_MAX_LOCAL 512

/** @brief Tipo de mensagem de progresso enviada periodicamente ao processo pai. */
#define MSG_PROGRESSO  1

/** @brief Tipo de mensagem de resultado final enviada ao processo pai após concluir a fatia. */
#define MSG_RESULTADO  2

/**
 * @brief Envia uma mensagem de progresso ao processo pai via pipe.
 *
 * @details A mensagem é composta por um inteiro de tipo (MSG_PROGRESSO) seguido de uma
 * estrutura ProgressUpdate. O pai usa estes dados para actualizar o dashboard de barras
 * de progresso em tempo real. As escritas não usam writen() porque um progresso perdido
 * não é fatal — o dashboard simplesmente não actualiza nesse ciclo.
 *
 * @param pipe_fd     Descritor do extremo de escrita do pipe.
 * @param worker_index Índice deste worker (0..N-1), usado pelo pai para indexar o dashboard.
 * @param bytes_done  Bytes já processados nesta quota.
 * @param bytes_total Total de bytes desta quota (denominador da percentagem).
 */
static void enviar_progresso(int pipe_fd, int worker_index, long bytes_done, long bytes_total) {
    int tipo = MSG_PROGRESSO;
    write(pipe_fd, &tipo, sizeof(tipo));

    ProgressUpdate pu;
    pu.pid          = getpid();
    pu.worker_index = worker_index;
    pu.bytes_done   = bytes_done;
    pu.bytes_total  = bytes_total;
    write(pipe_fd, &pu, sizeof(pu));
}

/**
 * @brief Ordena os IPs por frequência decrescente e prepara o resultado final.
 *
 * @details Usa bubble sort sobre os arrays paralelos ips[] e counts[] para ordenar
 * por contagem decrescente. O algoritmo O(n²) é aceitável porque MAX_IPS é pequeno
 * (tipicamente ≤ 256 entradas). Os 10 IPs mais frequentes são copiados para o resultado.
 *
 * @param m Métricas acumuladas durante o processamento da fatia.
 * @param r Estrutura de resultado a preencher com os dados a enviar ao pai.
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
                strncpy(tmp_ip,      ips[j],     IP_LEN);
                strncpy(ips[j],      ips[j + 1], IP_LEN);
                strncpy(ips[j + 1],  tmp_ip,     IP_LEN);
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
 * @brief Envia o resultado final ao processo pai via pipe.
 *
 * @details Serializa a estrutura WorkerResult (precedida pelo tipo MSG_RESULTADO)
 * directamente no pipe. O pai lê a struct como um bloco contíguo após ler o tipo.
 * Após esta chamada, o pipe deve ser fechado pelo chamador.
 *
 * @param pipe_fd Descritor do extremo de escrita do pipe.
 * @param m       Métricas acumuladas a serializar.
 */
static void enviar_resultado(int pipe_fd, Metrics *m) {
    int tipo = MSG_RESULTADO;
    write(pipe_fd, &tipo, sizeof(tipo));

    WorkerResult r;
    preparar_resultado(m, &r);
    write(pipe_fd, &r, sizeof(r));
}

/**
 * @brief Processa a fatia [byte_inicio, byte_fim) dos ficheiros de log e comunica com o pai via pipe.
 *
 * @details Esta função é o ponto de entrada do processo filho. Itera sobre todos os ficheiros,
 * calculando a intersecção de cada um com o intervalo [byte_inicio, byte_fim) para determinar
 * os offsets locais a processar. Usa lseek(2) para saltar directamente para local_start,
 * alinha à próxima fronteira de linha se necessário e lê em blocos de BUF_SIZE bytes.
 *
 * O progresso é enviado ao pai a cada 100 linhas; o resultado final é enviado uma vez,
 * seguido do fecho do pipe para sinalizar EOF ao processo pai.
 *
 * @param ficheiros       Array de caminhos absolutos dos ficheiros de log a analisar.
 * @param total_ficheiros Número de entradas em @p ficheiros.
 * @param pipe_fd_write   Descritor do extremo de escrita do pipe para comunicação com o pai.
 * @param worker_index    Índice deste worker (0..N-1).
 * @param byte_inicio     Offset global inicial (inclusivo) da fatia a processar.
 * @param byte_fim        Offset global final (exclusivo) da fatia a processar.
 * @param verbose         Se não-zero, imprime informação de debug no stdout.
 */
void run_worker_pipe(char **ficheiros, int total_ficheiros, int pipe_fd_write,
                     int worker_index, off_t byte_inicio, off_t byte_fim, int verbose) {
    Metrics m;
    init_metrics(&m);

    off_t quota         = byte_fim - byte_inicio; /* total de bytes desta fatia */
    off_t global_offset = 0; /* acumulador do espaço de endereçamento virtual percorrido */
    long  linhas_feitas = 0; /* contador de linhas para cadência de envio de progresso */

    char buf[BUF_SIZE];
    char linha[LINE_MAX_LOCAL];

    if (verbose)
        posix_writef(STDOUT_FILENO,
                     "[Worker %d PID %d] intervalo bytes: [%lld, %lld)\n",
                     worker_index, (int)getpid(),
                     (long long)byte_inicio, (long long)byte_fim);

    for (int i = 0; i < total_ficheiros; i++) {
        struct stat st;
        if (stat(ficheiros[i], &st) != 0) continue; /* ficheiro inacessível: ignorar */
        off_t fsize = st.st_size;

        /* Ficheiro completamente anterior à fatia: avançar offset e continuar */
        if (global_offset + fsize <= byte_inicio) {
            global_offset += fsize;
            continue;
        }
        /* Ficheiro completamente posterior à fatia: não há mais dados a processar */
        if (global_offset >= byte_fim) break;

        /*
         * Calcular offsets locais dentro deste ficheiro:
         *   local_start: byte a partir do qual esta fatia começa (0 se o ficheiro
         *                começa dentro da fatia ou antes dela).
         *   local_end:   byte até ao qual ler (exclusivo); clampado a fsize.
         */
        off_t local_start = (byte_inicio > global_offset) ? byte_inicio - global_offset : 0;
        off_t local_end   = (byte_fim < global_offset + fsize) ? byte_fim - global_offset : fsize;

        int fd = open(ficheiros[i], O_RDONLY);
        if (fd < 0) { global_offset += fsize; continue; }

        if (verbose)
            posix_writef(STDOUT_FILENO,
                         "[Worker %d] %s local [%lld-%lld]\n",
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
         * uma linha pertencente ao worker anterior. Avançar byte a byte até ao '\n'
         * para garantir que o parsing começa sempre numa linha completa.
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
        LogFormat fmt = FORMAT_UNKNOWN; /* inferido na primeira linha válida */

        while (!done) {
            ssize_t n = read(fd, buf, BUF_SIZE);
            if (n <= 0) break; /* EOF ou erro de I/O */

            for (int b = 0; b < (int)n && !done; b++) {
                char c = buf[b];
                file_pos++;

                if (c == '\n') {
                    if (len > 0) {
                        linha[len] = '\0';
                        if (fmt == FORMAT_UNKNOWN) fmt = detect_format(linha);
                        LogEntry entry;
                        if (parse_line(linha, fmt, &entry) == 0)
                            update_metrics(&m, &entry);
                        len = 0;
                    }

                    linhas_feitas++;
                    if (linhas_feitas % 100 == 0) {
                        /* Enviar progresso periódico ao pai para actualização do dashboard */
                        off_t bytes_done = global_offset + file_pos - byte_inicio;
                        if (bytes_done > quota) bytes_done = quota;
                        enviar_progresso(pipe_fd_write, worker_index,
                                         (long)bytes_done, (long)quota);
                    }

                    if (file_pos >= local_end) done = 1;

                } else if (c != '\r') {
                    /* Ignorar '\r' de terminadores CRLF (ficheiros Windows) */
                    if (len < LINE_MAX_LOCAL - 1) linha[len++] = c;
                }
            }
        }

        /* Tratar última linha sem '\n' (ficheiro sem newline final ou done activado a meio) */
        if (len > 0) {
            linha[len] = '\0';
            if (fmt == FORMAT_UNKNOWN) fmt = detect_format(linha);
            LogEntry entry;
            if (parse_line(linha, fmt, &entry) == 0)
                update_metrics(&m, &entry);
        }

        close(fd);
        global_offset += fsize;
    }

    /* Progresso final a 100 % para que o dashboard marque este worker como concluído */
    enviar_progresso(pipe_fd_write, worker_index, (long)quota, (long)quota);

    enviar_resultado(pipe_fd_write, &m);

    /* Fechar o pipe sinaliza EOF ao processo pai no select(); qualquer erro é fatal */
    if (close(pipe_fd_write) == -1) {
        perror("close");
        exit(1);
    }

    exit(0);
}
