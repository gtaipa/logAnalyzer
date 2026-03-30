#include "parser.h"

#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>

/* =========================================================
 * Utilitários internos
 * ========================================================= */

/* ===========================================================
 * level_from_string
 * ========================================================= */
LogLevel level_from_string(const char *s) {
    if (s == NULL) return LEVEL_UNKNOWN;

    /* cópia local em maiúsculas para comparar */
    char buf[16];
    int i;
    for (i = 0; i < 15 && s[i]; i++) buf[i] = toupper((unsigned char)s[i]);
    buf[i] = '\0';

    if (strcmp(buf, "DEBUG")    == 0) return LEVEL_DEBUG;
    if (strcmp(buf, "INFO")     == 0) return LEVEL_INFO;
    if (strcmp(buf, "NOTICE")   == 0) return LEVEL_INFO;   /* syslog notice → INFO */
    if (strcmp(buf, "WARN")     == 0) return LEVEL_WARN;
    if (strcmp(buf, "WARNING")  == 0) return LEVEL_WARN;
    if (strcmp(buf, "ERROR")    == 0) return LEVEL_ERROR;
    if (strcmp(buf, "ERR")      == 0) return LEVEL_ERROR;
    if (strcmp(buf, "CRIT")     == 0) return LEVEL_CRITICAL;
    if (strcmp(buf, "CRITICAL") == 0) return LEVEL_CRITICAL;
    if (strcmp(buf, "ALERT")    == 0) return LEVEL_CRITICAL;
    if (strcmp(buf, "EMERG")    == 0) return LEVEL_CRITICAL;
    return LEVEL_UNKNOWN;
}

/* =========================================================
 * detect_format
 *
 * Heurísticas simples para identificar o formato:
 *   Apache  – começa com um IP seguido de " - "
 *   JSON    – começa com '{'
 *   Nginx   – começa com "AAAA/MM/DD HH:MM:SS ["
 *   Syslog  – começa com "Mmm DD HH:MM:SS "
 * ========================================================= */
LogFormat detect_format(const char *line) {
    if (line == NULL || *line == '\0') return FORMAT_UNKNOWN;

    /* JSON: primeira letra é '{' */
    if (line[0] == '{') return FORMAT_JSON;

    /* Nginx error: "YYYY/MM/DD HH:MM:SS [level]" */
    /* Exemplo:  2024/02/13 10:23:45 [error] */
    if (strlen(line) > 20) {
        /* verifica padrão digito*4 + '/' */
        if (isdigit((unsigned char)line[0]) &&
            isdigit((unsigned char)line[1]) &&
            isdigit((unsigned char)line[2]) &&
            isdigit((unsigned char)line[3]) &&
            line[4] == '/') {
            return FORMAT_NGINX_ERROR;
        }
    }

    /* Syslog: começa com 3 letras de mês + espaço + dia */
    /* Exemplo:  Feb 13 10:23:45 hostname */
    if (strlen(line) > 15) {
        if (isalpha((unsigned char)line[0]) &&
            isalpha((unsigned char)line[1]) &&
            isalpha((unsigned char)line[2]) &&
            line[3] == ' ' &&
            (isdigit((unsigned char)line[4]) || line[4] == ' ')) {
            return FORMAT_SYSLOG;
        }
    }

    /* Apache: começa com um IP (dígitos e pontos) seguido de " - " */
    {
        const char *p = line;
        int dots = 0;
        while (*p && (isdigit((unsigned char)*p) || *p == '.')) {
            if (*p == '.') dots++;
            p++;
        }
        if (dots == 3 && *p == ' ') return FORMAT_APACHE;
    }

    return FORMAT_UNKNOWN;
}

/* =========================================================
 * Parsers específicos por formato
 * ========================================================= */

/* ---------------------------------------------------------
 * Apache Combined Log
 * 192.168.1.100 - - [13/Feb/2024:10:23:45 +0000]
 *   "GET /api/users HTTP/1.1" 200 1234 "referer" "ua"
 * --------------------------------------------------------- */
