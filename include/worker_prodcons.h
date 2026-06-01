/**
 * @file worker_prodcons.h
 * @brief Buffer limitado (bounded buffer) e threads produtor/consumidor para
 *        processamento paralelo de logs com o padrão produtor-consumidor.
 *
 * Define a fila partilhada (LogQueue), os argumentos das threads produtoras
 * (ProducerArgs) e consumidoras (ConsumerArgs), as variáveis globais de
 * sincronização e os protótipos das funções de inicialização, destruição e
 * execução de cada tipo de thread.
 *
 * O buffer é limitado a BUFFER_SIZE entradas e a exclusão mútua é garantida
 * por um mutex POSIX; a coordenação de cheio/vazio usa dois semáforos POSIX
 * (empty e full).
 */

#ifndef WORKER_PRODCONS_H
#define WORKER_PRODCONS_H

#include <pthread.h>
#include <semaphore.h>
#include <sys/types.h>
#include "parser.h"

/** @brief Capacidade máxima do buffer circular partilhado entre produtores e consumidores. */
#define BUFFER_SIZE      30

/** @brief Capacidade máxima do buffer de alertas acumulados pelos consumidores. */
#define ALERT_BUFFER_SIZE 64

/**
 * @brief Fila circular limitada (bounded buffer) partilhada entre produtores e consumidores.
 *
 * Os produtores inserem LogEntry via o índice @c in e os consumidores retiram
 * via o índice @c out. O acesso concorrente é serializado pelo @c mutex;
 * os semáforos @c empty e @c full controlam os slots disponíveis e ocupados,
 * respetivamente.
 */
typedef struct {
    LogEntry        queue[BUFFER_SIZE]; /**< @brief Array circular de entradas de log prontas a consumir. */
    int             count;              /**< @brief Número de entradas atualmente presentes no buffer. */
    int             in;                 /**< @brief Índice do próximo slot de inserção (produtor). */
    int             out;                /**< @brief Índice do próximo slot de remoção (consumidor). */
    pthread_mutex_t mutex;              /**< @brief Mutex que serializa o acesso ao buffer entre todas as threads. */
    sem_t           empty;              /**< @brief Semáforo que conta os slots vazios disponíveis para produção (valor inicial: BUFFER_SIZE). */
    sem_t           full;               /**< @brief Semáforo que conta os slots preenchidos disponíveis para consumo (valor inicial: 0). */
} LogQueue;

/**
 * @brief Argumentos passados a cada thread produtora.
 *
 * A thread produtora lê o intervalo de bytes indicado nos ficheiros de log,
 * parseia cada linha e deposita as LogEntry resultantes na LogQueue partilhada.
 */
typedef struct {
    char  **ficheiros;       /**< @brief Lista de caminhos dos ficheiros de log a processar. */
    int     total_ficheiros; /**< @brief Número de entradas em @p ficheiros. */
    off_t   byte_inicio;     /**< @brief Offset de byte (inclusivo) onde esta thread deve começar a ler. */
    off_t   byte_fim;        /**< @brief Offset de byte (exclusivo) onde esta thread deve parar de ler. */
    int     worker_index;    /**< @brief Índice deste produtor no conjunto de threads (0-based). */
    int     verbose;         /**< @brief 1 se o modo verboso está ativo; 0 caso contrário. */
    long   *bytes_done;      /**< @brief Ponteiro para o contador partilhado de bytes já processados (progresso global). */
    long   *bytes_total;     /**< @brief Ponteiro para o total global de bytes a processar (denominador do progresso). */
} ProducerArgs;

/**
 * @brief Argumentos passados a cada thread consumidora.
 *
 * A thread consumidora retira LogEntry da LogQueue partilhada, acumula
 * métricas localmente e, no final, funde-as nas métricas globais sob
 * proteção do mutex indicado.
 */
typedef struct {
    Metrics         *global_metrics; /**< @brief Ponteiro para o acumulador de métricas global partilhado entre todos os consumidores. */
    pthread_mutex_t *metrics_mutex;  /**< @brief Mutex que protege o acesso concorrente a @p global_metrics. */
    int              worker_index;   /**< @brief Índice deste consumidor no conjunto de threads (0-based). */
} ConsumerArgs;

/** @brief Buffer circular partilhado entre todas as threads produtoras e consumidoras. */
extern LogQueue        buffer;

/** @brief Número de threads produtoras ainda ativas; quando chega a 0 os consumidores podem terminar. */
extern int             produtores_ativos;

/** @brief Mutex que protege o acesso concorrente às métricas globais. */
extern pthread_mutex_t metrics_mutex;

/**
 * @brief Inicializa o buffer circular e as primitivas de sincronização associadas.
 *
 * Deve ser chamada uma única vez antes de criar qualquer thread produtora ou
 * consumidora. Inicializa o mutex e os semáforos @c empty (BUFFER_SIZE) e
 * @c full (0), e coloca os índices e o contador a zero.
 */
void  init_bounded_buffer(void);

/**
 * @brief Destrói o buffer circular e liberta os recursos de sincronização.
 *
 * Deve ser chamada após a conclusão de todas as threads produtoras e
 * consumidoras. Destrói o mutex e os semáforos POSIX associados ao buffer.
 */
void  destroy_bounded_buffer(void);

/**
 * @brief Função executada por cada thread produtora no modelo produtor-consumidor.
 *
 * Lê e parseia o intervalo de bytes definido em ProducerArgs, depositando
 * cada LogEntry válida no buffer partilhado. Quando termina, decrementa
 * @c produtores_ativos e sinaliza os consumidores para que possam detetar
 * o fim da produção.
 *
 * @param arg Ponteiro para um ProducerArgs com os argumentos desta thread
 *            (passado como void* de acordo com a interface pthread_create).
 * @return Sempre NULL.
 */
void *run_producer(void *arg);

/**
 * @brief Função executada por cada thread consumidora no modelo produtor-consumidor.
 *
 * Retira LogEntry do buffer partilhado e acumula métricas localmente até que
 * não existam mais produtores ativos e o buffer esteja vazio. No final, funde
 * as métricas locais nas métricas globais sob proteção do mutex.
 *
 * @param arg Ponteiro para um ConsumerArgs com os argumentos desta thread
 *            (passado como void* de acordo com a interface pthread_create).
 * @return Sempre NULL.
 */
void *run_consumer(void *arg);

#endif /* WORKER_PRODCONS_H */
