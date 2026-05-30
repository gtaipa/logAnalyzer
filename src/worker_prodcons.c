/* worker_prodcons.c
 *
 * Implementa o padrão Produtor-Consumidor para análise de logs.
 *
 * Arquitectura:
 *   - N threads PRODUTORAS: cada uma abre um subconjunto de ficheiros,
 *     parseia linha a linha, e insere LogEntry no buffer circular.
 *   - M threads CONSUMIDORAS: retiram LogEntry do buffer e acumulam
 *     métricas numa estrutura global protegida por mutex.
 *   - O buffer circular é protegido por um mutex + dois semáforos
 *     (empty conta slots livres, full conta slots ocupados).
 */

/* ALTERAÇÃO: Removidos includes de ipc.h e worker.h — o produtor-consumidor
 * não usa pipes nem sockets; a comunicação é feita em memória partilhada. */
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

#define BUF_SIZE       4096
#define LINE_MAX_LOCAL  512

/* =========================================================
 * Variáveis globais partilhadas
 * ========================================================= */

/* ALTERAÇÃO: buffer, produtores_ativos e metrics_mutex passam a ser
 * definidos aqui (uma só vez). O header declara-os como extern para
 * que main_prodcons.c os possa usar sem redefinir. */
LogQueue        buffer;
int             produtores_ativos = 0;
pthread_mutex_t metrics_mutex     = PTHREAD_MUTEX_INITIALIZER;

/* =========================================================
 * Wrappers de semáforo com tratamento de interrupções EINTR
 * ========================================================= */

/* Aguarda um semáforo com retry em caso de sinal */
static void esperar(sem_t *s) {
    while (sem_wait(s) == -1 && errno == EINTR)
        ;   /* repetir se interrompido por sinal */
}

/* Assinala um semáforo (incrementa contador) */
static void assinalar(sem_t *s) {
    sem_post(s);
}

/* =========================================================
 * init_bounded_buffer / destroy_bounded_buffer
 * Inicializa e limpa o buffer circular com sincronização
 * ========================================================= */
/* Inicializa o buffer circular e seus semáforos */
void init_bounded_buffer(void) {
    buffer.in    = 0;
    buffer.out   = 0;
    buffer.count = 0;
    pthread_mutex_init(&buffer.mutex, NULL);
    /* empty começa em BUFFER_SIZE: há BUFFER_SIZE slots livres */
    sem_init(&buffer.empty, 0, BUFFER_SIZE);
    /* full começa em 0: não há nenhum slot ocupado */
    sem_init(&buffer.full,  0, 0);
}

/* Liberta recursos do buffer circular */
void destroy_bounded_buffer(void) {
    pthread_mutex_destroy(&buffer.mutex);
    sem_destroy(&buffer.empty);
    sem_destroy(&buffer.full);
}

/* =========================================================
 * inserir_no_buffer (chamado pelo produtor)
 *
 * Protocolo clássico Produtor-Consumidor:
 *   1. esperar(empty)  → garante que há pelo menos 1 slot livre
 *   2. lock(mutex)     → acesso exclusivo ao buffer
 *   3. escrever
 *   4. unlock(mutex)
 *   5. assinalar(full) → avisa consumidores que há 1 slot ocupado
 * ========================================================= */
/* Insere uma entry no buffer circular (chamado pelo produtor) */
static void inserir_no_buffer(LogEntry entry) {
    esperar(&buffer.empty);
    pthread_mutex_lock(&buffer.mutex);

    buffer.queue[buffer.in] = entry;
    buffer.in = (buffer.in + 1) % BUFFER_SIZE;
    buffer.count++;

    pthread_mutex_unlock(&buffer.mutex);
    assinalar(&buffer.full);
}

/* =========================================================
 * run_producer
 *
 * ALTERAÇÃO: Esta função é completamente nova.
 * O produtor antigo (em main_prodcons.c) abria "log.txt" hardcoded,
 * lia em blocos com read() (não linha a linha), e não fazia parse
 * nenhum — apenas copiava bytes para o campo .message.
 *
 * Este produtor:
 *   - Recebe ProducerArgs com o subconjunto de ficheiros que lhe cabe.
 *   - Lê cada ficheiro linha a linha (igual ao worker_threads.c).
 *   - Para cada linha, deteta o formato e chama parse_line().
 *   - Se parse_line() tiver sucesso, insere a LogEntry no buffer.
 *   - Quando termina TODOS os seus ficheiros, decrementa produtores_ativos
 *     (com mutex) e acorda todos os consumidores para que possam verificar
 *     se devem sair.
 * ========================================================= */
