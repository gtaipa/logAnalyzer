/**
 * @file worker_prodcons.c
 * @brief Implementação do padrão Produtor-Consumidor com bounded buffer.
 *
 * @details
 * Este ficheiro contém toda a mecânica de sincronização entre produtores
 * e consumidores:
 *
 *  - **Buffer circular (bounded buffer):** estrutura `LogQueue` com capacidade
 *    fixa `BUFFER_SIZE = 30`. Os índices `in` (próxima posição de escrita) e
 *    `out` (próxima posição de leitura) avançam em módulo `BUFFER_SIZE`,
 *    reutilizando ciclicamente as posições do array. O campo `count` mantém
 *    o número de entradas actualmente no buffer.
 *
 *  - **Semáforos POSIX:**
 *      - `buffer.empty`: inicializado a `BUFFER_SIZE`; decrementado pelo
 *        produtor antes de escrever (bloqueia quando o buffer está cheio).
 *      - `buffer.full`: inicializado a 0; decrementado pelo consumidor antes
 *        de ler (bloqueia quando o buffer está vazio).
 *
 *  - **Mutex `buffer.mutex`:** garante exclusão mútua na secção crítica onde
 *    os índices `in`/`out`/`count` são actualizados — evita corridas de dados
 *    entre múltiplos produtores e consumidores.
 *
 *  - **`produtores_ativos`:** contador decrementado pelo último produtor a
 *    terminar; quando chega a zero, esse produtor envia `buffer.count + 1`
 *    sinais em `buffer.full` para acordar todos os consumidores que possam
 *    estar bloqueados em `sem_wait`, permitindo-lhes detectar o fim e sair.
 *
 *  - **`metrics_mutex`:** mutex independente que protege a actualização da
 *    estrutura `Metrics` global partilhada por todos os consumidores.
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

/** @brief Tamanho do buffer de leitura I/O (em bytes) usado pelo produtor. */
#define BUF_SIZE       4096

/** @brief Comprimento máximo de uma linha de log suportada pelo produtor. */
#define LINE_MAX_LOCAL  512

/**
 * @brief Buffer circular partilhado entre todos os produtores e consumidores.
 *
 * @details
 * Contém o array de `LogEntry`, os índices `in`/`out`/`count`, o mutex de
 * acesso e os dois semáforos de controlo de fluxo (`empty` e `full`).
 * Definido como variável global para ser visível em main_prodcons.c via
 * `extern` declarado em worker_prodcons.h.
 */
LogQueue        buffer;

/**
 * @brief Número de threads produtoras ainda em execução.
 *
 * @details
 * Inicializado em main() com o número total de produtores lançados.
 * Cada produtor decrementa esta variável (com o mutex do buffer adquirido)
 * imediatamente antes de terminar. Quando chega a zero, o último produtor
 * sabe que não chegará mais nenhuma entrada ao buffer e acorda os consumidores.
 */
int             produtores_ativos = 0;

/**
 * @brief Mutex que protege a actualização da estrutura de métricas global.
 *
 * @details
 * Todos os consumidores partilham a mesma `Metrics`; este mutex garante que
 * apenas um consumidor de cada vez executa `update_metrics`, evitando
 * condições de corrida nos contadores acumulados.
 * Inicializado estaticamente com `PTHREAD_MUTEX_INITIALIZER`.
 */
pthread_mutex_t metrics_mutex     = PTHREAD_MUTEX_INITIALIZER;

/**
 * @brief Wrapper seguro em torno de `sem_wait` que reomeça se interrompido.
 *
 * @details
 * `sem_wait` pode retornar -1 com `errno == EINTR` quando o processo recebe
 * um sinal durante a espera. O loop garante que a espera recomeça nesse caso,
 * implementando uma espera bloqueante verdadeiramente fiável.
 *
 * @param s  Apontador para o semáforo POSIX a decrementar (bloquear se zero).
 */
static void esperar(sem_t *s) {
    /* Repetir se a chamada foi interrompida por um sinal (EINTR) */
    while (sem_wait(s) == -1 && errno == EINTR)
        ;
}

/**
 * @brief Wrapper em torno de `sem_post` para incrementar (sinalizar) um semáforo.
 *
 * @details
 * Incrementa o semáforo, acordando uma thread que esteja bloqueada em
 * `esperar()` (i.e., em `sem_wait`). Usado pelo produtor para sinalizar
 * `buffer.full` após inserir uma entrada, e pelo consumidor para sinalizar
 * `buffer.empty` após retirar uma entrada.
 *
 * @param s  Apontador para o semáforo POSIX a incrementar (sinalizar).
 */
