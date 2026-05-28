#include "parser.h"

#include "event_classifier.h"
#include "log_parser.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static AnalysisMode g_mode = MODE_FULL;

static LogLevel level_from_severity(int severity) {
    switch (severity) {
        case 0: return LEVEL_INFO;
        case 1: return LEVEL_INFO;
        case 2: return LEVEL_WARN;
        case 3: return LEVEL_ERROR;
        case 4: return LEVEL_CRITICAL;
        default: return LEVEL_UNKNOWN;
    }
}

static LogLevel level_from_json_level(int json_level) {
    switch (json_level) {
        case LOG_DEBUG: return LEVEL_DEBUG;
        case LOG_INFO: return LEVEL_INFO;
        case LOG_WARN: return LEVEL_WARN;
        case LOG_ERROR: return LEVEL_ERROR;
        case LOG_CRITICAL: return LEVEL_CRITICAL;
        default: return LEVEL_UNKNOWN;
    }
}

static void copy_description_or_fallback(LogEntry *entry, const char *description, const char *fallback) {
    const char *text = (description && description[0] != '\0') ? description : fallback;
    if (text == NULL || text[0] == '\0') text = "Evento critico sem descricao";

    strncpy(entry->message, text, MSG_LEN - 1);
    entry->message[MSG_LEN - 1] = '\0';
}

static int extract_ipv4(const char *s, char out[IP_LEN]) {
    if (!s) return -1;
    out[0] = '\0';

    for (const char *p = s; *p; p++) {
        if (!isdigit((unsigned char)*p)) continue;

        int a = -1, b = -1, c = -1, d = -1;
        if (sscanf(p, "%d.%d.%d.%d", &a, &b, &c, &d) != 4) continue;
        if (a < 0 || a > 255 || b < 0 || b > 255 || c < 0 || c > 255 || d < 0 || d > 255) continue;

        snprintf(out, IP_LEN, "%d.%d.%d.%d", a, b, c, d);
        return 0;
    }

    return -1;
}

static const char *skip_syslog_priority(const char *line) {
    const char *p = line;

    if (*p != '<') {
        return p;
    }

    p++;
    if (!isdigit((unsigned char)*p)) {
        return line;
    }

    while (isdigit((unsigned char)*p)) {
        p++;
    }

    return *p == '>' ? p + 1 : line;
}

static int looks_like_syslog_timestamp(const char *line) {
    return strlen(line) > 15 &&
           isalpha((unsigned char)line[0]) &&
           isalpha((unsigned char)line[1]) &&
           isalpha((unsigned char)line[2]) &&
           line[3] == ' ' &&
           (isdigit((unsigned char)line[4]) || line[4] == ' ');
}

int parser_set_mode_from_string(const char *mode_str) {
    if (!mode_str) return -1;
    if (strcasecmp(mode_str, "security") == 0) g_mode = MODE_SECURITY;
    else if (strcasecmp(mode_str, "performance") == 0) g_mode = MODE_PERFORMANCE;
    else if (strcasecmp(mode_str, "traffic") == 0) g_mode = MODE_TRAFFIC;
    else if (strcasecmp(mode_str, "full") == 0) g_mode = MODE_FULL;
    else return -1;
    return 0;
}

LogLevel level_from_string(const char *s) {
    if (s == NULL) return LEVEL_UNKNOWN;

    char buf[16];
    int i;
    for (i = 0; i < 15 && s[i]; i++) buf[i] = (char)toupper((unsigned char)s[i]);
    buf[i] = '\0';

    if (strcmp(buf, "DEBUG") == 0) return LEVEL_DEBUG;
    if (strcmp(buf, "INFO") == 0) return LEVEL_INFO;
    if (strcmp(buf, "NOTICE") == 0) return LEVEL_INFO;
    if (strcmp(buf, "WARN") == 0) return LEVEL_WARN;
    if (strcmp(buf, "WARNING") == 0) return LEVEL_WARN;
    if (strcmp(buf, "ERROR") == 0) return LEVEL_ERROR;
    if (strcmp(buf, "ERR") == 0) return LEVEL_ERROR;
    if (strcmp(buf, "CRIT") == 0) return LEVEL_CRITICAL;
    if (strcmp(buf, "CRITICAL") == 0) return LEVEL_CRITICAL;
    if (strcmp(buf, "ALERT") == 0) return LEVEL_CRITICAL;
    if (strcmp(buf, "EMERG") == 0) return LEVEL_CRITICAL;
    return LEVEL_UNKNOWN;
}

