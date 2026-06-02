/**
 * @file worker_prodcons.c
 * @brief Implementação do padrão Produtor-Consumidor com bounded buffer para análise paralela
 *        de logs.
 *
 * @details
 * Este módulo contém toda a mecânica de sincronização entre threads produtoras e consumidoras:
 *
 *  - **Buffer circular (bounded buffer):** estrutura LogQueue com capacidade fixa BUFFER_SIZE.
 *    Os índices `in` (próximo slot de escrita) e `out` (próximo slot de leitura) avançam em
 *    módulo BUFFER_SIZE, reutilizando ciclicamente as posições do array. O campo `count` mantém
 *    o número de entradas actualmente presentes no buffer.
 *
 *  - **Semáforos POSIX:**
 *      - `buffer.empty`: inicializado a BUFFER_SIZE; decrementado pelo produtor antes de
 *        escrever (bloqueia quando o buffer está cheio, count == BUFFER_SIZE).
 *      - `buffer.full`:  inicializado a 0; decrementado pelo consumidor antes de ler
 *        (bloqueia quando o buffer está vazio, count == 0).
 *
 *  - **Mutex `buffer.mutex`:** garante exclusão mútua no acesso aos índices `in`/`out`/`count`,
 *    evitando data races entre múltiplos produtores e consumidores.
 *
 *  - **`produtores_ativos`:** contador decrementado por cada produtor ao terminar (com o mutex
 *    adquirido). Quando chega a zero, o último produtor envia sinais extra em `buffer.full`
 *    para acordar os consumidores bloqueados em sem_wait(2), permitindo-lhes detectar o EOF
 *    e sair do loop de consumo.
 *
 *  - **`metrics_mutex`:** mutex independente que protege a actualização da estrutura Metrics
 *    global partilhada por todos os consumidores.
 */

#include "parser.h"
#include "posix_io.h"
#include "worker_prodcons.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/stat.h>

/** @brief Dimensão do buffer de I/O por invocação de read(2) (4 KiB — alinhado ao tamanho de página). */
#define BUF_SIZE       4096

/** @brief Comprimento máximo de uma linha de log suportada pelo produtor. */
#define LINE_MAX_LOCAL  512

/**
 * @brief Buffer circular partilhado entre todos os produtores e consumidores.
 *
 * @details
 * Contém o array de LogEntry, os índices `in`/`out`/`count`, o mutex de acesso exclusivo
 * e os semáforos `empty` e `full` de controlo de fluxo. Definido como variável global
 * para ser acessível em main_prodcons.c via declaração `extern` em worker_prodcons.h.
 */
LogQueue        buffer;

/**
 * @brief Número de threads produtoras ainda em execução.
 *
 * @details
 * Inicializado em main() com o número total de produtores lançados. Cada produtor
 * decrementa esta variável (com buffer.mutex adquirido) imediatamente antes de terminar.
 * Quando chega a zero, o último produtor sinaliza os consumidores para que possam
 * detectar o fim da produção e sair do seu loop.
 */
int             produtores_ativos = 0;

/**
 * @brief Mutex que protege a actualização da estrutura de métricas global.
 *
 * @details
 * Todos os consumidores partilham a mesma estrutura Metrics; este mutex garante que
 * apenas um consumidor de cada vez invoca update_metrics(), evitando data races nos
 * contadores acumulados. Inicializado estaticamente com PTHREAD_MUTEX_INITIALIZER.
 */
pthread_mutex_t metrics_mutex = PTHREAD_MUTEX_INITIALIZER;

/**
 * @brief Wrapper seguro em torno de sem_wait(3) que reinicia a espera se interrompido por sinal.
 *
 * @details
 * sem_wait(3) pode retornar -1 com errno == EINTR quando o processo recebe um sinal durante
 * a espera bloqueante. O loop garante que a espera recomeça nesse caso, implementando uma
 * espera bloqueante verdadeiramente fiável e resiliente a sinais assíncronos.
 *
 * @param s Ponteiro para o semáforo POSIX a decrementar; bloqueia se o valor for zero.
 */
static void esperar(sem_t *s) {
    while (sem_wait(s) == -1 && errno == EINTR) /* reiniciar se interrompido por sinal */
        ;
}

/**
 * @brief Wrapper em torno de sem_post(3) para incrementar (sinalizar) um semáforo POSIX.
 *
 * @details
 * Incrementa o semáforo, acordando uma thread bloqueada em esperar() (i.e., em sem_wait(3)).
 * Utilizado pelo produtor para sinalizar buffer.full após inserir uma entrada, e pelo
 * consumidor para sinalizar buffer.empty após retirar uma entrada.
 *
 * @param s Ponteiro para o semáforo POSIX a incrementar.
 */
