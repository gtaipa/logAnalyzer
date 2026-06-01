/**
 * @file worker_sockets.c
 * @brief Processo FILHO — cliente Unix Domain Socket e motor de análise de logs.
 *
 * @details
 * Este ficheiro implementa toda a lógica do processo filho (worker).
 * Cada worker é criado pelo pai via `fork()` e corre independentemente,
 * comunicando com o pai exclusivamente através de um Unix Domain Socket.
 *
 * **Fluxo de execução do worker:**
 *
 *  1. **Ligação ao servidor** — chama `connect_to_server()`, que internamente
 *     cria um socket `AF_UNIX / SOCK_STREAM` e faz `connect()` ao ficheiro
 *     de socket criado pelo pai com `bind()` / `listen()`.
 *
 *  2. **Recepção da configuração (`MSG_CONFIG`)** — lê do socket um `int`
 *     com o tipo `MSG_CONFIG` seguido de uma estrutura `WorkerConfig` que
 *     contém o intervalo de bytes `[byte_inicio, byte_fim)` a processar.
 *
 *  3. **Processamento da fatia de bytes** — para cada ficheiro cujo intervalo
 *     se sobreponha ao intervalo atribuído, usa `open()` + `lseek()` para
 *     posicionar directamente no offset correcto e lê apenas os bytes da sua
 *     quota.  As fronteiras são ajustadas para coincidir com limites de linha
 *     (`\n`), evitando processar linhas parciais.
 *
 *  4. **Envio de progresso (`MSG_PROGRESSO`)** — a cada 100 linhas processadas,
 *     o worker escreve no socket um `int MSG_PROGRESSO` seguido de um
 *     `ProgressUpdate` com o número de bytes já processados.  O pai usa isto
 *     para actualizar o dashboard.
 *
 *  5. **Envio do resultado final (`MSG_RESULTADO`)** — após processar toda a
 *     sua fatia, o worker escreve um `int MSG_RESULTADO` seguido de um
 *     `WorkerResult` com todas as métricas acumuladas.
 *
 *  6. **Fecho do socket e terminação** — fecha o socket e regressa para que
 *     o pai chame `exit(0)`.
 *
 * **Protocolo de mensagens (worker → pai):**
 * ```
 *   [int MSG_PROGRESSO]  + [ProgressUpdate]   — enviado periodicamente
 *   [int MSG_RESULTADO]  + [WorkerResult]      — enviado uma única vez, no final
 * ```
 *
 * **Tratamento de fronteiras de linha:**
 * - Se `byte_inicio` cai no meio de um ficheiro, o worker avança byte a byte
 *   até encontrar um `\n`, garantindo que não começa a processar uma linha
 *   que pertence ao worker anterior.
 * - Se `byte_fim` cai a meio de uma linha, o worker continua a ler até ao
 *   próximo `\n`, garantindo que a linha não é truncada.
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

/** @brief Tamanho do buffer de leitura em bytes (leitura em blocos para eficiência de I/O). */
#define BUF_SIZE  4096
/** @brief Comprimento máximo de uma linha de log suportada pelo parser. */
#define LINHA_MAX  512

/* ─────────────────────────────────────────────────────────────────────────────
 * Funções para enviar mensagens ao pai
 * ─────────────────────────────────────────────────────────────────────────────
 * Funções para enviar mensagens ao pai via Unix Domain Socket
 * ───────────────────────────────────────────────────────────────────────────── */

/**
 * @brief Envia uma mensagem `MSG_PROGRESSO` ao processo pai via socket.
 *
 * @details
 * Escreve dois blocos sequenciais no socket:
 *  1. Um `int` com o valor `MSG_PROGRESSO` — o pai lê este int primeiro para
 *     saber o que se segue (protocolo type-then-payload).
 *  2. Uma estrutura `ProgressUpdate` com os contadores de progresso.
 *
 * Ambas as chamadas `write()` são chamadas ao sistema que transferem dados do
 * espaço de utilizador para o buffer do kernel associado ao socket; o kernel
 * entrega-os ao processo pai quando este fizer `read()`.
 *
 * @param sock          File descriptor do socket ligado ao pai.
 * @param worker_index  Índice deste worker no conjunto de filhos (0-based).
 * @param bytes_done    Número de bytes já processados por este worker.
 * @param bytes_total   Total de bytes atribuídos a este worker (quota).
 */