static void assinalar(sem_t *s) {
    sem_post(s);  /* Incrementa o semáforo e acorda um waiter, se existir */
}

/**
 * @brief Inicializa o bounded buffer e os seus primitivos de sincronização.
 *
 * @details
 * Deve ser chamada uma única vez em main() antes de lançar qualquer thread.
 * Configura:
 *  - `buffer.in = buffer.out = buffer.count = 0` — buffer vazio.
 *  - `pthread_mutex_init(&buffer.mutex)` — mutex para acesso exclusivo ao buffer.
 *  - `sem_init(&buffer.empty, 0, BUFFER_SIZE)` — há `BUFFER_SIZE` espaços livres.
 *  - `sem_init(&buffer.full,  0, 0)` — zero entradas disponíveis para consumir.
 */
void init_bounded_buffer(void) {
    buffer.in    = 0;   /* Índice de escrita: próxima posição livre no array */
    buffer.out   = 0;   /* Índice de leitura: próxima posição a consumir */
    buffer.count = 0;   /* Entradas actualmente no buffer */

    /* Mutex que garante exclusão mútua no acesso aos índices do buffer */
    pthread_mutex_init(&buffer.mutex, NULL);

    /*
     * Semáforo "empty": conta o número de posições livres no buffer circular.
     * Começa em BUFFER_SIZE (buffer completamente vazio).
     * O produtor faz sem_wait(empty) antes de escrever — bloqueia se cheio.
     * O consumidor faz sem_post(empty) depois de ler — liberta uma posição.
     */
    sem_init(&buffer.empty, 0, BUFFER_SIZE);

    /*
     * Semáforo "full": conta o número de entradas disponíveis para consumir.
     * Começa em 0 (buffer vazio, nada para consumir).
     * O consumidor faz sem_wait(full) antes de ler — bloqueia se vazio.
     * O produtor faz sem_post(full) depois de escrever — sinaliza nova entrada.
     */
    sem_init(&buffer.full,  0, 0);
}

/**
 * @brief Destroi o mutex e os semáforos do bounded buffer.
 *
 * @details
 * Deve ser chamada em main() depois de todos os threads terem terminado
 * (após os `pthread_join`), para libertar os recursos do sistema operativo
 * associados ao mutex e aos semáforos POSIX.
 */
void destroy_bounded_buffer(void) {
    pthread_mutex_destroy(&buffer.mutex);  /* Libertar recursos do mutex */
    sem_destroy(&buffer.empty);            /* Libertar semáforo de espaços livres */
    sem_destroy(&buffer.full);             /* Libertar semáforo de entradas prontas */
}

/**
 * @brief Insere uma `LogEntry` no buffer circular (secção do produtor).
 *
 * @details
 * Implementa a metade "produtor" do padrão clássico Produtor-Consumidor:
 *
 *  1. `esperar(&buffer.empty)` — decrementa o semáforo de espaços livres;
 *     bloqueia se o buffer estiver cheio (count == BUFFER_SIZE).
 *  2. `pthread_mutex_lock(&buffer.mutex)` — adquire acesso exclusivo ao buffer
 *     para que dois produtores não escrevam na mesma posição.
 *  3. Escreve a entrada em `buffer.queue[buffer.in]` e avança `in` em módulo
 *     `BUFFER_SIZE` (comportamento circular).
 *  4. Incrementa `buffer.count`.
 *  5. `pthread_mutex_unlock` — liberta o acesso exclusivo.
 *  6. `assinalar(&buffer.full)` — incrementa o semáforo de entradas disponíveis,
 *     acordando um consumidor bloqueado se existir.
 *
 * @param entry  A entrada de log a inserir no buffer.
 */
static void inserir_no_buffer(LogEntry entry) {
    /*
     * Passo 1 — Esperar por um espaço livre no buffer.
     * Se buffer.count == BUFFER_SIZE, sem_wait bloqueia aqui até que um
     * consumidor liberte uma posição com sem_post(&buffer.empty).
     */
    esperar(&buffer.empty);

    /*
     * Passo 2 — Acesso exclusivo: apenas um produtor de cada vez pode
     * escrever, para evitar corridas de dados no índice `in` e em `count`.
     */
    pthread_mutex_lock(&buffer.mutex);

    /* Passo 3 — Escrita no buffer circular na posição `in` */
    buffer.queue[buffer.in] = entry;

    /* Avançar o índice de escrita em módulo BUFFER_SIZE (buffer circular) */
    buffer.in = (buffer.in + 1) % BUFFER_SIZE;

    /* Passo 4 — Registar que há mais uma entrada no buffer */
    buffer.count++;

    /* Passo 5 — Libertar o mutex para que outros produtores possam escrever */
    pthread_mutex_unlock(&buffer.mutex);

    /*
     * Passo 6 — Sinalizar que há uma nova entrada disponível para consumir.
     * Se um consumidor estiver bloqueado em esperar(&buffer.full), acorda agora.
     */
    assinalar(&buffer.full);
}