static void assinalar(sem_t *s) {
    sem_post(s); /* incrementar semáforo e acordar um waiter bloqueado, se existir */
}

/**
 * @brief Inicializa o bounded buffer e os primitivos de sincronização associados.
 *
 * @details
 * Deve ser chamada uma única vez em main() antes de lançar qualquer thread.
 * Configura:
 *  - buffer.in = buffer.out = buffer.count = 0 (buffer vazio)
 *  - pthread_mutex_init(&buffer.mutex) — mutex para exclusão mútua nos índices
 *  - sem_init(&buffer.empty, 0, BUFFER_SIZE) — BUFFER_SIZE slots livres inicialmente
 *  - sem_init(&buffer.full,  0, 0) — zero entradas disponíveis para consumo
 */
void init_bounded_buffer(void) {
    buffer.in    = 0; /* índice de escrita: próxima posição livre para inserção */
    buffer.out   = 0; /* índice de leitura: próxima posição a consumir */
    buffer.count = 0; /* número de entradas actualmente no buffer */

    /* Inicializar mutex de exclusão mútua para acesso aos índices in/out/count */
    pthread_mutex_init(&buffer.mutex, NULL);

    /*
     * Semáforo "empty": conta o número de posições livres no buffer circular.
     * Começa em BUFFER_SIZE (buffer completamente vazio).
     * O produtor decrementa (bloqueia se cheio); o consumidor incrementa (liberta posição).
     */
    sem_init(&buffer.empty, 0, BUFFER_SIZE);

    /*
     * Semáforo "full": conta o número de entradas prontas para consumo.
     * Começa em 0 (buffer vazio, nada para consumir).
     * O consumidor decrementa (bloqueia se vazio); o produtor incrementa (sinaliza nova entrada).
     */
    sem_init(&buffer.full, 0, 0);
}

/**
 * @brief Destrói o mutex e os semáforos do bounded buffer, libertando os recursos do OS.
 *
 * @details
 * Deve ser chamada em main() após todos os pthread_join() terem retornado, garantindo que
 * nenhuma thread ainda acede ao buffer. Liberta os recursos do kernel associados ao mutex
 * e aos semáforos POSIX.
 */
void destroy_bounded_buffer(void) {
    pthread_mutex_destroy(&buffer.mutex); /* destruir mutex e libertar recursos associados */
    sem_destroy(&buffer.empty);           /* destruir semáforo de slots livres */
    sem_destroy(&buffer.full);            /* destruir semáforo de entradas prontas */
}

/**
 * @brief Insere uma LogEntry no buffer circular (operação do lado do produtor).
 *
 * @details
 * Implementa a metade "produtor" do padrão clássico Produtor-Consumidor:
 *
 *  1. esperar(&buffer.empty)   — decrementar slots livres; bloqueia se count == BUFFER_SIZE.
 *  2. pthread_mutex_lock       — adquirir acesso exclusivo para actualizar índices.
 *  3. buffer.queue[buffer.in] = entry; buffer.in = (buffer.in + 1) % BUFFER_SIZE; buffer.count++
 *  4. pthread_mutex_unlock     — libertar acesso exclusivo.
 *  5. assinalar(&buffer.full)  — incrementar entradas disponíveis; acorda um consumidor.
 *
 * @param entry A LogEntry a inserir no buffer circular.
 */
static void inserir_no_buffer(LogEntry entry) {
    /*
     * Passo 1: aguardar um slot livre.
     * Se buffer.count == BUFFER_SIZE, sem_wait bloqueia até que um consumidor
     * liberte uma posição com sem_post(&buffer.empty).
     */
    esperar(&buffer.empty);

    /*
     * Passo 2: acesso exclusivo — apenas um produtor de cada vez pode escrever,
     * para evitar data races no índice `in` e no contador `count`.
     */
    pthread_mutex_lock(&buffer.mutex);

    buffer.queue[buffer.in] = entry;                    /* escrita no slot circular */
    buffer.in = (buffer.in + 1) % BUFFER_SIZE;          /* avançar índice em módulo */
    buffer.count++;                                      /* registar nova entrada no buffer */

    pthread_mutex_unlock(&buffer.mutex); /* libertar mutex para outros produtores */

    /*
     * Passo 5: sinalizar que há uma nova entrada disponível para consumo.
     * Se um consumidor estiver bloqueado em esperar(&buffer.full), acorda agora.
     */
    assinalar(&buffer.full);
}