static void enviar_progresso(int sock, int worker_index, long bytes_done, long bytes_total) {
    /* Enviar o tipo de mensagem primeiro — o pai identifica o que se segue */
    int tipo = MSG_PROGRESSO;
    write(sock, &tipo, sizeof(tipo));

    /* Preencher e enviar a estrutura de progresso */
    ProgressUpdate pu;
    /* getpid() retorna o PID do processo actual (o filho) — identificação para o pai */
    pu.pid          = getpid();
    pu.worker_index = worker_index;
    pu.bytes_done   = bytes_done;
    pu.bytes_total  = bytes_total;
    /* write() envia o struct ProgressUpdate como uma sequência de bytes raw;
     * o pai faz read() com o mesmo sizeof para reconstruir a estrutura */
    write(sock, &pu, sizeof(pu));
}

/**
 * @brief Preenche uma estrutura `WorkerResult` a partir das métricas acumuladas.
 *
 * @details
 * Copia os contadores de `Metrics` para `WorkerResult` e ordena os IPs por
 * frequência decrescente (bubble sort), retendo apenas os 10 mais frequentes.
 * Esta ordenação local simplifica o trabalho de fusão no pai.
 *
 * @param m  Métricas acumuladas durante o processamento da fatia de logs.
 * @param r  Estrutura de resultado a preencher (saída).
 */
static void preparar_resultado(const Metrics *m, WorkerResult *r) {
    /* Zerrar toda a estrutura para garantir que não há lixo nos campos não usados */
    memset(r, 0, sizeof(*r));
    /* Identificação do worker que gerou este resultado */
    r->pid            = getpid();
    /* Copiar contadores de linhas e níveis de log */
    r->total_lines    = m->total_lines;
    r->count_debug    = m->count_debug;
    r->count_info     = m->count_info;
    r->count_warn     = m->count_warn;
    r->count_error    = m->count_error;
    r->count_critical = m->count_critical;
    /* Copiar contadores de erros HTTP */
    r->count_4xx      = m->count_4xx;
    r->count_5xx      = m->count_5xx;

    /* Copiar a lista de IPs para um array local antes de ordenar,
     * para não modificar as métricas originais */
    char ips[MAX_IPS][IP_LEN];
    long counts[MAX_IPS];
    int n = m->ip_num;
    if (n > MAX_IPS) n = MAX_IPS;

    for (int i = 0; i < n; i++) {
        strncpy(ips[i], m->ip_list[i], IP_LEN - 1);
        ips[i][IP_LEN - 1] = '\0';
        counts[i] = m->ip_count[i];
    }

    /* Ordenar os IPs por contagem decrescente (bubble sort).
     * Embora O(n²), n ≤ MAX_IPS é pequeno e este código corre uma única
     * vez por worker no final do processamento. */
    for (int i = 0; i < n - 1; i++) {
        for (int j = 0; j < n - i - 1; j++) {
            if (counts[j] < counts[j + 1]) {
                /* Trocar contagens */
                long tmp_count = counts[j];
                counts[j] = counts[j + 1];
                counts[j + 1] = tmp_count;

                /* Trocar os IPs correspondentes */
                char tmp_ip[IP_LEN];
                strncpy(tmp_ip, ips[j], IP_LEN);
                strncpy(ips[j], ips[j + 1], IP_LEN);
                strncpy(ips[j + 1], tmp_ip, IP_LEN);
            }
        }
    }

    /* Guardar apenas os 10 IPs mais frequentes no resultado */
    int limite = n < 10 ? n : 10;
    for (int i = 0; i < limite; i++) {
        strncpy(r->top_ips[i], ips[i], IP_LEN - 1);
        r->top_ips[i][IP_LEN - 1] = '\0';
        r->top_ips_counts[i] = counts[i];
    }

    /* Copiar alertas críticos, respeitando o limite máximo */
    r->num_alerts = m->num_alerts < MAX_ALERTS ? m->num_alerts : MAX_ALERTS;
    for (int i = 0; i < r->num_alerts; i++) {
        strncpy(r->alerts[i], m->alerts[i], ALERT_LEN - 1);
        r->alerts[i][ALERT_LEN - 1] = '\0';
    }
}

/**
 * @brief Envia a mensagem `MSG_RESULTADO` com as métricas finais ao processo pai.
 *
 * @details
 * Escreve dois blocos sequenciais no socket:
 *  1. Um `int` com `MSG_RESULTADO` — sinaliza ao pai que este é o último
 *     envio deste worker.
 *  2. Uma estrutura `WorkerResult` com todas as métricas.
 *
 * Após esta chamada, o worker pode fechar o socket e terminar.
 *
 * @param sock  File descriptor do socket ligado ao pai.
 * @param m     Métricas acumuladas a enviar.
 */
