/**
 * @file worker_threads.c
 * @brief Implementação do worker thread para análise paralela de logs via data parallelism.
 *
 * @details
 * Cada thread worker recebe uma fatia [byte_inicio, byte_fim) do espaço de endereçamento
 * virtual formado pela concatenação lógica de todos os ficheiros de log. O padrão
 * utilizado é **data parallelism**: o mesmo código executa concorrentemente sobre
 * partições disjuntas dos dados, sem coordenação durante a fase de leitura.
 *
 * Fluxo de execução por thread:
 *  1. Inicializar métricas locais (privadas à thread, sem contenção no mutex).
 *  2. Iterar os ficheiros e calcular os offsets locais correspondentes à fatia global.
 *  3. Invocar lseek(2) para posicionar o cursor e alinhar à fronteira da linha seguinte.
 *  4. Ler em blocos de BUF_SIZE bytes e acumular resultados em local_metrics.
 *  5. Adquirir o mutex, fundir local_metrics em global_metrics e libertar o mutex.
 *
 * O uso de métricas locais durante o processamento elimina a contenção no mutex
 * ao longo do loop de leitura. A exclusão mútua é adquirida apenas uma vez,
 * na fase de fusão final, minimizando a janela crítica e maximizando o throughput.
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

/** @brief Dimensão do buffer de I/O por invocação de read(2) (4 KiB — alinhado ao tamanho de página). */
#define BUF_SIZE 4096

/** @brief Comprimento máximo de uma linha de log aceite pelo parser (caracteres incluindo '\0'). */
#define LINE_MAX 512

/**
 * @brief Ponto de entrada de cada thread worker — processa uma fatia de bytes dos ficheiros
 *        de log e funde as métricas locais nas métricas globais sob exclusão mútua.
 *
 * @param arg Ponteiro genérico (void*) para uma estrutura ThreadArgs preenchida pelo main().
 *            Contém a fatia [byte_inicio, byte_fim), o ponteiro para global_metrics,
 *            o mutex de serialização e os contadores de progresso (bytes_done / bytes_total).
 * @return NULL — os resultados são comunicados via ponteiros partilhados; pthread_join ignora
 *         o valor de retorno (segundo argumento NULL).
 *
 * @details
 * A thread mantém métricas locais na sua stack durante toda a fase de leitura, evitando
 * contenção no mutex partilhado. Apenas na secção crítica de fusão final é que o mutex
 * é adquirido uma única vez, o que garante escalabilidade com múltiplos workers.
 *
 * Sem o mutex na fusão, dois incrementos simultâneos (e.g., count_error += N) seriam
 * não-atómicos em C (leitura + adição + escrita), podendo ambas as threads ler o mesmo
 * valor antes de qualquer escrita — resultando em lost updates (contribuição perdida).
 */
