/**
 * @file ipc.h
 * @brief Comunicação inter-processos via Unix Domain Sockets entre o processo
 *        pai (servidor) e os processos filho (workers).
 *
 * Define o protocolo de mensagens (tipos MSG_*), as estruturas de dados
 * trocadas pelo socket (WorkerConfig, WorkerResult, ProgressUpdate) e os
 * protótipos das funções auxiliares de leitura/escrita robusta (readn, writen)
 * e de ligação ao servidor (connect_to_server).
 */

#ifndef IPC_H
#define IPC_H

#include <unistd.h>
#include <stdint.h>
#include <sys/types.h>

#include "parser.h"  /* IP_LEN, MAX_ALERTS, ALERT_LEN */

/* =========================================================
 * Path do socket Unix Domain para comunicação pai-filho
 * ========================================================= */
/** @brief Caminho do ficheiro de socket Unix Domain usado na comunicação pai-filho. */
#define SOCKET_PATH "/tmp/loganalyzer.sock"

/* =========================================================
 * Tipos de mensagens (protocol)
 * ========================================================= */
/** @brief Tipo de mensagem: o pai envia a configuração (intervalo de bytes) ao worker. */
#define MSG_CONFIG     0

/** @brief Tipo de mensagem: o worker envia uma atualização de progresso ao pai. */
#define MSG_PROGRESSO  1

/** @brief Tipo de mensagem: o worker envia o resultado final de processamento ao pai. */
#define MSG_RESULTADO  2

/* =========================================================
 * Estrutura enviada do pai para o filho — configuração
 * ========================================================= */
/**
 * @brief Configuração enviada pelo processo pai a cada worker no início da execução.
 *
 * Delimita a fatia do(s) ficheiro(s) de log que o worker deve processar,
 * através de offsets de byte absolutos.
 */
typedef struct {
    off_t byte_inicio;           /**< @brief Offset de byte (inclusivo) onde o worker deve começar a ler. */
    off_t byte_fim;              /**< @brief Offset de byte (exclusivo) onde o worker deve parar de ler. */
    off_t total_bytes_globais;   /**< @brief Total global de bytes a processar (todos os workers); usado para calcular progresso percentual. */
    int   worker_index;          /**< @brief Índice deste worker no conjunto de processos filhos (0-based). */
} WorkerConfig;

/* =========================================================
 * Estrutura enviada do filho para o pai — resultados finais
 * ========================================================= */
/**
 * @brief Resultado final enviado por um worker ao processo pai após terminar o processamento.
 *
 * Agrega as métricas acumuladas durante o processamento da fatia de log
 * atribuída ao worker, incluindo contadores de níveis, erros HTTP,
 * os 10 IPs mais frequentes e os alertas críticos encontrados.
 */
typedef struct {
    pid_t pid;                          /**< @brief PID do processo filho que gerou este resultado. */
    long  total_lines;                  /**< @brief Total de linhas de log processadas pelo worker. */
    long  count_debug;                  /**< @brief Número de entradas com nível DEBUG. */
    long  count_info;                   /**< @brief Número de entradas com nível INFO. */
    long  count_warn;                   /**< @brief Número de entradas com nível WARN. */
    long  count_error;                  /**< @brief Número de entradas com nível ERROR. */
    long  count_critical;               /**< @brief Número de entradas com nível CRITICAL. */
    long  count_4xx;                    /**< @brief Número de respostas HTTP com código 4xx. */
    long  count_5xx;                    /**< @brief Número de respostas HTTP com código 5xx. */
    char  top_ips[10][IP_LEN];          /**< @brief Os 10 endereços IP mais frequentes observados pelo worker. */
    long  top_ips_counts[10];           /**< @brief Contagem de ocorrências de cada IP em top_ips (índice correspondente). */
    char  alerts[MAX_ALERTS][ALERT_LEN];/**< @brief Mensagens de alertas críticos/alta severidade recolhidos pelo worker. */
    int   num_alerts;                   /**< @brief Número de alertas guardados no array alerts. */
} WorkerResult;

/* =========================================================
 * Estrutura enviada do filho para o pai — progresso
 * ========================================================= */
