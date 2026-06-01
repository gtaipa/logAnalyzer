/**
 * @file log_parser.h
 * @brief Estruturas normalizadas e protótipos dos parsers de baixo nível para
 *        cada formato de log suportado pelo logAnalyzer.
 *
 * Este header define:
 * - Limites máximos de buffers usados no parsing.
 * - Estruturas normalizadas para cada formato suportado:
 *   ApacheLogEntry, JSONLogEntry, SyslogEntry, NginxErrorEntry.
 * - Protótipos das funções de parsing por formato (parse_apache_log, etc.).
 * - Protótipos das funções auxiliares de parsing de timestamps.
 *
 * Convenção de retorno (em geral):
 * - 0  : sucesso (linha reconhecida / parse efetuado).
 * - -1 : erro (linha inválida / parâmetros inválidos).
 *
 * Nota: alguns campos (ex.: timestamps) são "best-effort" nas funções
 * parse_*_log(): mesmo que o parse do timestamp falhe, o parser pode
 * devolver 0 se o resto do formato bater (ver implementação).
 */

#ifndef LOG_PARSER_H
#define LOG_PARSER_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include <stdbool.h>

/* ---------------------------------------------------------
 * Limites de buffers
 * --------------------------------------------------------- */

/** @brief Tamanho máximo de uma linha lida do ficheiro de log (inclui terminador NUL). */
#define MAX_LINE_LENGTH 4096

/** @brief Tamanho máximo de um endereço IP em texto, suportando IPv6 (inclui terminador NUL). */
#define MAX_IP_LENGTH 46

/** @brief Tamanho máximo de uma URL ou campo "request" (inclui terminador NUL). */
#define MAX_URL_LENGTH 512

/** @brief Tamanho máximo de uma mensagem genérica de log (inclui terminador NUL). */
#define MAX_MSG_LENGTH 1024

/* ---------------------------------------------------------
 * Apache (Combined Log)
 * Exemplo:
 *  192.168.1.1 - - [13/Feb/2024:10:23:45 +0000] "GET / HTTP/1.1" 200 1234
 * --------------------------------------------------------- */

/**
 * @brief Entrada de log Apache Combined Log já parseada.
 *
 * Representa todos os campos extraídos de uma linha no formato Apache
 * Combined Log. Campos que não constem na linha ficam com valor zero/vazio.
 */
typedef struct {
    char      ip[MAX_IP_LENGTH];       /**< @brief Endereço IP do cliente (IPv4 ou IPv6 como string). */
    struct tm timestamp;               /**< @brief Timestamp da linha ("DD/Mon/YYYY:HH:MM:SS"); timezone ignorado. */
    char      method[16];              /**< @brief Método HTTP da requisição (ex.: "GET", "POST"). */
    char      url[MAX_URL_LENGTH];     /**< @brief URL/caminho pedido, sem query string parseada. */
    char      http_version[16];        /**< @brief Versão do protocolo HTTP (ex.: "1.1", "2.0"). */
    int       status_code;             /**< @brief Código de estado HTTP da resposta (ex.: 200, 404). */
    long      response_size;           /**< @brief Tamanho da resposta em bytes; -1 ou 0 quando ausente. */
    char      referer[MAX_URL_LENGTH]; /**< @brief Cabeçalho Referer da requisição (pode estar vazio). */
    char      user_agent[256];         /**< @brief Cabeçalho User-Agent do cliente (pode estar vazio). */
} ApacheLogEntry;

/* ---------------------------------------------------------
 * JSON (Log estruturado simples)
 * Esperado algo do género:
 *  {"timestamp":"2024-02-13T10:23:45","level":"INFO","service":"api","message":"...","metadata":{"ip":"...","user_id":123}}
 * --------------------------------------------------------- */

/**
 * @brief Entrada de log JSON estruturado já parseada.
 *
 * Representa os campos extraídos de uma linha de log no formato JSON
 * simplificado (objeto de nível único com campos fixos e bloco "metadata").
 */
typedef struct {
    struct tm timestamp;           /**< @brief Timestamp ISO-8601 básico ("YYYY-MM-DDTHH:MM:SS"). */
    /**
     * @brief Nível de severidade mapeado a partir da string "level" do JSON.
     */
    enum {
        LOG_DEBUG    = 0, /**< @brief Nível de depuração; detalhe máximo de execução. */
        LOG_INFO     = 1, /**< @brief Informação operacional normal. */
        LOG_WARN     = 2, /**< @brief Aviso de situação potencialmente problemática. */
        LOG_ERROR    = 3, /**< @brief Erro que impede uma operação específica. */
        LOG_CRITICAL = 4  /**< @brief Erro crítico de alta severidade. */
    } level;
    char service[64];              /**< @brief Nome do serviço que gerou o evento (ex.: "auth", "api"). */
    char message[MAX_MSG_LENGTH];  /**< @brief Mensagem principal do evento de log. */
    char ip[MAX_IP_LENGTH];        /**< @brief Endereço IP extraído de "metadata.ip", quando presente. */
    int  user_id;                  /**< @brief Identificador do utilizador de "metadata.user_id"; 0 quando ausente. */
} JSONLogEntry;