/**
 * @brief Função de thread do produtor: lê ficheiros de log e alimenta o buffer circular.
 *
 * @details
 * Cada produtor recebe uma fatia [byte_inicio, byte_fim) do espaço virtual resultante da
 * concatenação lógica de todos os ficheiros de log. Algoritmo:
 *
 *  1. Percorrer ficheiros calculando os offsets locais correspondentes à fatia global.
 *  2. Saltar linhas parciais no início (alinhamento ao nível de linha).
 *  3. Ler em blocos de BUF_SIZE bytes, acumular caracteres em linha[] até '\n',
 *     detectar o formato e invocar parse_line(3).
 *  4. Inserir cada LogEntry válida no buffer via inserir_no_buffer().
 *  5. Parar ao atingir o fim da fatia (file_pos >= local_end).
 *
 * Ao terminar, decrementa produtores_ativos (com o mutex adquirido). Se for o último
 * produtor, envia sinais extra em buffer.full para acordar todos os consumidores bloqueados.
 *
 * @param arg Ponteiro genérico (void*) para ProducerArgs com a fatia de bytes e metadados.
 * @return NULL (convenção POSIX pthread).
 */
void *run_producer(void *arg) {
    ProducerArgs *a = (ProducerArgs *)arg; /* cast do argumento genérico para ProducerArgs* */

    /* Extrair limites da fatia global atribuída a este produtor */
    off_t byte_inicio = a->byte_inicio; /* offset inicial (inclusivo) no espaço virtual */
    off_t byte_fim    = a->byte_fim;    /* offset final (exclusivo) no espaço virtual */
    off_t quota       = byte_fim - byte_inicio; /* total de bytes desta fatia */

    /* Publicar a quota no dashboard de monitorização */
    if (a->bytes_total) *(a->bytes_total) = (long)quota;
    if (a->bytes_done)  *(a->bytes_done)  = 0;

    /*
     * global_offset: acumulador do deslocamento em bytes ao longo dos ficheiros visitados.
     * Mapeia byte_inicio/byte_fim para offsets locais dentro de cada ficheiro individual.
     */
    off_t global_offset = 0;
    char  buf[BUF_SIZE];         /* buffer de I/O para read(2) */
    char  linha[LINE_MAX_LOCAL]; /* acumulador de caracteres para a linha corrente */

    for (int i = 0; i < a->total_ficheiros; i++) {
        struct stat st;
        if (stat(a->ficheiros[i], &st) != 0) continue; /* ignorar ficheiro inacessível */
        off_t fsize = st.st_size; /* tamanho físico em bytes do ficheiro actual */

        /* Ficheiro completamente anterior à fatia: avançar offset e continuar */
        if (global_offset + fsize <= byte_inicio) {
            global_offset += fsize;
            continue;
        }
        /* Ficheiro completamente posterior à fatia: não há mais dados para este produtor */
        if (global_offset >= byte_fim) break;

        /*
         * Calcular offsets locais (coordenadas dentro deste ficheiro):
         *   local_start: byte a partir do qual este produtor deve ler.
         *   local_end:   byte até ao qual deve ler (exclusivo).
         */
        off_t local_start = (byte_inicio > global_offset) ? byte_inicio - global_offset : 0;
        off_t local_end   = (byte_fim < global_offset + fsize) ? byte_fim - global_offset : fsize;

        int fd = open(a->ficheiros[i], O_RDONLY); /* abrir descritor em modo só-leitura */
        if (fd < 0) { global_offset += fsize; continue; } /* falha de open(2): próximo ficheiro */

        if (a->verbose) /* modo verboso: reportar intervalo de bytes no stdout */
            posix_writef(STDOUT_FILENO,
                         "[Produtor %d] %s bytes [%lld-%lld]\n",
                         a->worker_index, a->ficheiros[i],
                         (long long)local_start, (long long)local_end);

        /* Reposicionar cursor para o início da fatia (SEEK_SET = absoluto) */
        if (lseek(fd, local_start, SEEK_SET) < 0) {
            perror("lseek");
            close(fd);
            global_offset += fsize;
            continue;
        }

        off_t file_pos = local_start; /* posição de leitura corrente dentro do ficheiro */

        /*
         * Alinhamento de linha: se local_start > 0, o cursor pode estar a meio de uma linha
         * pertencente ao produtor anterior. Avançar byte a byte até '\n' garante que este
         * produtor inicia sempre no começo de uma linha completa — cada linha é processada
         * por exactamente um produtor.
         */
        if (local_start > 0) {
            char c;
            ssize_t r;
            while ((r = read(fd, &c, 1)) == 1) { /* leitura byte a byte para alinhamento */
                file_pos++;
                if (c == '\n') break; /* fronteira de linha encontrada */
            }
            if (r <= 0) { /* EOF ou erro antes de encontrar '\n' */
                close(fd);
                global_offset += fsize;
                continue;
            }
        }

        int len  = 0;                   /* comprimento corrente do acumulador linha[] */
        int done = 0;                   /* flag: 1 quando a fatia deste ficheiro foi esgotada */
        LogFormat fmt = FORMAT_UNKNOWN; /* formato do log, inferido na primeira linha válida */

        /* ── Loop principal: leitura em blocos de BUF_SIZE bytes ── */
        while (!done) {
            ssize_t n = read(fd, buf, BUF_SIZE); /* ler bloco de até BUF_SIZE bytes */
            if (n <= 0) break; /* EOF ou erro de I/O: parar processamento */

            /* Actualizar progresso no slot do dashboard de monitorização */
            off_t pos_na_quota = (global_offset + file_pos) - byte_inicio;
            if (pos_na_quota < 0)     pos_na_quota = 0;
            if (pos_na_quota > quota) pos_na_quota = quota;
            if (a->bytes_done) *(a->bytes_done) = (long)pos_na_quota;

            /* Processar o bloco lido caracter a caracter para reconstruir linhas */
            for (ssize_t b = 0; b < n && !done; b++) {
                char c = buf[b];
                file_pos++; /* avançar posição corrente dentro do ficheiro */

                if (c == '\n') {
                    if (len > 0) {
                        linha[len] = '\0'; /* terminar string C para passagem ao parser */

                        /* Inferir formato (Apache, Nginx, syslog, JSON) na primeira linha */
                        if (fmt == FORMAT_UNKNOWN)
                            fmt = detect_format(linha);

                        LogEntry entry;
                        if (parse_line(linha, fmt, &entry) == 0) /* parsing com sucesso */
                            inserir_no_buffer(entry); /* produzir: depositar no buffer partilhado */

                        len = 0; /* reiniciar acumulador para a linha seguinte */
                    }
                    /* Verificar se a posição de leitura atingiu o limite da fatia */
                    if (file_pos >= local_end) done = 1;
                } else if (c != '\r') {
                    /* Acumular caractere; ignorar '\r' de terminadores CRLF (Windows) */
                    if (len < LINE_MAX_LOCAL - 1)
                        linha[len++] = c;
                }
            }
        }

        /* Tratar última linha sem '\n' (ficheiro sem newline final ou fatia cortada) */
        if (len > 0) {
            linha[len] = '\0';
            if (fmt == FORMAT_UNKNOWN) fmt = detect_format(linha);
            LogEntry entry;
            if (parse_line(linha, fmt, &entry) == 0)
                inserir_no_buffer(entry); /* inserir a última entrada no buffer */
        }

        close(fd);              /* libertar o descritor de ficheiro */
        global_offset += fsize; /* avançar offset acumulado para o próximo ficheiro */
    }

    /* Forçar progresso a 100 % no slot de monitorização ao terminar */
    if (a->bytes_done) *(a->bytes_done) = (long)quota;

    /*
     * Sinalização de fim ao último produtor:
     *
     * Adquirir buffer.mutex para decrementar produtores_ativos de forma atómica
     * relativamente aos consumidores (que também lêem esta variável com o mutex adquirido).
     *
     * Se este for o ÚLTIMO produtor (produtores_ativos chega a zero):
     *   Enviar buffer.count + 1 sinais extra em buffer.full para acordar todos os consumidores
     *   bloqueados em esperar(&buffer.full), permitindo-lhes detectar a condição de paragem
     *   (count == 0 && produtores_ativos == 0) e sair do loop de consumo.
     */
    pthread_mutex_lock(&buffer.mutex);
    produtores_ativos--; /* registar que este produtor terminou */
    int consumidores_a_acordar = (produtores_ativos == 0) ? buffer.count + 1 : 0;
    pthread_mutex_unlock(&buffer.mutex);

    /* Acordar os consumidores bloqueados em sem_wait(&buffer.full) */
    for (int k = 0; k < consumidores_a_acordar; k++)
        assinalar(&buffer.full); /* sem_post: acordar um consumidor por invocação */

    return NULL; /* convenção POSIX: thread termina com retorno NULL */
}