LogFormat detect_format(const char *line) {
    if (line == NULL || *line == '\0') return FORMAT_UNKNOWN;
    if (line[0] == '{') return FORMAT_JSON;

    if (strlen(line) > 20 &&// ver se a mensagem é grande tipico de nginx error log
        isdigit((unsigned char)line[0]) &&
        isdigit((unsigned char)line[1]) &&
        isdigit((unsigned char)line[2]) &&
        isdigit((unsigned char)line[3]) &&//procura se é uma data(4 numeros seguidos)
        line[4] == '/') {//ve se tem a barra na data (2024/../..)
        return FORMAT_NGINX_ERROR;
    }

    if (looks_like_syslog_timestamp(skip_syslog_priority(line))) {
        return FORMAT_SYSLOG;//chama a funcao de syslogs que ve se tem algo deste genero <32>...
    }

    {
        const char *p = line;
        int dots = 0;
        while (*p && (isdigit((unsigned char)*p) || *p == '.')) {
            if (*p == '.') dots++;
            p++;
        }
        if (dots == 3 && *p == ' ') return FORMAT_APACHE;// analisa para ver se o log começa por um ip como o apache
    }

    /* Fallback: tentar os parsers canonicos em ordem. */
    //para nao crashar caso venham corrompidos, aqui ele tenta ler os logs com o formato de cada um para tentar classificar
    {
        ApacheLogEntry a;
        if (parse_apache_log(line, &a) == 0) return FORMAT_APACHE;
        JSONLogEntry j;
        if (parse_json_log(line, &j) == 0) return FORMAT_JSON;
        SyslogEntry s;
        if (parse_syslog(line, &s) == 0) return FORMAT_SYSLOG;
        NginxErrorEntry n;
        if (parse_nginx_error(line, &n) == 0) return FORMAT_NGINX_ERROR;
    }

    return FORMAT_UNKNOWN;
}