/**
 * @brief Função de thread do produtor: lê ficheiros de log e alimenta o buffer.
 *
 * @details
 * Cada produtor recebe uma fatia [byte_inicio, byte_fim[ do espaço virtual
 * resultante da concatenação lógica de todos os ficheiros de log. O algoritmo:
 *
 *  1. Percorre os ficheiros em ordem, calculando os offsets locais da fatia.
 *  2. Salta linhas parciais no início da fatia (para não processar linhas que
 *     pertencem ao produtor anterior — sincronização ao nível de linha).
 *  3. Lê o ficheiro em blocos de `BUF_SIZE` bytes, acumula caracteres em
 *     `linha[]` até encontrar `'\n'`, detecta o formato e chama `parse_line`.
 *  4. Cada linha válida é inserida no buffer partilhado via `inserir_no_buffer`.
 *  5. Para ao atingir o fim da sua fatia (`file_pos >= local_end`).
 *
 * Ao terminar, decrementa `produtores_ativos` com o mutex adquirido.
 * Se for o último produtor (`produtores_ativos == 0`), envia sinais extra em
 * `buffer.full` para acordar todos os consumidores bloqueados.
 *
 * @param arg  Apontador para `ProducerArgs` com a fatia de bytes e metadados.
 * @return     NULL (convenção pthread).
 */
