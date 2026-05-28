#ifndef PARSER_H
#define PARSER_H

/* =========================================================
 * parser.h  –  Estruturas e protótipos do parser de logs
 * ========================================================= */

#include <stddef.h>  /* size_t */

/* ---------------------------------------------------------
 * Formatos de log suportados
 * --------------------------------------------------------- */
typedef enum {
    FORMAT_UNKNOWN = 0,
    FORMAT_APACHE,      /* 192.168.1.1 - - [13/Feb/2024:...] "GET /..." 200 1234 */
    FORMAT_JSON,        /* {"timestamp":..., "level":..., "message":...}          */
    FORMAT_SYSLOG,      /* Feb 13 10:23:45 hostname service[pid]: message         */
    FORMAT_NGINX_ERROR  /* 2024/02/13 10:23:45 [error] 12345#0: ...               */
} LogFormat;

/* ------------------------------------------------------------
 * Níveis / tipos de evento
 * --------------------------------------------------------- */
typedef enum {
    LEVEL_UNKNOWN = 0,
    LEVEL_DEBUG,
    LEVEL_INFO,
    LEVEL_WARN,
    LEVEL_ERROR,
    LEVEL_CRITICAL
} LogLevel;

/* ---------------------------------------------------------
 * Uma entrada de log já parseada
 * --------------------------------------------------------- */
#define IP_LEN      16   /* "255.255.255.255\0" */
#define MSG_LEN    256
#define MAX_ALERTS 20
#define ALERT_LEN  128

typedef struct {
    LogFormat format;
    LogLevel  level;
    int       http_status;          /* 0 se não aplicável          */
    char      ip[IP_LEN];           /* IP do cliente (se existir)  */
    char      message[MSG_LEN];     /* Mensagem / URL / descrição  */
} LogEntry;

/* ---------------------------------------------------------
 * Acumulador de métricas por worker
 * --------------------------------------------------------- */
#define MAX_IPS 1024   /* máx. IPs distintos que rastreamos */

typedef struct {
    long total_lines;
    long count_debug;
    long count_info;
    long count_warn;
    long count_error;
    long count_critical;
    long count_4xx;     /* erros HTTP 4xx */
    long count_5xx;     /* erros HTTP 5xx */

    /* Top IPs: guardamos pares (ip, contagem) */
    char ip_list[MAX_IPS][IP_LEN];
    long ip_count[MAX_IPS];
    int  ip_num;        /* quantos IPs distintos encontrámos até agora */

    /* Alertas críticos/alta severidade guardados por worker */
    char alerts[MAX_ALERTS][ALERT_LEN];
    int  num_alerts;
} Metrics;

/* ---------------------------------------------------------
 * Protótipos
 * --------------------------------------------------------- */

/**
 * @brief Deteta o formato de uma linha de log.
 * @param line Linha de texto a analisar.
 * @return O LogFormat correspondente, ou FORMAT_UNKNOWN se não reconhecido.
 */
LogFormat detect_format(const char *line);

/**
 * @brief Parseia uma linha de acordo com o formato indicado.
 * @param line   Linha de texto a parsear.
 * @param format Formato detectado previamente (ex: FORMAT_APACHE).
 * @param entry  Estrutura de saída preenchida com os dados extraídos.
 * @return 0 em sucesso, -1 se a linha não puder ser parseada ou não corresponde ao modo activo.
 */
int parse_line(const char *line, LogFormat format, LogEntry *entry);

/**
 * @brief Acumula os dados de uma entrada nas métricas de um worker.
 * @param m     Métricas acumuladas (modificadas in-place).
 * @param entry Entrada de log já parseada a acumular.
 */
void update_metrics(Metrics *m, const LogEntry *entry);

/**
 * @brief Inicializa uma estrutura Metrics a zeros.
 * @param m Estrutura a inicializar.
 */
void init_metrics(Metrics *m);

/**
 * @brief Converte uma string de nível para LogLevel.
 * @param s String com o nível (ex: "ERROR", "warn", "CRITICAL").
 * @return O LogLevel correspondente, ou LEVEL_UNKNOWN se não reconhecido.
 */
LogLevel level_from_string(const char *s);

/**
 * @brief Define o modo de análise a partir do argumento CLI.
 * @param mode_str String do modo: "security", "performance", "traffic" ou "full".
 * @return 0 em sucesso, -1 se a string for inválida.
 *
 * O modo afecta parse_line(): entradas que não correspondem ao modo
 * activo são ignoradas (parse_line retorna -1).
 */
int parser_set_mode_from_string(const char *mode_str);

/**
 * @brief Funde as métricas de @p src em @p dst, somando contadores e desduplicando IPs.
 * @param dst Métricas de destino (acumulador global).
 * @param src Métricas locais do worker a fundir.
 */
void merge_metrics(Metrics *dst, const Metrics *src);

#endif /* PARSER_H */