/* Thread produtor: lê logs de ficheiros e insere no buffer */
void *run_producer(void *arg) {
    ProducerArgs *a = (ProducerArgs *)arg;

    /* Contar total de linhas para o dashboard */
    long total = 0;
    for (int i = a->inicio; i < a->fim; i++) {
        int fd2 = open(a->ficheiros[i], O_RDONLY);
        if (fd2 < 0) continue;
        char tmp[4096];
        ssize_t n;
        while ((n = read(fd2, tmp, 4096)) > 0)
            for (ssize_t j = 0; j < n; j++)
                if (tmp[j] == '\n') total++;
        close(fd2);
    }
    if (a->lines_total) *(a->lines_total) = total;
    if (a->lines_done)  *(a->lines_done)  = 0;

    for (int i = a->inicio; i < a->fim; i++) {
        int fd = open(a->ficheiros[i], O_RDONLY);
        if (fd < 0) {
            perror("producer: open");
            continue;   /* ficheiro inacessível — continuar com os outros */
        }

        if (a->verbose)
            posix_writef(STDOUT_FILENO,
                         "[Produtor %d] A ler: %s\n",
                         a->worker_index, a->ficheiros[i]);

        char    buf[BUF_SIZE];
        char    linha[LINE_MAX_LOCAL];
        int     len = 0;
        LogFormat fmt = FORMAT_UNKNOWN;
        ssize_t bytes;

        /* Ler o ficheiro em blocos mas processar linha a linha */
        while ((bytes = read(fd, buf, BUF_SIZE)) > 0) {
            for (ssize_t b = 0; b < bytes; b++) {
                char c = buf[b];

                if (c == '\n' || c == '\r') {
                    if (len == 0) continue;   /* linha vazia */
                    linha[len] = '\0';

                    /* Detectar formato na primeira linha não vazia */
                    if (fmt == FORMAT_UNKNOWN)
                        fmt = detect_format(linha);

                    /* Parsear e inserir no buffer apenas se válido */
                    LogEntry entry;
                    if (parse_line(linha, fmt, &entry) == 0)
                        inserir_no_buffer(entry);

                    if (a->lines_done) (*a->lines_done)++;

                    len = 0;
                } else {
                    if (len < LINE_MAX_LOCAL - 1)
                        linha[len++] = c;
                }
            }
        }

        /* Tratar última linha sem '\n' no final do ficheiro */
        if (len > 0) {
            linha[len] = '\0';
            if (fmt == FORMAT_UNKNOWN) fmt = detect_format(linha);
            LogEntry entry;
            if (parse_line(linha, fmt, &entry) == 0)
                inserir_no_buffer(entry);
            if (a->lines_done) (*a->lines_done)++;
        }

        close(fd);
    }

    /* ALTERAÇÃO: Quando o produtor termina, decrementa o contador global
     * dentro do mutex do buffer para evitar race condition com os consumidores.
     * Depois acorda TODOS os consumidores com sem_post repetido — cada
     * consumidor vai verificar a condição de saída e os que não devem
     * sair vão fazer sem_wait novamente.
     *
     * Porquê acordar todos?
     * Se tivermos M consumidores e o último produtor termina, pode haver
     * consumidores bloqueados em esperar(full). Sem este "aviso geral",
     * ficariam bloqueados para sempre mesmo com produtores_ativos == 0. */
    pthread_mutex_lock(&buffer.mutex);
    produtores_ativos--;
    int consumidores_a_acordar = (produtores_ativos == 0) ? buffer.count + 1 : 0;
    pthread_mutex_unlock(&buffer.mutex);

    /* Acordar consumidores adormecidos se este for o último produtor */
    for (int k = 0; k < consumidores_a_acordar; k++)
        assinalar(&buffer.full);

    return NULL;
}

/* =========================================================
 * run_consumer
 *
 * ALTERAÇÃO: O consumidor antigo (consumer_routine em main_prodcons.c)
 * escrevia para "results.txt" hardcoded e usava a pílula venenosa como
 * sinal de fim — mas com múltiplos consumidores apenas um recebia a pílula.
 *
 * Este consumidor:
 *   - Usa produtores_ativos + buffer.count para decidir quando parar:
 *     sai quando não há mais produtores E o buffer está vazio.
 *   - Acumula métricas na estrutura global usando o metrics_mutex.
 * ========================================================= */
void *run_consumer(void *arg) {
    ConsumerArgs *a = (ConsumerArgs *)arg;

    while (1) {
        /* Esperar que haja pelo menos 1 entrada no buffer */
        esperar(&buffer.full);
        pthread_mutex_lock(&buffer.mutex);

        /* ALTERAÇÃO: Condição de saída correcta para múltiplos consumidores.
         * Verificamos DENTRO do mutex para evitar race condition:
         * se buffer.count == 0 e produtores_ativos == 0, não há mais
         * trabalho — libertamos o mutex e saímos. */
        if (buffer.count == 0 && produtores_ativos == 0) {
            pthread_mutex_unlock(&buffer.mutex);
            /* Acordar outro consumidor que possa estar bloqueado */
            assinalar(&buffer.full);
            break;
        }

        /* Se count == 0 mas ainda há produtores, foi um "acorde falso"
         * (o produtor acordou-nos mas não inseriu nada ainda) — volta a esperar. */
        if (buffer.count == 0) {
            pthread_mutex_unlock(&buffer.mutex);
            continue;
        }

        /* Retirar a entrada do buffer */
        LogEntry entry = buffer.queue[buffer.out];
        buffer.out = (buffer.out + 1) % BUFFER_SIZE;
        buffer.count--;

        pthread_mutex_unlock(&buffer.mutex);
        assinalar(&buffer.empty);   /* liberta 1 slot para os produtores */

        /* ALTERAÇÃO: Acumular métricas na estrutura global.
         * O antigo consumidor escrevia para ficheiro — aqui usamos
         * update_metrics() igual ao worker_threads.c, protegido por mutex. */
        pthread_mutex_lock(a->metrics_mutex);
        update_metrics(a->global_metrics, &entry);
        pthread_mutex_unlock(a->metrics_mutex);
    }

    return NULL;
}