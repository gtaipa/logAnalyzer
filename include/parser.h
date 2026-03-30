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

/* ---------------------------------------------------------
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
} Metrics;

/* ---------------------------------------------------------
 * Protótipos
 * --------------------------------------------------------- */

/**
 * Deteta o formato de uma linha de log.
 * Retorna o LogFormat correspondente ou FORMAT_UNKNOWN.
 */
LogFormat detect_format(const char *line);

/**
 * Parseia uma linha de acordo com o formato indicado.
 * Preenche *entry e retorna 0 em sucesso, -1 em erro.
 */
int parse_line(const char *line, LogFormat format, LogEntry *entry);

/**
 * Acumula os dados de *entry nas métricas *m.
 */
void update_metrics(Metrics *m, const LogEntry *entry);

/**
 * Inicializa uma estrutura Metrics a zeros.
 */
void init_metrics(Metrics *m);

/**
 * Converte uma string de nível ("ERROR", "warn", etc.) para LogLevel.
 */
LogLevel level_from_string(const char *s);

#endif /* PARSER_H */