static int parse_apache(const char *line, LogEntry *e) {
    /* Extrair IP (tudo até ao primeiro espaço) */
    int i = 0;
    while (line[i] && line[i] != ' ' && i < IP_LEN - 1) {
        e->ip[i] = line[i];
        i++;
    }
    e->ip[i] = '\0';

    /* Saltar " - - [timestamp] " */
    const char *p = strchr(line, ']');
    if (p == NULL) return -1;
    p++;  /* passa o ']' */
    while (*p == ' ') p++;

    /* Extrair URL: entre as primeiras aspas */
    if (*p != '"') return -1;
    p++;  /* passa a " inicial */
    /* Saltar o método HTTP (GET, POST, …) */
    while (*p && *p != ' ') p++;
    while (*p == ' ') p++;
    /* Agora p aponta para a URL */
    int j = 0;
    while (*p && *p != ' ' && *p != '"' && j < MSG_LEN - 1) {
        e->message[j++] = *p++;
    }
    e->message[j] = '\0';

    /* Saltar até ao status code (após o fecho de aspas e espaços) */
    p = strchr(p, '"');
    if (p == NULL) return -1;
    p++;
    while (*p == ' ') p++;

    /* Ler status code */
    e->http_status = atoi(p);

    /* Nível com base no status code */
    if (e->http_status >= 500)       e->level = LEVEL_ERROR;
    else if (e->http_status >= 400)  e->level = LEVEL_WARN;
    else if (e->http_status >= 300)  e->level = LEVEL_INFO;
    else                             e->level = LEVEL_INFO;

    return 0;
}

/* ---------------------------------------------------------
 * JSON Structured Log
 * {"timestamp":"...","level":"ERROR","message":"...","ip":"..."}
 *
 * Parsing manual simples — sem dependências externas.
 * --------------------------------------------------------- */
static void json_extract(const char *json, const char *key, char *out, int out_len) {
    out[0] = '\0';
    /* Procura "key": */
    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (p == NULL) return;
    p += strlen(search);
    while (*p == ' ' || *p == ':') p++;

    if (*p == '"') {
        /* valor entre aspas */
        p++;
        int i = 0;
        while (*p && *p != '"' && i < out_len - 1) out[i++] = *p++;
        out[i] = '\0';
    } else {
        /* valor numérico ou booleano */
        int i = 0;
        while (*p && *p != ',' && *p != '}' && i < out_len - 1) out[i++] = *p++;
        out[i] = '\0';
    }
}

static int parse_json(const char *line, LogEntry *e) {
    char level_str[32];
    json_extract(line, "level",   level_str,  sizeof(level_str));
    json_extract(line, "message", e->message, MSG_LEN);
    json_extract(line, "ip",      e->ip,      IP_LEN);

    e->level       = level_from_string(level_str);
    e->http_status = 0;

    /* Alguns JSON logs têm status_code */
    char status_str[8];
    json_extract(line, "status_code", status_str, sizeof(status_str));
    if (status_str[0] != '\0') {
        e->http_status = atoi(status_str);
        if (e->http_status >= 500 && e->level < LEVEL_ERROR)
            e->level = LEVEL_ERROR;
    }

    return 0;
}

/* ---------------------------------------------------------
 * Syslog (RFC 3164)
 * Feb 13 10:23:45 hostname service[pid]: message
 * --------------------------------------------------------- */
static int parse_syslog(const char *line, LogEntry *e) {
    e->ip[0]      = '\0';
    e->http_status = 0;

    /* Saltar "Mmm DD HH:MM:SS " (16 caracteres típicos) */
    const char *p = line;
    int spaces = 0;
    while (*p && spaces < 3) {
        if (*p == ' ') {
            spaces++;
            /* Saltar espaços duplos (dia < 10 → "Feb  3") */
            while (p[1] == ' ') p++;
        }
        p++;
    }
    /* p aponta para hostname */
    /* Saltar hostname */
    while (*p && *p != ' ') p++;
    while (*p == ' ') p++;
    /* p aponta para "service[pid]: message" */

    /* Copiar o resto como mensagem */
    int j = 0;
    while (*p && j < MSG_LEN - 1) e->message[j++] = *p++;
    e->message[j] = '\0';

    /* Detetar nível a partir de palavras-chave na mensagem */
    if (strstr(e->message, "error")    || strstr(e->message, "Error")    ||
        strstr(e->message, "ERROR"))       e->level = LEVEL_ERROR;
    else if (strstr(e->message, "warn")  || strstr(e->message, "WARN"))
                                           e->level = LEVEL_WARN;
    else if (strstr(e->message, "crit")  || strstr(e->message, "CRIT") ||
             strstr(e->message, "emerg") || strstr(e->message, "alert"))
                                           e->level = LEVEL_CRITICAL;
    else                                   e->level = LEVEL_INFO;

    return 0;
}