/* ---------------------------------------------------------
 * Syslog (RFC3164-ish)
 * Exemplo:
 *  <34>Feb 13 10:23:45 hostname sshd[1234]: Failed password for invalid user ...
 * --------------------------------------------------------- */

/**
 * @brief Entrada de log Syslog (RFC 3164) já parseada.
 *
 * Representa os campos extraídos de uma linha no formato syslog tradicional,
 * incluindo prioridade, timestamp, hostname, serviço, PID e mensagem.
 * Inclui também flags de deteção heurística para categorias de segurança comuns.
 */
typedef struct {
    int       priority;            /**< @brief Campo de prioridade numérica "<nnn>"; 0 quando ausente. */
    struct tm timestamp;           /**< @brief Timestamp no formato "Mon DD HH:MM:SS" (ano inferido do tempo atual). */
    char      hostname[256];       /**< @brief Hostname do sistema que gerou a mensagem. */
    char      service[64];         /**< @brief Nome do serviço ou processo (ex.: "sshd", "sudo", "kernel"). */
    int       pid;                 /**< @brief PID do processo, quando presente em "serviço[pid]"; 0 caso contrário. */
    char      message[MAX_MSG_LENGTH]; /**< @brief Texto da mensagem após o separador ':'. */
    bool      is_auth_failure;     /**< @brief true se a heurística detetou uma falha de autenticação na mensagem. */
    bool      is_sudo_attempt;     /**< @brief true se a heurística detetou uma tentativa de elevação via sudo. */
    bool      is_firewall_block;   /**< @brief true se a heurística detetou um bloqueio de firewall na mensagem. */
} SyslogEntry;

/* ---------------------------------------------------------
 * Nginx Error Log
 * Exemplo:
 *  2024/02/13 10:23:45 [error] 12345#0: *1234 connect() failed ..., client: 1.2.3.4, server: ..., request: "GET / ..."
 * --------------------------------------------------------- */

/**
 * @brief Entrada de log Nginx Error Log já parseada.
 *
 * Representa os campos extraídos de uma linha no formato de log de erros
 * do Nginx, incluindo timestamp, nível, PID, TID, ID de conexão, mensagem
 * e campos adicionais quando presentes.
 */
typedef struct {
    struct tm timestamp;           /**< @brief Timestamp no formato "YYYY/MM/DD HH:MM:SS", quando parseável. */
    /**
     * @brief Nível de severidade mapeado a partir da string entre colchetes "[...]".
     */
    enum {
        NGINX_DEBUG  = 0, /**< @brief Nível de depuração detalhado do Nginx. */
        NGINX_INFO   = 1, /**< @brief Informação operacional do Nginx. */
        NGINX_NOTICE = 2, /**< @brief Aviso de notificação (menos grave que WARN). */
        NGINX_WARN   = 3, /**< @brief Aviso de situação potencialmente problemática. */
        NGINX_ERROR  = 4, /**< @brief Erro que impede o processamento de um pedido. */
        NGINX_CRIT   = 5, /**< @brief Erro crítico que pode comprometer o serviço. */
        NGINX_ALERT  = 6, /**< @brief Alerta que exige ação imediata. */
        NGINX_EMERG  = 7  /**< @brief Emergência; o sistema pode estar inutilizável. */
    } level;
    int       pid;                 /**< @brief PID do processo worker do Nginx (extraído de "pid#tid"). */
    int       tid;                 /**< @brief TID do thread worker do Nginx (extraído de "pid#tid"). */
    long      connection_id;       /**< @brief Identificador de conexão extraído de "*<id>"; 0 quando ausente. */
    char      message[MAX_MSG_LENGTH]; /**< @brief Texto da mensagem de erro até ", client:" quando existir. */
    char      client_ip[MAX_IP_LENGTH]; /**< @brief Endereço IP do cliente em "client: <ip>"; vazio quando ausente. */
    char      server[256];         /**< @brief Nome do servidor virtual em "server: <name>"; vazio quando ausente. */
    char      request[MAX_URL_LENGTH]; /**< @brief Linha de pedido HTTP em "request: \"...\""; vazia quando ausente. */
} NginxErrorEntry;

