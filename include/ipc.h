#ifndef IPC_H
#define IPC_H

#include <unistd.h>
#include <stdint.h>
#include <sys/types.h>

#include "parser.h"  /* IP_LEN */

/* =========================================================
 * Path do socket Unix Domain
 * ========================================================= */
#define SOCKET_PATH "/tmp/loganalyzer.sock"

/* =========================================================
 * Tipos de mensagens (protocol)
 * ========================================================= */
#define MSG_CONFIG     0  /* Pai envia configuração ao worker */
#define MSG_PROGRESSO  1  /* Worker envia progresso ao pai */
#define MSG_RESULTADO  2  /* Worker envia resultado final ao pai */

/* =========================================================
 * Estrutura enviada do pai para o filho — configuração
 * ========================================================= */
typedef struct {
    off_t byte_inicio;
    off_t byte_fim;
    off_t total_bytes_globais;
    int worker_index;
} WorkerConfig;

/* =========================================================
 * Estrutura enviada do filho para o pai — resultados finais
 * ========================================================= */
typedef struct {
    pid_t pid;
    long total_lines;
    long count_debug;
    long count_info;
    long count_warn;
    long count_error;
    long count_critical;
    long count_4xx;
    long count_5xx;
    char top_ips[10][IP_LEN];
    long top_ips_counts[10];
    char alerts[MAX_ALERTS][ALERT_LEN];
    int num_alerts;
} WorkerResult;

/* =========================================================
 * Estrutura enviada do filho para o pai — progresso
 * ========================================================= */
typedef struct {
    pid_t pid;
    int   worker_index;
    long  bytes_done;
    long  bytes_total;
} ProgressUpdate;

/* =========================================================
 * Funções de Unix Domain Sockets
 * ========================================================= */

/**
 * @brief Liga ao socket servidor (usado pelos filhos/workers).
 * @return Descritor do socket ligado, ou -1 em caso de erro.
 *
 * Tenta ligar até 10 vezes com espera de 50 ms entre tentativas,
 * para tolerar o tempo de arranque do pai.
 */
int connect_to_server(void);

/**
 * @brief Lê exactamente @p nbytes de @p fd, repetindo read() se necessário.
 * @param fd     Descritor de ficheiro (pipe ou socket).
 * @param ptr    Buffer de destino.
 * @param nbytes Número exacto de bytes a ler.
 * @return Número de bytes efectivamente lidos (< nbytes indica EOF prematuro), ou -1 em erro.
 */
ssize_t readn(int fd, void *ptr, size_t nbytes);

/**
 * @brief Escreve exactamente @p nbytes em @p fd, repetindo write() se necessário.
 * @param fd     Descritor de ficheiro (pipe ou socket).
 * @param ptr    Buffer de origem.
 * @param nbytes Número exacto de bytes a escrever.
 * @return Número de bytes escritos (igual a nbytes em sucesso), ou -1 em erro.
 */
ssize_t writen(int fd, void *ptr, size_t nbytes);

/**
 * @brief Função principal do worker de pipes.
 * @param ficheiros        Lista de todos os ficheiros a processar.
 * @param total_ficheiros  Número total de ficheiros.
 * @param pipe_fd_write    Extremo de escrita do pipe para enviar resultados ao pai.
 * @param worker_index     Índice deste worker no array de progresso global.
 * @param byte_inicio      Offset de byte onde este worker começa a processar.
 * @param byte_fim         Offset de byte onde este worker termina (exclusive).
 * @param verbose          1 se --verbose foi passado, 0 caso contrário.
 */
void run_worker_pipe(char **ficheiros, int total_ficheiros, int pipe_fd_write,
                     int worker_index, off_t byte_inicio, off_t byte_fim, int verbose);

/**
 * @brief Converte as métricas internas de um worker num WorkerResult pronto a enviar.
 * @param m Métricas acumuladas pelo worker.
 * @param r Estrutura de saída preenchida com contadores e top 10 IPs ordenados.
 */
void preparar_resultado(const Metrics *m, WorkerResult *r);

/**
 * @brief Funde um WorkerResult parcial no total agregado pelo processo pai.
 * @param total           Acumulador total (modificado in-place).
 * @param r               Resultado parcial de um worker.
 * @param ip_list_global  Lista intermédia de IPs únicos (estado entre chamadas).
 * @param ip_count_global Contagens correspondentes aos IPs em ip_list_global.
 * @param ip_num_global   Número actual de IPs únicos em ip_list_global.
 */
void acumular_resultado(WorkerResult *total, const WorkerResult *r,
                        char ip_list_global[256][IP_LEN],
                        long ip_count_global[256], int *ip_num_global);

#endif /* IPC_H */