void *run_worker_thread(void *arg) {
    ThreadArgs *t = (ThreadArgs *)arg; /* cast do void* para o tipo concreto ThreadArgs* */

    /* Extrair os limites da fatia global atribuída a esta thread */
    off_t byte_inicio = t->byte_inicio;  /* offset inicial (inclusivo) no espaço virtual */
    off_t byte_fim    = t->byte_fim;     /* offset final (exclusivo) no espaço virtual */
    off_t quota       = byte_fim - byte_inicio; /* total de bytes desta fatia */

    /* Publicar a quota no array de progresso antes de iniciar o processamento */
    *(t->bytes_total) = (long)quota;
    *(t->bytes_done)  = 0;

    /*
     * Métricas locais: alocadas na stack desta thread, invisíveis a qualquer outra.
     * Evitam contenção no mutex durante todo o loop de leitura; o mutex é adquirido
     * apenas uma vez no final, na fase de fusão.
     */
    Metrics local_metrics;
    init_metrics(&local_metrics);

    /*
     * global_offset: acumulador do deslocamento em bytes ao longo dos ficheiros já visitados.
     * Permite mapear os offsets globais [byte_inicio, byte_fim) para coordenadas locais
     * (dentro de cada ficheiro individual), dado que os ficheiros são tratados como um
     * espaço de endereçamento linear e contíguo.
     */
    off_t global_offset = 0;
    char  buf[BUF_SIZE];    /* buffer de leitura reutilizável; preenchido por cada read(2) */
    char  linha[LINE_MAX];  /* acumulador de caracteres para a linha corrente em construção */

    /* ── Iterar ficheiros para identificar os que pertencem à fatia desta thread ── */
    for (int i = 0; i < t->total_ficheiros; i++) {
        struct stat st;
        if (stat(t->ficheiros[i], &st) != 0) continue; /* ignorar ficheiro inacessível */
        off_t fsize = st.st_size; /* tamanho físico em bytes do ficheiro actual */

        /* Ficheiro completamente anterior à fatia: avançar offset acumulado e continuar */
        if (global_offset + fsize <= byte_inicio) {
            global_offset += fsize;
            continue;
        }
        /* Ficheiro completamente posterior à fatia: não há mais dados a processar */
        if (global_offset >= byte_fim) break;

        /*
         * Calcular offsets locais (coordenadas dentro deste ficheiro):
         *   local_start: byte a partir do qual esta thread deve começar a ler.
         *   local_end:   byte até ao qual deve ler (exclusivo); clampado a fsize.
         */
        off_t local_start = (byte_inicio > global_offset) ? byte_inicio - global_offset : 0;
        off_t local_end   = (byte_fim < global_offset + fsize) ? byte_fim - global_offset : fsize;

        int fd = open(t->ficheiros[i], O_RDONLY); /* abrir descritor em modo só-leitura */
        if (fd < 0) { global_offset += fsize; continue; } /* falha de open(2): próximo ficheiro */

        if (t->verbose) /* modo verboso: reportar intervalo de bytes processados no stderr */
            posix_writef(STDERR_FILENO, "[Thread %d] %s bytes [%lld-%lld]\n",
                         t->worker_index, t->ficheiros[i],
                         (long long)local_start, (long long)local_end);

        /*
         * lseek(2): reposicionar o cursor de leitura para local_start (SEEK_SET = absoluto).
         * Sem esta chamada, a leitura começaria no byte 0, processando dados pertencentes
         * a outras threads e violando a invariante de partição disjunta.
         */
        if (lseek(fd, local_start, SEEK_SET) < 0) {
            perror("lseek");
            close(fd);
            global_offset += fsize;
            continue;
        }

        off_t file_pos = local_start; /* posição de leitura corrente dentro do ficheiro */

        /*
         * Alinhamento de linha: se local_start > 0, o cursor pode estar a meio de uma linha
         * pertencente ao worker anterior (que processa os bytes imediatamente antes de
         * local_start). Para evitar parsear uma entrada truncada, avança-se byte a byte
         * até ao '\n' mais próximo — o território desta thread começa sempre no início
         * de uma linha completa.
         */
        if (local_start > 0) {
            char c;
            ssize_t r;
            while ((r = read(fd, &c, 1)) == 1) { /* leitura byte a byte para alinhamento */
                file_pos++;
                if (c == '\n') break; /* fronteira de linha encontrada; terminar alinhamento */
            }
            if (r <= 0) { /* EOF ou erro de I/O antes de encontrar '\n' */
                close(fd);
                global_offset += fsize;
                continue;
            }
        }

        int len  = 0;                   /* número de caracteres no acumulador linha[] */
        int done = 0;                   /* flag: 1 quando a fatia deste ficheiro foi esgotada */
        LogFormat fmt = FORMAT_UNKNOWN; /* formato do log, inferido na primeira linha válida */

        /* ── Loop principal: leitura em blocos de BUF_SIZE bytes ── */
        while (!done) {
            /*
             * read(2): lê até BUF_SIZE bytes para buf. Retorna o número real de bytes
             * lidos (pode ser < BUF_SIZE perto do EOF), 0 em EOF ou -1 em erro.
             */
            ssize_t n = read(fd, buf, BUF_SIZE);
            if (n <= 0) break; /* EOF ou erro de I/O: parar processamento deste ficheiro */

            /* Actualizar progresso da thread no slot do dashboard de monitorização */
            off_t pos_na_quota = (global_offset + file_pos) - byte_inicio;
            if (pos_na_quota < 0)     pos_na_quota = 0;     /* clamp inferior */
            if (pos_na_quota > quota) pos_na_quota = quota;  /* clamp superior */
            *(t->bytes_done) = (long)pos_na_quota;

            /* Processar o bloco lido caracter a caracter para reconstruir linhas */
            for (int b = 0; b < (int)n && !done; b++) {
                char c = buf[b];
                file_pos++; /* avançar posição corrente dentro do ficheiro */

                if (c == '\n') {
                    if (len > 0) {
                        linha[len] = '\0'; /* terminar string C para passagem ao parser */

                        /* Inferir formato (Apache, Nginx, syslog, JSON) apenas na 1.ª linha */
                        if (fmt == FORMAT_UNKNOWN) fmt = detect_format(linha);

                        LogEntry entry;
                        if (parse_line(linha, fmt, &entry) == 0) /* parsing com sucesso */
                            update_metrics(&local_metrics, &entry); /* acumular em métricas locais */
                        len = 0; /* reiniciar acumulador para a linha seguinte */
                    }
                    /* Verificar se ultrapassámos o limite da fatia após processar a linha */
                    if (file_pos >= local_end) done = 1;
                } else if (c != '\r') {
                    /* Acumular caractere; ignorar '\r' de terminadores CRLF (Windows) */
                    if (len < LINE_MAX - 1) linha[len++] = c;
                }
            }
        }

        /*
         * Tratar última linha sem '\n': ocorre quando o ficheiro não termina com newline
         * ou quando done foi activado a meio de uma linha. Processar o conteúdo residual.
         */
        if (len > 0) {
            linha[len] = '\0';
            if (fmt == FORMAT_UNKNOWN) fmt = detect_format(linha);
            LogEntry entry;
            if (parse_line(linha, fmt, &entry) == 0)
                update_metrics(&local_metrics, &entry);
        }

        close(fd);              /* libertar o descritor de ficheiro e os recursos associados */
        global_offset += fsize; /* avançar offset acumulado para o próximo ficheiro */
    }

    /* Forçar progresso a 100 % para que o dashboard assinale esta thread como concluída */
    *(t->bytes_done) = (long)quota;

    /*
     * ── Secção crítica: fusão das métricas locais nas métricas globais ──
     *
     * pthread_mutex_lock(3) bloqueia se outra thread já detiver o mutex, garantindo
     * que apenas uma thread de cada vez executa a fusão. Sem esta protecção, dois
     * incrementos simultâneos (e.g., global->count_error += local->count_error)
     * seriam não-atómicos: ambas as threads poderiam ler o mesmo valor antigo antes
     * de qualquer escrita, e uma das contribuições seria silenciosamente perdida (lost update).
     */
    pthread_mutex_lock(t->mutex);

    /* Fundir contadores escalares dentro da secção crítica */
    t->global_metrics->total_lines    += local_metrics.total_lines;
    t->global_metrics->count_debug    += local_metrics.count_debug;
    t->global_metrics->count_info     += local_metrics.count_info;
    t->global_metrics->count_warn     += local_metrics.count_warn;
    t->global_metrics->count_error    += local_metrics.count_error;
    t->global_metrics->count_critical += local_metrics.count_critical;
    t->global_metrics->count_4xx      += local_metrics.count_4xx;
    t->global_metrics->count_5xx      += local_metrics.count_5xx;

    /*
     * Fusão da tabela de IPs: pesquisa linear sobre a tabela global para cada entrada local.
     * Se o IP já existe, incrementa a contagem; caso contrário, insere como nova entrada.
     * Executada dentro do mutex para serializar modificações concorrentes em ip_list[] / ip_count[].
     */
    for (int i = 0; i < local_metrics.ip_num; i++) {
        int found = -1;
        for (int j = 0; j < t->global_metrics->ip_num; j++) {
            if (strcmp(t->global_metrics->ip_list[j], local_metrics.ip_list[i]) == 0) {
                found = j; /* IP encontrado: guardar índice para actualizar contagem */
                break;
            }
        }
        if (found >= 0) {
            /* IP duplicado: somar a contagem local à entrada global existente */
            t->global_metrics->ip_count[found] += local_metrics.ip_count[i];
        } else if (t->global_metrics->ip_num < MAX_IPS) {
            /* IP novo: inserir na próxima posição livre da tabela global */
            strncpy(t->global_metrics->ip_list[t->global_metrics->ip_num],
                    local_metrics.ip_list[i], IP_LEN - 1);
            t->global_metrics->ip_list[t->global_metrics->ip_num][IP_LEN - 1] = '\0';
            t->global_metrics->ip_count[t->global_metrics->ip_num] = local_metrics.ip_count[i];
            t->global_metrics->ip_num++;
        }
        /* IP descartado silenciosamente quando MAX_IPS é atingido (overflow de tabela) */
    }

    /*
     * Fusão de alertas: copiar alertas locais para a lista global respeitando MAX_ALERTS.
     * Dentro do mutex para evitar escritas concorrentes no array global alerts[].
     */
    for (int i = 0; i < local_metrics.num_alerts && t->global_metrics->num_alerts < MAX_ALERTS; i++) {
        strncpy(t->global_metrics->alerts[t->global_metrics->num_alerts],
                local_metrics.alerts[i], ALERT_LEN - 1);
        t->global_metrics->alerts[t->global_metrics->num_alerts][ALERT_LEN - 1] = '\0';
        t->global_metrics->num_alerts++;
    }

    /*
     * pthread_mutex_unlock(3): libertar o mutex para que a próxima thread bloqueada
     * possa entrar na secção crítica. Um mutex nunca libertado causaria deadlock permanente.
     */
    pthread_mutex_unlock(t->mutex);

    /*
     * pthread_exit(3): terminar esta thread de forma controlada, executando os destruidores
     * de thread-local storage (TLS) antes do encerramento. O pthread_join correspondente
     * no main() desbloqueia imediatamente após esta chamada.
     */
    pthread_exit(NULL);
}