/* ---------------------------------------------------------
 * Funções de parsing por formato
 * --------------------------------------------------------- */

/**
 * @brief Parseia uma linha no formato Apache Combined Log.
 *
 * Interpreta @p line segundo o formato Apache Combined Log e preenche
 * a estrutura apontada por @p entry com os campos extraídos.
 * A estrutura é zerada antes de ser preenchida.
 *
 * @param line  Linha de log (string terminada em NUL).
 * @param entry Ponteiro para a estrutura ApacheLogEntry a preencher.
 * @return 0 em caso de sucesso; -1 se o formato for inválido ou os parâmetros forem NULL.
 */
int parse_apache_log(const char* line, ApacheLogEntry* entry);

/**
 * @brief Parseia uma linha no formato JSON simplificado.
 *
 * Interpreta @p line como um objeto JSON com os campos fixos esperados
 * e preenche a estrutura apontada por @p entry.
 * A estrutura é zerada antes de ser preenchida.
 *
 * @param line  Linha de log JSON (string terminada em NUL).
 * @param entry Ponteiro para a estrutura JSONLogEntry a preencher.
 * @return 0 em caso de sucesso; -1 se o formato for inválido ou os parâmetros forem NULL.
 */
int parse_json_log(const char* line, JSONLogEntry* entry);

/**
 * @brief Parseia uma linha no formato Syslog (RFC 3164).
 *
 * Interpreta @p line segundo o formato syslog tradicional e preenche
 * a estrutura apontada por @p entry, incluindo a deteção heurística
 * de falhas de autenticação, tentativas sudo e bloqueios de firewall.
 * A estrutura é zerada antes de ser preenchida.
 *
 * @param line  Linha de log syslog (string terminada em NUL).
 * @param entry Ponteiro para a estrutura SyslogEntry a preencher.
 * @return 0 em caso de sucesso; -1 se o formato for inválido ou os parâmetros forem NULL.
 */
int parse_syslog(const char* line, SyslogEntry* entry);

/**
 * @brief Parseia uma linha no formato Nginx Error Log.
 *
 * Interpreta @p line segundo o formato de log de erros do Nginx e
 * preenche a estrutura apontada por @p entry com todos os campos
 * reconhecíveis.
 * A estrutura é zerada antes de ser preenchida.
 *
 * @param line  Linha de log Nginx (string terminada em NUL).
 * @param entry Ponteiro para a estrutura NginxErrorEntry a preencher.
 * @return 0 em caso de sucesso; -1 se o formato for inválido ou os parâmetros forem NULL.
 */
int parse_nginx_error(const char* line, NginxErrorEntry* entry);

/* ---------------------------------------------------------
 * Funções auxiliares de parsing de timestamps
 * --------------------------------------------------------- */

/**
 * @brief Converte um timestamp Apache para struct tm.
 *
 * Interpreta @p timestamp_str no formato "DD/Mon/YYYY:HH:MM:SS +ZZZZ"
 * (timezone ignorado) e preenche @p tm_out.
 *
 * @param timestamp_str String com o timestamp Apache (terminada em NUL).
 * @param tm_out        Ponteiro para a struct tm a preencher.
 * @return 0 em caso de sucesso; -1 se o formato for inválido.
 */
int parse_apache_timestamp(const char* timestamp_str, struct tm* tm_out);

/**
 * @brief Converte um timestamp ISO-8601 básico para struct tm.
 *
 * Interpreta @p timestamp_str no formato "YYYY-MM-DDTHH:MM:SS"
 * e preenche @p tm_out.
 *
 * @param timestamp_str String com o timestamp ISO-8601 (terminada em NUL).
 * @param tm_out        Ponteiro para a struct tm a preencher.
 * @return 0 em caso de sucesso; -1 se o formato for inválido.
 */
int parse_iso8601_timestamp(const char* timestamp_str, struct tm* tm_out);

/**
 * @brief Converte um timestamp syslog para struct tm.
 *
 * Interpreta @p timestamp_str no formato "Mon DD HH:MM:SS" e preenche
 * @p tm_out. O ano é obtido a partir do tempo atual via localtime_r(),
 * uma vez que o formato syslog RFC 3164 não inclui o ano.
 *
 * @param timestamp_str String com o timestamp syslog (terminada em NUL).
 * @param tm_out        Ponteiro para a struct tm a preencher.
 * @return 0 em caso de sucesso; -1 se o formato for inválido.
 */
int parse_syslog_timestamp(const char* timestamp_str, struct tm* tm_out);

#endif /* LOG_PARSER_H */
