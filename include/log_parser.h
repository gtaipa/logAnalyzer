#ifndef LOG_PARSER_H
#define LOG_PARSER_H

/* =========================================================
 * log_parser.h - Estruturas e prototipos do parser de logs
 *
 * Este header define:
 * - Limites maximos de buffers usados no parsing
 * - Estruturas normalizadas para cada formato suportado
 * - Prototipos das funcoes de parsing e timestamps
 *
 * Convencao de retorno (em geral):
 * - 0  : sucesso (linha reconhecida / parse efetuado)
 * - -1 : erro (linha invalida / parametros invalidos)
 *
 * Nota: alguns campos (ex.: timestamps) sao "best-effort" nas funcoes
 * parse_*_log(): mesmo que o parse do timestamp falhe, o parser pode
 * devolver 0 se o resto do formato bater (ver implementacao).
 * ========================================================= */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include <stdbool.h>

/* ---------------------------------------------------------
 * Limites de buffers
 * --------------------------------------------------------- */
/* Tamanho maximo de uma linha lida do ficheiro de log. */
#define MAX_LINE_LENGTH 4096
/* Maximo para IPv6 textual (inclui terminador NUL). */
#define MAX_IP_LENGTH 46
/* URLs e campos "request" (inclui terminador NUL). */
#define MAX_URL_LENGTH 512
/* Mensagens genericas (inclui terminador NUL). */
#define MAX_MSG_LENGTH 1024

/* ---------------------------------------------------------
 * Apache (Combined Log)
 * Exemplo:
 *  192.168.1.1 - - [13/Feb/2024:10:23:45 +0000] "GET / HTTP/1.1" 200 1234
 * --------------------------------------------------------- */
typedef struct {
    /* IP do cliente (IPv4/IPv6 como string). */
    char ip[MAX_IP_LENGTH];
    /* Timestamp (parte "13/Feb/2024:10:23:45"). Timezone pode ser ignorado. */
    struct tm timestamp;
    /* Metodo HTTP (GET/POST/...). */
    char method[16];
    /* URL/caminho pedido (sem query parsing). */
    char url[MAX_URL_LENGTH];
    /* Versao HTTP (ex.: "1.1"). */
    char http_version[16];
    /* Status code (ex.: 200, 404...). */
    int status_code;
    /* Tamanho da resposta em bytes (quando presente). */
    long response_size;
    /* Campos extra (podem nao ser preenchidos pelo parser atual). */
    char referer[MAX_URL_LENGTH];
    char user_agent[256];
} ApacheLogEntry;

/* ---------------------------------------------------------
 * JSON (Log estruturado simples)
 * Esperado algo do genero:
 *  {"timestamp":"2024-02-13T10:23:45","level":"INFO","service":"api","message":"...","metadata":{"ip":"...","user_id":123}}
 * --------------------------------------------------------- */
typedef struct {
    /* Timestamp ISO-8601 basico (YYYY-MM-DDTHH:MM:SS). */
    struct tm timestamp;
    /* Nivel de severidade (mapeado de strings como "INFO", "WARN", ...). */
    enum {
        LOG_DEBUG = 0,
        LOG_INFO = 1,
        LOG_WARN = 2,
        LOG_ERROR = 3,
        LOG_CRITICAL = 4
    } level;
    /* Nome do servico (ex.: "auth", "api", ...). */
    char service[64];
    /* Mensagem principal. */
    char message[MAX_MSG_LENGTH];
    /* IP dentro de "metadata.ip" (se existir). */
    char ip[MAX_IP_LENGTH];
    /* user_id dentro de "metadata.user_id" (se existir). */
    int user_id;
} JSONLogEntry;

/* ---------------------------------------------------------
 * Syslog (RFC3164-ish)
 * Exemplo:
 *  <34>Feb 13 10:23:45 hostname sshd[1234]: Failed password for invalid user ...
 * --------------------------------------------------------- */