int parse_line(const char *line, LogFormat format, LogEntry *entry) {
    if (line == NULL || entry == NULL) return -1;

    entry->format = format;
    entry->level = LEVEL_UNKNOWN;
    entry->http_status = 0;
    entry->ip[0] = '\0';
    entry->message[0] = '\0';

    ClassifiedEvent event;

    switch (format) {
        case FORMAT_APACHE: {
            ApacheLogEntry a;
            if (parse_apache_log(line, &a) != 0) return -1;//manda para o log_parser para tentar ler o log, se nao conseguir ele descarta a linha
            (void)classify_apache_event(&a, &event);//manda para o ficheiro do stor de classificacao de logs de cada tipo
            if (!event_matches_mode(&event, g_mode)) return -1;//manda para o event classifier para ver se o evento corresponde ao modo de analise atual, se nao corresponder ele descarta

            entry->level = level_from_severity(event.severity);// volta a empacootar tudo
            entry->http_status = a.status_code;
            strncpy(entry->ip, a.ip, IP_LEN - 1);
            entry->ip[IP_LEN - 1] = '\0';
            copy_description_or_fallback(entry, event.description, a.url);
            return 0;
        }//O PROCESSO REPETE-SE PARA OS OUTROS FORMATOS: TENTA PARSEAR, CLASSIFICAR, VER SE CORRESPONDE AO MODO, E EMPACOTAR O RESULTADO PARA O UPDATE DAS METRICAS
        case FORMAT_JSON: {
            JSONLogEntry j;
            if (parse_json_log(line, &j) != 0) return -1;
            (void)classify_json_event(&j, &event);
            if (!event_matches_mode(&event, g_mode)) return -1;

            entry->level = level_from_json_level(j.level);
            strncpy(entry->ip, j.ip, IP_LEN - 1);
            entry->ip[IP_LEN - 1] = '\0';
            copy_description_or_fallback(entry, event.description, j.message);
            return 0;
        }
        case FORMAT_SYSLOG: {
            SyslogEntry s;
            if (parse_syslog(line, &s) != 0) return -1;
            (void)classify_syslog_event(&s, &event);
            if (!event_matches_mode(&event, g_mode)) return -1;

            entry->level = level_from_severity(event.severity);
            (void)extract_ipv4(s.message, entry->ip);
            copy_description_or_fallback(entry, event.description, s.message);
            return 0;
        }
        case FORMAT_NGINX_ERROR: {
            NginxErrorEntry n;
            if (parse_nginx_error(line, &n) != 0) return -1;
            (void)classify_nginx_event(&n, &event);
            if (!event_matches_mode(&event, g_mode)) return -1;

            entry->level = level_from_severity(event.severity);
            strncpy(entry->ip, n.client_ip, IP_LEN - 1);
            entry->ip[IP_LEN - 1] = '\0';
            copy_description_or_fallback(entry, event.description, n.message);
            return 0;
        }
        default:
            return -1;
    }
}
//NO FIM ATUALIZA AS METRICAS COM O LOG ENTRY PARSEADO, SE O LOG ENTRY FOR VALIDO E CORRESPONDER AO MODO DE ANALISE ATUAL
void update_metrics(Metrics *m, const LogEntry *e) {
    m->total_lines++;

    switch (e->level) {
        case LEVEL_DEBUG: m->count_debug++; break;
        case LEVEL_INFO: m->count_info++; break;
        case LEVEL_WARN: m->count_warn++; break;
        case LEVEL_ERROR: m->count_error++; break;
        case LEVEL_CRITICAL: m->count_critical++; break;
        default: break;
    }

    if (e->http_status >= 500) m->count_5xx++;
    else if (e->http_status >= 400) m->count_4xx++;

    if ((e->level == LEVEL_ERROR || e->level == LEVEL_CRITICAL) && m->num_alerts < MAX_ALERTS) {
        const char *alert = e->message[0] != '\0' ? e->message : "Evento critico sem descricao";
        strncpy(m->alerts[m->num_alerts], alert, ALERT_LEN - 1);
        m->alerts[m->num_alerts][ALERT_LEN - 1] = '\0';
        m->num_alerts++;
    }

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

void init_metrics(Metrics *m) {
    memset(m, 0, sizeof(Metrics));
}

void merge_metrics(Metrics *dst, const Metrics *src) {
    dst->total_lines    += src->total_lines;
    dst->count_debug    += src->count_debug;
    dst->count_info     += src->count_info;
    dst->count_warn     += src->count_warn;
    dst->count_error    += src->count_error;
    dst->count_critical += src->count_critical;
    dst->count_4xx      += src->count_4xx;
    dst->count_5xx      += src->count_5xx;

    for (int i = 0; i < src->num_alerts && dst->num_alerts < MAX_ALERTS; i++) {
        strncpy(dst->alerts[dst->num_alerts], src->alerts[i], ALERT_LEN - 1);
        dst->alerts[dst->num_alerts][ALERT_LEN - 1] = '\0';
        dst->num_alerts++;
    }

    for (int i = 0; i < src->ip_num; i++) {
        int found = -1;
        for (int j = 0; j < dst->ip_num; j++) {
            if (strcmp(dst->ip_list[j], src->ip_list[i]) == 0) { found = j; break; }
        }
        if (found >= 0) {
            dst->ip_count[found] += src->ip_count[i];
        } else if (dst->ip_num < MAX_IPS) {
            strncpy(dst->ip_list[dst->ip_num], src->ip_list[i], IP_LEN - 1);
            dst->ip_list[dst->ip_num][IP_LEN - 1] = '\0';
            dst->ip_count[dst->ip_num] = src->ip_count[i];
            dst->ip_num++;
        }
    }
}