/**
 * @brief Função de thread do consumidor: retira entradas do buffer e actualiza métricas.
 *
 * @details
 * Implementa a metade "consumidor" do padrão Produtor-Consumidor:
 *
 *  1. esperar(&buffer.full)   — bloqueia enquanto o buffer estiver vazio.
 *  2. pthread_mutex_lock      — adquirir acesso exclusivo ao buffer.
 *  3. Verificar condição de paragem: buffer.count == 0 && produtores_ativos == 0.
 *     Se verdadeira: libertar mutex, propagar sinal (cascade wake-up) e sair.
 *  4. Retirar buffer.queue[buffer.out]; buffer.out = (buffer.out + 1) % BUFFER_SIZE; buffer.count--.
 *  5. pthread_mutex_unlock — libertar acesso exclusivo.
 *  6. assinalar(&buffer.empty) — sinalizar slot livre para produtores.
 *  7. pthread_mutex_lock(metrics_mutex); update_metrics(); pthread_mutex_unlock(metrics_mutex).
 *
 * **Condição de paragem:** buffer.count == 0 && produtores_ativos == 0.
 * Ao detectar esta condição, o consumidor propaga o sinal em buffer.full (cascade wake-up)
 * para acordar os restantes consumidores ainda bloqueados, e termina com break.
 *
 * @param arg Ponteiro genérico (void*) para ConsumerArgs com global_metrics e metrics_mutex.
 * @return NULL (convenção POSIX pthread).
 */