typedef struct {
    /* Prioridade (campo "<nnn>"), ou 0 quando ausente. */
    int priority;
    /* Timestamp "Mon DD HH:MM:SS" (ano inferido do tempo atual). */
    struct tm timestamp;
    /* Hostname logo apos o timestamp. */
    char hostname[256];
    /* Nome do servico (ex.: "sshd", "sudo", ...). */
    char service[64];
    /* PID quando presente em "service[pid]". */
    int pid;
    /* Mensagem restante apos ':'. */
    char message[MAX_MSG_LENGTH];
    /* Flags de deteccao simples (heuristicas por substring). */
    bool is_auth_failure;
    bool is_sudo_attempt;
    bool is_firewall_block;
} SyslogEntry;

/* ---------------------------------------------------------
 * Nginx Error Log
 * Exemplo:
 *  2024/02/13 10:23:45 [error] 12345#0: *1234 connect() failed ..., client: 1.2.3.4, server: ..., request: "GET / ..."
 * --------------------------------------------------------- */
typedef struct {
    /* Timestamp "YYYY/MM/DD HH:MM:SS" (quando parseavel). */
    struct tm timestamp;
    /* Nivel dentro de "[...]" (mapeado para enum). */
    enum {
        NGINX_DEBUG = 0,
        NGINX_INFO = 1,
        NGINX_NOTICE = 2,
        NGINX_WARN = 3,
        NGINX_ERROR = 4,
        NGINX_CRIT = 5,
        NGINX_ALERT = 6,
        NGINX_EMERG = 7
    } level;
    /* PID e TID extraidos de "pid#tid" (quando presente). */
    int pid;
    int tid;
    /* ID de conexao extraido de "*<id>" (quando presente). */
    long connection_id;
    /* Mensagem (texto apos ':', ate ", client:" quando existir). */
    char message[MAX_MSG_LENGTH];
    /* IP do cliente quando existir em "client: <ip>". */
    char client_ip[MAX_IP_LENGTH];
    /* Campos extra (podem nao ser preenchidos pelo parser atual). */
    char server[256];
    char request[MAX_URL_LENGTH];
} NginxErrorEntry;

/* ---------------------------------------------------------
 * Funcoes de parsing por formato
 * --------------------------------------------------------- */
/**
 * Parse de uma linha Apache Combined Log.
 * @param line  Linha de log (string NUL-terminated).
 * @param entry Estrutura de saida (sera zerada antes de preencher).
 * @return 0 em sucesso, -1 em erro de formato/parametros.
 */
int parse_apache_log(const char* line, ApacheLogEntry* entry);

/**
 * Parse de uma linha JSON (formato simplificado).
 * @return 0 em sucesso, -1 em erro de formato/parametros.
 */
int parse_json_log(const char* line, JSONLogEntry* entry);

/**
 * Parse de uma linha Syslog (RFC3164-ish).
 * @return 0 em sucesso, -1 em erro de formato/parametros.
 */
int parse_syslog(const char* line, SyslogEntry* entry);

/**
 * Parse de uma linha do Nginx Error Log.
 * @return 0 em sucesso, -1 em erro de formato/parametros.
 */
int parse_nginx_error(const char* line, NginxErrorEntry* entry);

/* ---------------------------------------------------------
 * Funcoes auxiliares de parsing de timestamps
 * --------------------------------------------------------- */
/**
 * Converte timestamp Apache: "13/Feb/2024:10:23:45 +0000" (timezone ignorado).
 * Preenche tm_out quando possivel.
 * @return 0 em sucesso, -1 se o formato for invalido.
 */
int parse_apache_timestamp(const char* timestamp_str, struct tm* tm_out);

/**
 * Converte timestamp ISO-8601 basico: "YYYY-MM-DDTHH:MM:SS".
 * @return 0 em sucesso, -1 se o formato for invalido.
 */
int parse_iso8601_timestamp(const char* timestamp_str, struct tm* tm_out);

/**
 * Converte timestamp syslog: "Feb 13 10:23:45".
 * O ano e obtido a partir do tempo atual (localtime_r).
 * @return 0 em sucesso, -1 se o formato for invalido.
 */
int parse_syslog_timestamp(const char* timestamp_str, struct tm* tm_out);

#endif /* LOG_PARSER_H */
