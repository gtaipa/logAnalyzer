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

    if (strlen(line) > 20 &&
        isdigit((unsigned char)line[0]) &&
        isdigit((unsigned char)line[1]) &&
        isdigit((unsigned char)line[2]) &&
        isdigit((unsigned char)line[3]) &&
        line[4] == '/') {
        return FORMAT_NGINX_ERROR;
    }

    if (strlen(line) > 15 &&
        isalpha((unsigned char)line[0]) &&
        isalpha((unsigned char)line[1]) &&
        isalpha((unsigned char)line[2]) &&
        line[3] == ' ' &&
        (isdigit((unsigned char)line[4]) || line[4] == ' ')) {
        return FORMAT_SYSLOG;
    }

    {
        const char *p = line;
        int dots = 0;
        while (*p && (isdigit((unsigned char)*p) || *p == '.')) {
            if (*p == '.') dots++;
            p++;
        }
        if (dots == 3 && *p == ' ') return FORMAT_APACHE;
    }

    /* Fallback: tentar os parsers canonicos em ordem. */
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
            if (parse_apache_log(line, &a) != 0) return -1;
            (void)classify_apache_event(&a, &event);
            if (!event_matches_mode(&event, g_mode)) return -1;

            entry->level = level_from_severity(event.severity);
            entry->http_status = a.status_code;
            strncpy(entry->ip, a.ip, IP_LEN - 1);
            entry->ip[IP_LEN - 1] = '\0';
            strncpy(entry->message, a.url, MSG_LEN - 1);
            entry->message[MSG_LEN - 1] = '\0';
            return 0;
        }
        case FORMAT_JSON: {
            JSONLogEntry j;
            if (parse_json_log(line, &j) != 0) return -1;
            (void)classify_json_event(&j, &event);
            if (!event_matches_mode(&event, g_mode)) return -1;

            entry->level = level_from_json_level(j.level);
            strncpy(entry->ip, j.ip, IP_LEN - 1);
            entry->ip[IP_LEN - 1] = '\0';
            strncpy(entry->message, j.message, MSG_LEN - 1);
            entry->message[MSG_LEN - 1] = '\0';
            return 0;
        }
        case FORMAT_SYSLOG: {
            SyslogEntry s;
            if (parse_syslog(line, &s) != 0) return -1;
            (void)classify_syslog_event(&s, &event);
            if (!event_matches_mode(&event, g_mode)) return -1;

            entry->level = level_from_severity(event.severity);
            (void)extract_ipv4(s.message, entry->ip);
            strncpy(entry->message, s.message, MSG_LEN - 1);
            entry->message[MSG_LEN - 1] = '\0';
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
            strncpy(entry->message, n.message, MSG_LEN - 1);
            entry->message[MSG_LEN - 1] = '\0';
            return 0;
        }
        default:
            return -1;
    }
}

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