void *run_producer(void *arg) {
    ProducerArgs *a = (ProducerArgs *)arg;

    /* Limites da fatia de bytes atribuída a este produtor */
    off_t byte_inicio = a->byte_inicio;
    off_t byte_fim    = a->byte_fim;
    off_t quota       = byte_fim - byte_inicio;  /* Total de bytes a processar */

    /* Registar o total de bytes para o dashboard de progresso */
    if (a->bytes_total) *(a->bytes_total) = (long)quota;
    if (a->bytes_done)  *(a->bytes_done)  = 0;

    /*
     * global_offset: posição acumulada no espaço virtual de bytes concatenados.
     * À medida que iteramos os ficheiros, global_offset avança com o tamanho
     * de cada um, permitindo mapear byte_inicio/byte_fim para offsets locais.
     */
    off_t global_offset = 0;
    char  buf[BUF_SIZE];        /* Buffer de leitura I/O para read() */
    char  linha[LINE_MAX_LOCAL]; /* Buffer para acumular uma linha de log */

    for (int i = 0; i < a->total_ficheiros; i++) {
        struct stat st;
        if (stat(a->ficheiros[i], &st) != 0) continue;  /* Ignorar se inacessível */
        off_t fsize = st.st_size;

        /* Ficheiro completamente antes da nossa fatia → ignorar */
        if (global_offset + fsize <= byte_inicio) {
            global_offset += fsize;
            continue;
        }
        /* Ficheiro completamente depois da nossa fatia → parar */
        if (global_offset >= byte_fim) break;

        /*
         * Calcular os offsets locais (dentro deste ficheiro) correspondentes
         * ao início e fim da nossa fatia global.
         */
        off_t local_start = (byte_inicio > global_offset) ? byte_inicio - global_offset : 0;
        off_t local_end   = (byte_fim < global_offset + fsize) ? byte_fim - global_offset : fsize;

        /* Abrir o ficheiro em modo só-leitura com a API POSIX */
        int fd = open(a->ficheiros[i], O_RDONLY);
        if (fd < 0) { global_offset += fsize; continue; }

        /* Modo verbose: imprimir o intervalo de bytes a processar neste ficheiro */
        if (a->verbose)
            posix_writef(STDOUT_FILENO,
                         "[Produtor %d] %s bytes [%lld-%lld]\n",
                         a->worker_index, a->ficheiros[i],
                         (long long)local_start, (long long)local_end);

        /* Posicionar o descritor de ficheiro no início da nossa fatia */
        if (lseek(fd, local_start, SEEK_SET) < 0) {
            perror("lseek");
            close(fd);
            global_offset += fsize;
            continue;
        }

        off_t file_pos = local_start;  /* Posição actual de leitura no ficheiro */

        /*
         * Se não estamos no início do ficheiro, avançar até ao '\n'
         * para não processar uma linha que pertence ao produtor anterior.
         * Isto garante que cada linha é processada por exactamente um produtor.
         */
        if (local_start > 0) {
            char c;
            ssize_t r;
            while ((r = read(fd, &c, 1)) == 1) {
                file_pos++;
                if (c == '\n') break;  /* Encontrou o fim da linha parcial */
            }
            if (r <= 0) {
                /* EOF ou erro antes de encontrar '\n' — nada a processar */
                close(fd);
                global_offset += fsize;
                continue;
            }
        }

        int len  = 0;              /* Número de caracteres acumulados em `linha` */
        int done = 0;              /* Flag: 1 quando a fatia foi completamente lida */
        LogFormat fmt = FORMAT_UNKNOWN;  /* Formato do ficheiro (detectado na 1ª linha) */

        /* Loop principal de leitura: lê o ficheiro em blocos de BUF_SIZE bytes */
        while (!done) {
            ssize_t n = read(fd, buf, BUF_SIZE);
            if (n <= 0) break;  /* EOF ou erro de I/O */

            /* Actualizar o progresso em bytes para o dashboard do monitor */
            off_t pos_na_quota = (global_offset + file_pos) - byte_inicio;
            if (pos_na_quota < 0)     pos_na_quota = 0;
            if (pos_na_quota > quota) pos_na_quota = quota;
            if (a->bytes_done) *(a->bytes_done) = (long)pos_na_quota;

            /* Processar byte a byte o bloco lido */
            for (ssize_t b = 0; b < n && !done; b++) {
                char c = buf[b];
                file_pos++;

                if (c == '\n') {
                    /* Fim de linha: processar a linha acumulada em `linha[]` */
                    if (len > 0) {
                        linha[len] = '\0';  /* Terminar a string C */

                        /* Detectar o formato do ficheiro (Apache, JSON, etc.) na primeira linha válida */
                        if (fmt == FORMAT_UNKNOWN)
                            fmt = detect_format(linha);

                        /* Fazer parse da linha para uma LogEntry estruturada */
                        LogEntry entry;
                        if (parse_line(linha, fmt, &entry) == 0)
                            inserir_no_buffer(entry);  /* Produzir: inserir no buffer partilhado */

                        len = 0;  /* Reiniciar o acumulador de linha */
                    }
                    /* Verificar se já atingimos o fim da nossa fatia de bytes */
                    if (file_pos >= local_end) done = 1;
                } else if (c != '\r') {
                    /* Acumular o caractere na linha (ignorar '\r' de fins de linha Windows) */
                    if (len < LINE_MAX_LOCAL - 1)
                        linha[len++] = c;
                }
            }
        }

        /* Última linha sem '\n' no final do ficheiro — processar igualmente */
        if (len > 0) {
            linha[len] = '\0';
            if (fmt == FORMAT_UNKNOWN) fmt = detect_format(linha);
            LogEntry entry;
            if (parse_line(linha, fmt, &entry) == 0)
                inserir_no_buffer(entry);  /* Inserir a última entrada no buffer */
        }

        close(fd);             /* Fechar o descritor de ficheiro POSIX */
        global_offset += fsize; /* Avançar o offset virtual para o próximo ficheiro */
    }

    /* Forçar progresso a 100% no dashboard ao terminar */
    if (a->bytes_done) *(a->bytes_done) = (long)quota;

    /*
     * Sinalização de fim ao último produtor:
     *
     * Adquirir o mutex do buffer para decrementar `produtores_ativos` de forma
     * atómica relativamente aos consumidores (que também lêem esta variável
     * com o mutex adquirido para decidir se devem terminar).
     *
     * Se este for o ÚLTIMO produtor (produtores_ativos chega a 0):
     *   - Calcular quantos consumidores podem estar bloqueados em sem_wait(full).
     *     No pior caso são todos os NUM_CONSUMERS consumidores, mais os que
     *     já retiraram mas voltaram a bloquear. Enviar buffer.count + 1 sinais
     *     extra garante que todos os consumidores acordam e verificam a condição
     *     de paragem (count == 0 && produtores_ativos == 0).
     */
    pthread_mutex_lock(&buffer.mutex);
    produtores_ativos--;  /* Registar que este produtor terminou */
    int consumidores_a_acordar = (produtores_ativos == 0) ? buffer.count + 1 : 0;
    pthread_mutex_unlock(&buffer.mutex);

    /* Acordar consumidores bloqueados em sem_wait(&buffer.full) */
    for (int k = 0; k < consumidores_a_acordar; k++)
        assinalar(&buffer.full);  /* sem_post: acordar um consumidor por chamada */

    return NULL;
}