/**
 * @brief Atualização de progresso enviada periodicamente pelo worker ao processo pai.
 *
 * Permite que o pai calcule e exiba a percentagem de conclusão global
 * combinando as atualizações de todos os workers.
 */
typedef struct {
    pid_t pid;           /**< @brief PID do processo filho que enviou esta atualização. */
    int   worker_index;  /**< @brief Índice do worker no conjunto de processos filhos (0-based). */
    long  bytes_done;    /**< @brief Número de bytes já processados por este worker. */
    long  bytes_total;   /**< @brief Total de bytes atribuídos a este worker. */
} ProgressUpdate;

/* =========================================================
 * Funções de Unix Domain Sockets
 * ========================================================= */

/**
 * @brief Liga ao socket servidor Unix Domain (usado pelos processos filho).
 *
 * Cria um socket de domínio Unix e estabelece a ligação ao endereço
 * definido em SOCKET_PATH, que deve estar à escuta pelo processo pai.
 *
 * @return File descriptor do socket ligado em caso de sucesso; -1 em caso de erro.
 */
int connect_to_server(void);

/* =========================================================
 * Funções auxiliares seguras para Pipes/Sockets
 *
 * readn() tenta ler exatamente nbytes, repetindo read()
 * quando ha leituras parciais ou interrupcao por sinal.
 *
 * writen() tenta escrever exatamente nbytes, repetindo write()
 * quando ha escritas parciais ou interrupcao por sinal.
 * ========================================================= */

/**
 * @brief Lê exatamente @p nbytes do file descriptor @p fd.
 *
 * Repete a chamada read() automaticamente em caso de leitura parcial
 * ou interrupção por sinal (EINTR), garantindo que a totalidade dos
 * bytes solicitados é lida antes de regressar.
 *
 * @param fd     File descriptor de onde ler.
 * @param ptr    Buffer de destino com capacidade mínima de @p nbytes.
 * @param nbytes Número exato de bytes a ler.
 * @return Número de bytes lidos (igual a @p nbytes em caso de sucesso),
 *         0 se o fim do ficheiro for atingido antes, ou -1 em erro.
 */
ssize_t readn(int fd, void *ptr, size_t nbytes);

/**
 * @brief Escreve exatamente @p nbytes no file descriptor @p fd.
 *
 * Repete a chamada write() automaticamente em caso de escrita parcial
 * ou interrupção por sinal (EINTR), garantindo que a totalidade dos
 * bytes é escrita antes de regressar.
 *
 * @param fd     File descriptor onde escrever.
 * @param ptr    Buffer de origem com pelo menos @p nbytes bytes.
 * @param nbytes Número exato de bytes a escrever.
 * @return Número de bytes escritos (igual a @p nbytes em caso de sucesso),
 *         ou -1 em caso de erro.
 */
ssize_t writen(int fd, void *ptr, size_t nbytes);

/**
 * @brief Executa o processamento de log num worker comunicando via pipe.
 *
 * Versão do worker que usa um pipe anónimo em vez de Unix Domain Socket
 * para enviar resultados ao processo pai. Processa os ficheiros indicados
 * no intervalo de bytes [@p byte_inicio, @p byte_fim[ e escreve o
 * WorkerResult no descritor @p pipe_fd_write.
 *
 * @param ficheiros        Lista de caminhos dos ficheiros de log a processar.
 * @param total_ficheiros  Número de entradas em @p ficheiros.
 * @param pipe_fd_write    Extremidade de escrita do pipe para comunicar com o pai.
 * @param worker_index     Índice deste worker no conjunto de processos filhos (0-based).
 * @param byte_inicio      Offset de byte (inclusivo) onde começar a leitura.
 * @param byte_fim         Offset de byte (exclusivo) onde terminar a leitura.
 * @param verbose          1 se o modo verboso está ativo; 0 caso contrário.
 */
void run_worker_pipe(char **ficheiros, int total_ficheiros, int pipe_fd_write,
                     int worker_index, off_t byte_inicio, off_t byte_fim, int verbose);

#endif /* IPC_H */