static void enviar_resultado(int sock, Metrics *m) {
    /* Enviar o tipo de mensagem — o pai sabe que a seguir vem um WorkerResult */
    int tipo = MSG_RESULTADO;
    write(sock, &tipo, sizeof(tipo));

    /* Preparar e serializar o resultado final.
     * write() envia a estrutura completa como bytes raw pelo socket. */
    WorkerResult r;
    preparar_resultado(m, &r);
    write(sock, &r, sizeof(r));
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Processamento por fatia de bytes
 *
 * Recebe o intervalo global [byte_inicio, byte_fim).  Para cada ficheiro cujo
 * intervalo de bytes se sobreponha ao nosso, usa lseek() para saltar directamente
 * para o offset correcto e lê apenas os bytes que nos pertencem.
 *
 * Tratamento de fronteiras:
 *   • Início no meio de um ficheiro → avança até ao próximo '\n' (evita linha
 *     parcial que pertence ao worker anterior).
 *   • Fim no meio de uma linha → continua a ler até ao '\n' (para não partir
 *     uma linha entre dois workers).
 * ─────────────────────────────────────────────────────────────────────────────
 * Processamento de lógicas por fatia de bytes com tratamento de fronteiras
 * ───────────────────────────────────────────────────────────────────────────── */

/**
 * @brief Processa a fatia de bytes atribuída a este worker em todos os ficheiros relevantes.
 *
 * @details
 * Itera pelos ficheiros da lista, identificando quais se sobrepõem ao intervalo
 * `[byte_inicio, byte_fim)`.  Para cada ficheiro relevante:
 *
 *  - Usa `stat()` para obter o tamanho sem abrir o conteúdo.
 *  - Usa `open()` + `lseek()` para posicionar directamente no offset local
 *    correcto, evitando ler bytes desnecessários.
 *  - Ajusta a fronteira de início para o próximo `\n` (linha completa).
 *  - Lê em blocos de `BUF_SIZE` bytes para minimizar syscalls `read()`.
 *  - Acumula caracteres num buffer de linha; ao encontrar `\n`, passa a linha
 *    ao parser e actualiza as métricas.
 *  - Envia `MSG_PROGRESSO` ao pai a cada 100 linhas.
 *  - Para de processar quando atinge `byte_fim` e a linha corrente está completa.
 *
 * @param ficheiros        Lista de caminhos de todos os ficheiros de log.
 * @param total_ficheiros  Número de ficheiros na lista.
 * @param m                Estrutura de métricas a actualizar (in/out).
 * @param sock             Socket ligado ao pai para enviar progresso.
 * @param worker_index     Índice deste worker (para identificar na mensagem de progresso).
 * @param byte_inicio      Offset global (inclusivo) onde este worker começa a ler.
 * @param byte_fim         Offset global (exclusivo) onde este worker para de ler.
 * @param verbose          1 se o modo verboso está activo; 0 caso contrário.
 */
static void processar_por_bytes(char **ficheiros, int total_ficheiros, Metrics *m,
                                int sock, int worker_index,
                                off_t byte_inicio, off_t byte_fim, int verbose) {
    off_t quota         = byte_fim - byte_inicio;  /* total de bytes a processar */
    off_t global_offset = 0;  /* posição acumulada no espaço global de bytes */
    long  linhas_feitas = 0;  /* contador de linhas para disparar progresso */

    char buf[BUF_SIZE];    /* buffer de leitura em bloco */
    char linha[LINHA_MAX]; /* buffer de acumulação de uma linha */

    for (int i = 0; i < total_ficheiros; i++) {
        struct stat st;
        /* stat() obtém metadados do ficheiro (tamanho, permissões, etc.)
         * sem o abrir — eficiente porque não faz I/O de conteúdo */
        if (stat(ficheiros[i], &st) != 0) continue;
        off_t fsize = st.st_size;

        /* Ficheiro completamente antes da nossa fatia — saltar */
        if (global_offset + fsize <= byte_inicio) {
            global_offset += fsize;
            continue;
        }
        /* Ficheiro completamente depois da nossa fatia — terminar o loop */
        if (global_offset >= byte_fim) break;

        /* Calcular os offsets locais (dentro deste ficheiro) correspondentes
         * aos nossos limites globais */
        off_t local_start = (byte_inicio > global_offset) ? byte_inicio - global_offset : 0;
        off_t local_end   = (byte_fim < global_offset + fsize) ? byte_fim - global_offset : fsize;

        /* open() abre o ficheiro em modo apenas-leitura (O_RDONLY).
         * Devolve um file descriptor — índice na tabela de FDs do processo. */
        int fd = open(ficheiros[i], O_RDONLY);
        if (fd < 0) { global_offset += fsize; continue; }

        if (verbose)
            printf("[Worker %d] %s local [%lld-%lld]\n",
                   worker_index, ficheiros[i],
                   (long long)local_start, (long long)local_end);

        /* lseek() posiciona o cursor de leitura directamente em local_start,
         * evitando ler todos os bytes anteriores.
         * SEEK_SET — offset absoluto a partir do início do ficheiro.
         * Sem lseek(), teríamos de ler e descartar bytes desnecessários. */
        if (lseek(fd, local_start, SEEK_SET) < 0) {
            perror("lseek");
            close(fd);
            global_offset += fsize;
            continue;
        }

        off_t file_pos = local_start;

        /* Se não estamos no início do ficheiro, avançar até ao próximo '\n'.
         * Isto garante que não processamos uma linha parcial que pertence
         * ao worker anterior (que termina exactamente no nosso byte_inicio). */
        if (local_start > 0) {
            char c;
            ssize_t r;
            /* Leitura byte a byte até encontrar '\n' — ineficiente mas
             * executado apenas uma vez por ficheiro na fronteira de início */
            while ((r = read(fd, &c, 1)) == 1) {
                file_pos++;
                if (c == '\n') break;  /* encontrámos o início da próxima linha */
            }
            if (r <= 0) {
                /* EOF ou erro antes de encontrar '\n' — nada a fazer neste ficheiro */
                close(fd);
                global_offset += fsize;
                continue;
            }
        }

        int len  = 0;     /* número de caracteres acumulados na linha atual */
        int done = 0;     /* flag de paragem */
        LogFormat fmt = FORMAT_UNKNOWN;  /* formato do log (detectado na primeira linha) */

        /* Loop principal de leitura em blocos */
        while (!done) {
            /* read() lê até BUF_SIZE bytes do ficheiro para o buffer.
             * Retorna o número de bytes efectivamente lidos (pode ser menor),
             * 0 em EOF, ou -1 em erro.  Ler em blocos grandes reduz o número
             * de syscalls em comparação com leitura byte a byte. */
            ssize_t n = read(fd, buf, BUF_SIZE);
            if (n <= 0) break;  /* EOF ou erro — terminar */

            /* Processar byte a byte o bloco lido */
            for (int b = 0; b < (int)n && !done; b++) {
                char c = buf[b];
                file_pos++;

                if (c == '\n') {
                    /* Fim de linha — processar a linha acumulada */
                    if (len > 0) {
                        linha[len] = '\0';
                        /* detect_format() identifica o formato do log (Apache,
                         * syslog, JSON, etc.) na primeira linha do ficheiro */
                        if (fmt == FORMAT_UNKNOWN) fmt = detect_format(linha);
                        LogEntry entrada;
                        /* parse_line() extrai os campos relevantes da linha
                         * (timestamp, nível, IP, código HTTP, etc.) */
                        if (parse_line(linha, fmt, &entrada) == 0)
                            /* update_metrics() acumula os dados da entrada
                             * nos contadores de métricas */
                            update_metrics(m, &entrada);
                        len = 0;
                    }

                    linhas_feitas++;
                    /* Enviar progresso a cada 100 linhas para não sobrecarregar
                     * o socket com mensagens demasiado frequentes */
                    if (linhas_feitas % 100 == 0) {
                        off_t bytes_done = global_offset + file_pos - byte_inicio;
                        if (bytes_done > quota) bytes_done = quota;
                        /* MSG_PROGRESSO: notificar o pai do progresso atual */
                        enviar_progresso(sock, worker_index,
                                         (long)bytes_done, (long)quota);
                    }

                    /* Linha completa e já passámos o fim da nossa fatia → parar */
                    if (file_pos >= local_end) done = 1;

                } else if (c != '\r') {
                    /* Acumular caractere na linha atual (ignorar \r de CRLF) */
                    if (len < LINHA_MAX - 1) linha[len++] = c;
                    /* Se passámos local_end a meio de uma linha, continuamos a acumular
                     * até ao próximo '\n' — evita partir uma linha entre workers */
                }
            }
        }

        /* Última linha sem '\n' (EOF sem terminador de linha final) */
        if (len > 0) {
            linha[len] = '\0';
            if (fmt == FORMAT_UNKNOWN) fmt = detect_format(linha);
            LogEntry entrada;
            if (parse_line(linha, fmt, &entrada) == 0)
                update_metrics(m, &entrada);
        }

        /* close() liberta o file descriptor e os recursos associados no kernel */
        close(fd);
        global_offset += fsize;
    }
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Função Principal do Worker (sockets)
 * ───────────────────────────────────────────────────────────────────────────── */

/**
 * @brief Função principal do processo filho — liga ao pai, processa os logs e envia resultados.
 *
 * @details
 * Esta função é chamada pelo pai logo após `fork()` e nunca regressa ao main:
 * o processo filho termina com `exit(0)` no final.
 *
 * **Sequência de operações:**
 *  1. `connect_to_server()` — cria socket AF_UNIX e faz `connect()` ao pai.
 *  2. `read()` do tipo de mensagem — espera `MSG_CONFIG`.
 *  3. `read()` de `WorkerConfig` — obtém `[byte_inicio, byte_fim)`.
 *  4. `processar_por_bytes()` — lê e analisa a fatia de ficheiros.
 *  5. `enviar_progresso()` — confirma 100% de conclusão.
 *  6. `enviar_resultado()` — envia `WorkerResult` final ao pai.
 *  7. `close()` — fecha o socket e termina.
 *
 * @param ficheiros               Lista de caminhos de todos os ficheiros de log.
 * @param total_ficheiros         Número de ficheiros na lista.
 * @param num_processos           Número total de workers criados pelo pai (não usado directamente aqui).
 * @param worker_index_original   Índice atribuído pelo pai antes do fork (não usado; o índice real vem em WorkerConfig).
 * @param verbose                 1 se o modo verboso está activo; 0 caso contrário.
 */
void run_worker(char **ficheiros, int total_ficheiros, int num_processos, int worker_index_original, int verbose) {
    (void)num_processos;
    (void)worker_index_original;

    /* connect_to_server() cria internamente um socket AF_UNIX/SOCK_STREAM
     * e chama connect() para se ligar ao ficheiro SOCKET_PATH criado pelo pai.
     * connect() bloqueia até o pai aceitar a ligação com accept(). */
    int sock = connect_to_server();
    if (sock < 0) { perror("connect_to_server"); exit(1); }

    /* Ler o tipo da primeira mensagem enviada pelo pai.
     * read() bloqueia até o pai enviar os dados (após accept()). */
    int tipo;
    read(sock, &tipo, sizeof(tipo));

    /* Verificar que o pai enviou MSG_CONFIG — é o único tipo esperado aqui */
    if (tipo != MSG_CONFIG) {
        fprintf(stderr, "Worker esperava MSG_CONFIG, recebeu tipo %d\n", tipo);
        exit(1);
    }

    /* Ler a estrutura WorkerConfig que o pai enviou imediatamente a seguir ao tipo.
     * Contém o intervalo [byte_inicio, byte_fim) que este worker deve processar. */
    WorkerConfig cfg;
    read(sock, &cfg, sizeof(cfg));

    /* Extrair os campos relevantes da configuração recebida */
    int   worker_index = cfg.worker_index;   /* índice deste worker na pool */
    off_t byte_inicio  = cfg.byte_inicio;    /* offset global de início (inclusivo) */
    off_t byte_fim     = cfg.byte_fim;       /* offset global de fim (exclusivo) */
    off_t quota        = byte_fim - byte_inicio;  /* total de bytes a processar */

    if (verbose)
        printf("[Worker %d (PID %d)] intervalo bytes: [%lld, %lld) quota=%lld\n",
               worker_index, (int)getpid(),
               (long long)byte_inicio, (long long)byte_fim, (long long)quota);

    /* Inicializar a estrutura de métricas a zeros */
    Metrics m;
    init_metrics(&m);

    /* Processar a fatia de bytes atribuída, enviando progresso ao pai
     * via socket à medida que avança */
    processar_por_bytes(ficheiros, total_ficheiros, &m,
                        sock, worker_index, byte_inicio, byte_fim, verbose);

    /* Enviar uma última mensagem de progresso a 100% para garantir que
     * o dashboard do pai mostra a barra completa para este worker */
    enviar_progresso(sock, worker_index, (long)quota, (long)quota);

    /* Enviar o resultado final ao pai via MSG_RESULTADO + WorkerResult.
     * Após este envio, o pai acumulará as métricas e fechará o socket. */
    enviar_resultado(sock, &m);

    /* Fechar o socket — liberta o file descriptor no processo filho.
     * O pai detecta o fecho quando read() devolve 0 (EOF), mas o protocolo
     * já prevê que o pai fechou o socket ao receber MSG_RESULTADO. */
    close(sock);
}