/**
 * @brief Função de thread do consumidor: retira entradas do buffer e actualiza métricas.
 *
 * @details
 * Implementa a metade "consumidor" do padrão Produtor-Consumidor:
 *
 *  1. `esperar(&buffer.full)` — bloqueia enquanto o buffer estiver vazio.
 *  2. Adquire o mutex e verifica as condições de paragem e de buffer vazio.
 *  3. Retira a entrada em `buffer.queue[buffer.out]`, avança `out` em módulo
 *     `BUFFER_SIZE` e decrementa `count`.
 *  4. Liberta o mutex e sinaliza `buffer.empty` (espaço livre para produtores).
 *  5. Actualiza as métricas globais com o mutex `metrics_mutex`.
 *
 * **Condição de paragem:** `buffer.count == 0 && produtores_ativos == 0`.
 * Quando um consumidor detecta esta condição, reenvia o sinal em `buffer.full`
 * (passa o testemunho) para acordar os outros consumidores ainda bloqueados,
 * e termina com `break`.
 *
 * @param arg  Apontador para `ConsumerArgs` com `global_metrics` e `metrics_mutex`.
 * @return     NULL (convenção pthread).
 */
void *run_consumer(void *arg) {
    ConsumerArgs *a = (ConsumerArgs *)arg;

    while (1) {
        /*
         * Passo 1 — Aguardar uma entrada disponível no buffer.
         * sem_wait decrementa buffer.full; bloqueia se for zero (buffer vazio).
         * Pode ser acordado de dois modos:
         *   a) Por um produtor após inserir uma entrada (caso normal).
         *   b) Pelo último produtor que envia sinais extra para acordar todos
         *      os consumidores quando produtores_ativos chega a zero.
         */
        esperar(&buffer.full);

        /* Passo 2 — Acesso exclusivo para verificar estado e retirar entrada */
        pthread_mutex_lock(&buffer.mutex);

        /*
         * Condição de paragem: buffer vazio E nenhum produtor activo.
         * Não chegará mais nenhuma entrada — é seguro terminar.
         * Reenviar o sinal em buffer.full para passar o testemunho aos
         * outros consumidores ainda bloqueados (padrão "cascade wake-up").
         */
        if (buffer.count == 0 && produtores_ativos == 0) {
            pthread_mutex_unlock(&buffer.mutex);
            assinalar(&buffer.full);  /* Acordar o próximo consumidor bloqueado */
            break;                    /* Terminar o loop de consumo */
        }

        /*
         * Protecção contra acordar espúrio (spurious wakeup) ou sinal extra
         * do produtor quando o buffer já foi esvaziado por outro consumidor.
         */
        if (buffer.count == 0) {
            pthread_mutex_unlock(&buffer.mutex);
            continue;  /* Voltar a esperar sem consumir */
        }

        /* Passo 3 — Retirar a entrada mais antiga do buffer circular */
        LogEntry entry = buffer.queue[buffer.out];

        /* Avançar o índice de leitura em módulo BUFFER_SIZE (buffer circular) */
        buffer.out = (buffer.out + 1) % BUFFER_SIZE;

        /* Registar que há menos uma entrada no buffer */
        buffer.count--;

        /* Passo 4 — Libertar o mutex e sinalizar que há uma posição livre */
        pthread_mutex_unlock(&buffer.mutex);

        /*
         * Sinalizar ao produtor que há um espaço livre no buffer.
         * Se um produtor estiver bloqueado em esperar(&buffer.empty), acorda agora.
         */
        assinalar(&buffer.empty);

        /*
         * Passo 5 — Actualizar as métricas globais com exclusão mútua.
         * metrics_mutex é diferente de buffer.mutex: protege apenas a estrutura
         * Metrics, permitindo que a inserção/remoção do buffer continue em
         * paralelo com a actualização das métricas.
         */
        pthread_mutex_lock(a->metrics_mutex);
        update_metrics(a->global_metrics, &entry);  /* Acumular contadores da entrada */
        pthread_mutex_unlock(a->metrics_mutex);
    }

    return NULL;
}