void *run_consumer(void *arg) {
    ConsumerArgs *a = (ConsumerArgs *)arg; /* cast do argumento genérico para ConsumerArgs* */

    while (1) { /* loop de consumo; a saída é controlada pela condição de paragem */
        /*
         * Passo 1: aguardar uma entrada disponível no buffer.
         * sem_wait decrementa buffer.full; bloqueia se for zero (buffer vazio).
         * Pode ser acordado de dois modos:
         *   a) Por um produtor após inserir uma entrada (caso normal de produção).
         *   b) Pelo último produtor que envia sinais extra quando produtores_ativos == 0.
         */
        esperar(&buffer.full);

        /* Passo 2: acesso exclusivo para verificar estado e retirar entrada */
        pthread_mutex_lock(&buffer.mutex);

        /*
         * Condição de paragem: buffer vazio e nenhum produtor activo.
         * Não chegará mais nenhuma entrada — é seguro terminar.
         * Propagar o sinal em buffer.full (cascade wake-up) para acordar os outros
         * consumidores ainda bloqueados em esperar(&buffer.full).
         */
        if (buffer.count == 0 && produtores_ativos == 0) {
            pthread_mutex_unlock(&buffer.mutex);
            assinalar(&buffer.full); /* propagar sinal para o próximo consumidor bloqueado */
            break;                   /* sair do loop de consumo */
        }

        /*
         * Protecção contra spurious wakeup ou sinal extra do último produtor quando o buffer
         * já foi esvaziado por outro consumidor antes de este adquirir o mutex.
         */
        if (buffer.count == 0) {
            pthread_mutex_unlock(&buffer.mutex);
            continue; /* voltar a esperar sem consumir */
        }

        /* Passo 3: retirar a entrada mais antiga do buffer circular */
        LogEntry entry = buffer.queue[buffer.out];

        buffer.out = (buffer.out + 1) % BUFFER_SIZE; /* avançar índice de leitura em módulo */
        buffer.count--;                               /* decrementar contador de entradas */

        /* Passo 4: libertar mutex e sinalizar slot livre para produtores */
        pthread_mutex_unlock(&buffer.mutex);

        /*
         * Sinalizar ao produtor que há um slot livre no buffer.
         * Se um produtor estiver bloqueado em esperar(&buffer.empty), acorda agora.
         */
        assinalar(&buffer.empty);

        /*
         * Passo 5: actualizar métricas globais com exclusão mútua.
         * metrics_mutex é independente de buffer.mutex: protege apenas a estrutura Metrics,
         * permitindo que a inserção/remoção do buffer continue em paralelo com a actualização
         * das métricas — aumentando o grau de concorrência efectiva.
         */
        pthread_mutex_lock(a->metrics_mutex);
        update_metrics(a->global_metrics, &entry); /* acumular contadores da entrada consumida */
        pthread_mutex_unlock(a->metrics_mutex);
    }

    return NULL; /* convenção POSIX: thread termina com retorno NULL */
}
