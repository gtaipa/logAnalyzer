#include "parser.h"

#include "event_classifier.h"
#include "log_parser.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static AnalysisMode g_mode = MODE_FULL;

/**
 * @brief Converte severidade numérica (classifier) em LogLevel.
 * @param severity Nível numérico (0=INFO, 2=WARN, 3=ERROR, 4=CRITICAL).
 * @return O LogLevel correspondente.
 */
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

/**
 * @brief Converte o nível numérico de um log JSON em LogLevel.
 * @param json_level Constante definida em log_parser.h (LOG_DEBUG, LOG_INFO, etc.).
 * @return O LogLevel correspondente.
 */
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

/**
 * @brief Preenche entry->message com description ou fallback se description estiver vazio.
 * @param entry       Entrada de log cujo campo message será preenchido.
 * @param description Texto preferencial (pode ser NULL ou vazio).
 * @param fallback    Texto alternativo se description não estiver disponível.
 */
static void copy_description_or_fallback(LogEntry *entry, const char *description, const char *fallback) {
    const char *text = (description && description[0] != '\0') ? description : fallback;
    if (text == NULL || text[0] == '\0') text = "Evento critico sem descricao";

    strncpy(entry->message, text, MSG_LEN - 1);
    entry->message[MSG_LEN - 1] = '\0';
}

/**
 * @brief Extrai o primeiro endereço IPv4 válido encontrado na string @p s.
 * @param s   String a pesquisar.
 * @param out Buffer de saída de tamanho IP_LEN onde o IP é escrito.
 * @return 0 se encontrou um IP válido, -1 caso contrário.
 */
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

/**
 * @brief Salta a prioridade no formato syslog (<N>) no início de uma linha.
 * @param line Linha de syslog.
 * @return Apontador para o carácter após ">", ou para @p line se não houver prioridade.
 */
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

/**
 * @brief Verifica heuristicamente se uma linha começa com um timestamp syslog ("Mmm DD").
 * @param line Linha a verificar.
 * @return 1 se parecer ter timestamp syslog, 0 caso contrário.
 */
static int looks_like_syslog_timestamp(const char *line) {
    return strlen(line) > 15 &&
           isalpha((unsigned char)line[0]) &&
           isalpha((unsigned char)line[1]) &&
           isalpha((unsigned char)line[2]) &&
           line[3] == ' ' &&
           (isdigit((unsigned char)line[4]) || line[4] == ' ');
}

/**
 * @brief Define o modo de análise global a partir de uma string CLI.
 * @param mode_str "security", "performance", "traffic" ou "full".
 * @return 0 em sucesso, -1 se a string for inválida.
 */
int parser_set_mode_from_string(const char *mode_str) {
    if (!mode_str) return -1;
    if (strcasecmp(mode_str, "security") == 0) g_mode = MODE_SECURITY;
    else if (strcasecmp(mode_str, "performance") == 0) g_mode = MODE_PERFORMANCE;
    else if (strcasecmp(mode_str, "traffic") == 0) g_mode = MODE_TRAFFIC;
    else if (strcasecmp(mode_str, "full") == 0) g_mode = MODE_FULL;
    else return -1;
    return 0;
}

/**
 * @brief Converte uma string de nível textual em LogLevel (case-insensitive).
 * @param s String a converter (ex: "ERROR", "warn", "CRIT").
 * @return LogLevel correspondente, ou LEVEL_UNKNOWN se não reconhecido.
 */
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

/**
 * @brief Deteta o formato de uma linha de log por heurística.
 * @param line Linha de texto a analisar.
 * @return FORMAT_JSON, FORMAT_APACHE, FORMAT_NGINX_ERROR, FORMAT_SYSLOG, ou FORMAT_UNKNOWN.
 */
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

/**
 * @brief Parseia uma linha de log e preenche uma LogEntry.
 * @param line   Linha de texto (sem newline).
 * @param format Formato da linha (detectado previamente com detect_format).
 * @param entry  Estrutura de saída preenchida em caso de sucesso.
 * @return 0 em sucesso, -1 se a linha não for válida ou não corresponder ao modo activo.
 */
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
/**
 * @brief Acumula os dados de uma LogEntry nas métricas do worker.
 * @param m Acumulador de métricas (modificado in-place).
 * @param e Entrada de log a acumular (contadores, IP, alertas).
 */
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

/**
 * @brief Inicializa uma estrutura Metrics a zeros.
 * @param m Estrutura a inicializar.
 */
void init_metrics(Metrics *m) {
    memset(m, 0, sizeof(Metrics));
}

/**
 * @brief Funde as métricas de @p src em @p dst com deduplicação de IPs.
 * @param dst Acumulador global (modificado in-place).
 * @param src Métricas locais do worker a fundir.
 */
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