/* ---------------------------------------------------------
 * Nginx Error Log
 * 2024/02/13 10:23:45 [error] 12345#0: *1234 mensagem
 * --------------------------------------------------------- */
static int parse_nginx(const char *line, LogEntry *e) {
    e->ip[0]      = '\0';
    e->http_status = 0;

    /* Saltar timestamp (19 chars "YYYY/MM/DD HH:MM:SS") + espaço */
    const char *p = line;
    if (strlen(p) < 21) return -1;
    p += 20;  /* após "YYYY/MM/DD HH:MM:SS " */

    /* Extrair nível: está entre '[' e ']' */
    if (*p != '[') return -1;
    p++;
    char level_str[16];
    int i = 0;
    while (*p && *p != ']' && i < 15) level_str[i++] = *p++;
    level_str[i] = '\0';
    e->level = level_from_string(level_str);
    if (*p == ']') p++;
    while (*p == ' ') p++;

    /* Saltar PID "12345#0: " */
    while (*p && *p != ':') p++;
    if (*p == ':') { p++; p++; }  /* passa ": " */

    /* Extrair IP do cliente se existir ("client: x.x.x.x") */
    const char *client = strstr(p, "client: ");
    if (client) {
        client += 8;
        int j = 0;
        while (*client && *client != ',' && *client != ' ' && j < IP_LEN - 1)
            e->ip[j++] = *client++;
        e->ip[j] = '\0';
    }

    /* Mensagem: tudo a partir de p */
    int j = 0;
    while (*p && j < MSG_LEN - 1) e->message[j++] = *p++;
    e->message[j] = '\0';

    return 0;
}

/* =========================================================
 * parse_line  –  dispatcher principal
 * ========================================================= */
int parse_line(const char *line, LogFormat format, LogEntry *entry) {
    if (line == NULL || entry == NULL) return -1;

    /* Inicializar entry */
    entry->format      = format;
    entry->level       = LEVEL_UNKNOWN;
    entry->http_status = 0;
    entry->ip[0]       = '\0';
    entry->message[0]  = '\0';

    switch (format) {
        case FORMAT_APACHE:      return parse_apache(line, entry);
        case FORMAT_JSON:        return parse_json(line, entry);
        case FORMAT_SYSLOG:      return parse_syslog(line, entry);
        case FORMAT_NGINX_ERROR: return parse_nginx(line, entry);
        default:                 return -1;
    }
}

/* =========================================================
 * update_metrics
 * ========================================================= */
void update_metrics(Metrics *m, const LogEntry *e) {
    m->total_lines++;

    switch (e->level) {
        case LEVEL_DEBUG:    m->count_debug++;    break;
        case LEVEL_INFO:     m->count_info++;     break;
        case LEVEL_WARN:     m->count_warn++;     break;
        case LEVEL_ERROR:    m->count_error++;    break;
        case LEVEL_CRITICAL: m->count_critical++; break;
        default: break;
    }

    if (e->http_status >= 500)      m->count_5xx++;
    else if (e->http_status >= 400) m->count_4xx++;

    /* Atualizar top IPs */
    if (e->ip[0] != '\0') {
        int found = 0;
        for (int i = 0; i < m->ip_num; i++) {
            if (strcmp(m->ip_list[i], e->ip) == 0) {
                m->ip_count[i]++;
                found = 1;
                break;
            }
        }
        if (!found && m->ip_num < MAX_IPS) {
            strncpy(m->ip_list[m->ip_num], e->ip, IP_LEN - 1);
            m->ip_list[m->ip_num][IP_LEN - 1] = '\0';
            m->ip_count[m->ip_num] = 1;
            m->ip_num++;
        }
    }
}

/* =========================================================
 * init_metrics
 * ========================================================= */
void init_metrics(Metrics *m) {
    memset(m, 0, sizeof(Metrics));
}