/**
 * @file parser.c
 * @brief Orquestrador central do pipeline de parsing de logs.
 *
 * @details
 * Este ficheiro implementa as quatro funções públicas que formam o núcleo do
 * sistema de análise de logs:
 *
 *  1. **detect_format()** — identifica o formato de uma linha de log (Apache,
 *     JSON, Syslog ou Nginx Error) sem ainda a processar completamente.
 *
 *  2. **parse_line()** — delega o parsing detalhado ao módulo `log_parser`,
 *     classifica o evento resultante via `event_classifier` e empacota o
 *     resultado numa estrutura `LogEntry` normalizada.
 *
 *  3. **update_metrics()** — acumula contadores e listas de IPs a partir de
 *     cada `LogEntry` válida, mantendo as métricas globais actualizadas.
 *
 *  4. **init_metrics()** — inicializa a estrutura `Metrics` a zeros antes
 *     de começar a processar qualquer linha.
 *
 * O módulo também expõe `parser_set_mode_from_string()` para configurar o
 * filtro de modo de análise (security / performance / traffic / full) e
 * `level_from_string()` para converter strings de nível textual em `LogLevel`.
 *
 * Fluxo de processamento por linha:
 * ```
 *   linha bruta
 *       │
 *       ▼
 *   detect_format()          ← heurísticas rápidas + fallback via parsers
 *       │
 *       ▼
 *   parse_line()
 *     ├─ parse_*_log()       ← log_parser.c (extrai campos estruturados)
 *     ├─ classify_*_event()  ← event_classifier.c (tipo + severidade)
 *     ├─ event_matches_mode()← filtra pelo modo de análise activo
 *     └─ preenche LogEntry   ← estrutura normalizada para update_metrics()
 *       │
 *       ▼
 *   update_metrics()         ← actualiza contadores, alertas e lista de IPs
 * ```
 */

#include "parser.h"

#include "event_classifier.h"
#include "log_parser.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Modo de análise global para filtrar eventos */
static AnalysisMode g_mode = MODE_FULL;

/**
 * @brief Converte uma severidade numérica (0–4) no LogLevel correspondente.
 *
 * @details A escala interna de severidade usada pelo classificador de eventos
 * é:
 *  - 0 → INFO (sem impacto relevante)
 *  - 1 → INFO (baixo impacto)
 *  - 2 → WARN (impacto moderado)
 *  - 3 → ERROR (impacto elevado)
 *  - 4 → CRITICAL (impacto crítico, gera alerta)
 *
 * @param severity Valor inteiro entre 0 e 4.
 * @return LogLevel correspondente; LEVEL_UNKNOWN para valores fora do intervalo.
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
 * @brief Converte um nível de log JSON (constante LOG_*) no LogLevel interno.
 *
 * @details Os logs JSON transportam o nível como inteiro usando as constantes
 * LOG_DEBUG, LOG_INFO, LOG_WARN, LOG_ERROR e LOG_CRITICAL definidas em
 * log_parser.h. Esta função faz o mapeamento directo para o enum LogLevel.
 *
 * @param json_level Constante LOG_* extraída da entrada JSON.
 * @return LogLevel correspondente; LEVEL_UNKNOWN se o valor não for reconhecido.
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
 * @brief Copia a descrição do evento para entry->message, com fallback seguro.
 *
 * @details Tenta usar @p description primeiro; se estiver vazia ou NULL usa
 * @p fallback. Se ambas forem vazias, escreve uma mensagem genérica de
 * protecção para garantir que o campo nunca fica vazio em eventos críticos.
 * Garante sempre terminação com '\0' dentro de MSG_LEN.
 *
 * @param entry      Estrutura LogEntry cujo campo message será preenchido.
 * @param description Texto preferencial (pode ser NULL ou vazio).
 * @param fallback   Texto alternativo usado quando description está vazio.
 */
static void copy_description_or_fallback(LogEntry *entry, const char *description, const char *fallback) {
    /* Usar description se não vazia; caso contrário usar fallback */
    const char *text = (description && description[0] != '\0') ? description : fallback;
    if (text == NULL || text[0] == '\0') text = "Evento critico sem descricao";

    /* Copiar com limite explícito para evitar buffer overflow */
    strncpy(entry->message, text, MSG_LEN - 1);
    entry->message[MSG_LEN - 1] = '\0'; /* garantir terminador nulo */
}

/**
 * @brief Extrai o primeiro endereço IPv4 válido encontrado numa string.
 *
 * @details Percorre @p s byte-a-byte procurando sequências de dígitos. Quando
 * encontra um dígito, tenta ler quatro octetos separados por pontos com
 * sscanf; valida que cada octeto está no intervalo [0, 255]. Esta abordagem
 * evita expressões regulares e funciona em contextos onde a IP pode estar
 * embebida num texto mais longo (e.g., mensagens syslog).
 *
 * @param s   String de entrada onde pesquisar.
 * @param out Buffer de saída com pelo menos IP_LEN bytes.
 * @return 0 em caso de sucesso; -1 se não for encontrado nenhum IPv4 válido.
 */
static int extract_ipv4(const char *s, char out[IP_LEN]) {
    if (!s) return -1;
    out[0] = '\0';

    /* Iterar sobre cada posição; só entrar no sscanf quando há um dígito */
    for (const char *p = s; *p; p++) {
        if (!isdigit((unsigned char)*p)) continue;

        int a = -1, b = -1, c = -1, d = -1;
        /* Tentar ler padrão d.d.d.d a partir da posição actual */
        if (sscanf(p, "%d.%d.%d.%d", &a, &b, &c, &d) != 4) continue;
        /* Validar que cada octeto é um valor IPv4 legal */
        if (a < 0 || a > 255 || b < 0 || b > 255 || c < 0 || c > 255 || d < 0 || d > 255) continue;

        snprintf(out, IP_LEN, "%d.%d.%d.%d", a, b, c, d);
        return 0;
    }

    return -1;
}

/**
 * @brief Avança o ponteiro para além de um campo de prioridade syslog (<nnn>).
 *
 * @details O RFC 3164 permite que as mensagens syslog comecem com uma
 * "PRI" no formato `<número>`. Esta função detecta e salta esse prefixo
 * para que o código de detecção de formato possa analisar o timestamp
 * a seguir sem ser perturbado pelos caracteres `<` e `>`.
 *
 * @param line Ponteiro para o início da linha de log.
 * @return Ponteiro para o primeiro caractere após `>`, ou @p line original
 *         se não existir campo de prioridade válido.
 */
static const char *skip_syslog_priority(const char *line) {
    const char *p = line;

    /* Verificar se existe o delimitador de abertura do campo PRI */
    if (*p != '<') {
        return p; /* sem campo PRI; retornar tal como está */
    }

    p++;
    /* O campo PRI deve conter pelo menos um dígito após '<' */
    if (!isdigit((unsigned char)*p)) {
        return line; /* formato inesperado; não avançar */
    }

    /* Saltar os dígitos do número de prioridade */
    while (isdigit((unsigned char)*p)) {
        p++;
    }

    /* Confirmar que termina com '>' e avançar para o conteúdo seguinte */
    return *p == '>' ? p + 1 : line;
}

/**
 * @brief Verifica se os primeiros bytes de uma linha correspondem ao timestamp
 *        no formato syslog ("Mmm DD HH:MM:SS").
 *
 * @details O formato syslog BSD (RFC 3164) usa um timestamp de 15 caracteres
 * como "Feb 13 10:23:45". Esta função verifica apenas os primeiros cinco
 * caracteres (três letras + espaço + dígito/espaço) como heurística rápida
 * antes de invocar um parser completo.
 *
 * @param line Linha de log (já sem prefixo PRI, se existia).
 * @return 1 se o padrão for compatível com syslog; 0 caso contrário.
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
 * @brief Configura o modo de análise global a partir de uma string.
 *
 * @details O modo de análise determina que categorias de eventos são aceites
 * por `parse_line()`. Modos suportados (insensível a maiúsculas/minúsculas):
 * - "security"    → apenas eventos do tipo EVENT_SECURITY
 * - "performance" → apenas eventos do tipo EVENT_PERFORMANCE
 * - "traffic"     → apenas eventos do tipo EVENT_TRAFFIC
 * - "full"        → todos os eventos (sem filtragem)
 *
 * A variável global g_mode é actualizada directamente; não há concorrência
 * porque este setter deve ser chamado antes de qualquer thread de parsing.
 *
 * @param mode_str String com o nome do modo desejado.
 * @return 0 em caso de sucesso; -1 se a string for NULL ou inválida.
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
 * @brief Converte uma string textual de nível de log no enum LogLevel.
 *
 * @details A conversão é insensível a maiúsculas/minúsculas. São aceites as
 * strings padrão de vários sistemas:
 *  - "DEBUG"
 *  - "INFO", "NOTICE" → LEVEL_INFO
 *  - "WARN", "WARNING" → LEVEL_WARN
 *  - "ERROR", "ERR" → LEVEL_ERROR
 *  - "CRIT", "CRITICAL", "ALERT", "EMERG" → LEVEL_CRITICAL
 *
 * @param s String com o nível de log (pode ser NULL).
 * @return LogLevel correspondente; LEVEL_UNKNOWN se não for reconhecido.
 */
LogLevel level_from_string(const char *s) {
    if (s == NULL) return LEVEL_UNKNOWN;

    /* Converter para maiúsculas num buffer local para comparação */
    char buf[16];
    int i;
    for (i = 0; i < 15 && s[i]; i++) buf[i] = (char)toupper((unsigned char)s[i]);
    buf[i] = '\0';

    if (strcmp(buf, "DEBUG") == 0) return LEVEL_DEBUG;
    if (strcmp(buf, "INFO") == 0) return LEVEL_INFO;
    if (strcmp(buf, "NOTICE") == 0) return LEVEL_INFO;    /* syslog NOTICE ~ INFO */
    if (strcmp(buf, "WARN") == 0) return LEVEL_WARN;
    if (strcmp(buf, "WARNING") == 0) return LEVEL_WARN;
    if (strcmp(buf, "ERROR") == 0) return LEVEL_ERROR;
    if (strcmp(buf, "ERR") == 0) return LEVEL_ERROR;      /* alias curto do syslog */
    if (strcmp(buf, "CRIT") == 0) return LEVEL_CRITICAL;
    if (strcmp(buf, "CRITICAL") == 0) return LEVEL_CRITICAL;
    if (strcmp(buf, "ALERT") == 0) return LEVEL_CRITICAL; /* syslog ALERT ≥ CRITICAL */
    if (strcmp(buf, "EMERG") == 0) return LEVEL_CRITICAL; /* syslog EMERG = pior nível */
    return LEVEL_UNKNOWN;
}

/**
 * @brief Detecta o formato de uma linha de log sem a processar completamente.
 *
 * @details A detecção segue uma sequência de heurísticas por ordem de custo
 * computacional crescente:
 *
 *  1. **JSON**: linha começa com '{'.
 *  2. **Nginx Error**: começa com 4 dígitos seguidos de '/' (data YYYY/MM/DD).
 *  3. **Syslog**: começa com timestamp do tipo "Mmm DD ..." (após saltar campo PRI).
 *  4. **Apache**: começa com um endereço IPv4 (três pontos em sequência seguidos de espaço).
 *  5. **Fallback**: tenta cada parser concreto em ordem; aceita o primeiro que não falhe.
 *
 * O fallback garante que linhas corrompidas ou com cabeçalhos atípicos
 * ainda têm hipótese de ser classificadas correctamente.
 *
 * @param line Linha de texto terminada em '\0' (não pode ser NULL).
 * @return LogFormat detectado; FORMAT_UNKNOWN se nenhuma heurística funcionar.
 */
LogFormat detect_format(const char *line) {
    if (line == NULL || *line == '\0') return FORMAT_UNKNOWN;

    /* Heurística 1: JSON — linha começa obrigatoriamente com '{' */
    if (line[0] == '{') return FORMAT_JSON;

    /* Heurística 2: Nginx Error Log — timestamp no formato YYYY/MM/DD
     * (quatro dígitos imediatamente seguidos de '/') */
    if (strlen(line) > 20 &&
        isdigit((unsigned char)line[0]) &&
        isdigit((unsigned char)line[1]) &&
        isdigit((unsigned char)line[2]) &&
        isdigit((unsigned char)line[3]) && /* quatro dígitos = ano */
        line[4] == '/') {                  /* barra a separar ano/mês/dia */
        return FORMAT_NGINX_ERROR;
    }

    /* Heurística 3: Syslog — campo PRI opcional seguido de timestamp "Mmm DD ..." */
    if (looks_like_syslog_timestamp(skip_syslog_priority(line))) {
        return FORMAT_SYSLOG;
    }

    /* Heurística 4: Apache — começa com endereço IPv4 (ex.: "192.168.1.1 ...") */
    {
        const char *p = line;
        int dots = 0;
        /* Contar pontos enquanto os caracteres são dígitos ou pontos */
        while (*p && (isdigit((unsigned char)*p) || *p == '.')) {
            if (*p == '.') dots++;
            p++;
        }
        /* Exactamente três pontos (quatro octetos) seguidos de espaço = IP válido */
        if (dots == 3 && *p == ' ') return FORMAT_APACHE;
    }

    /* Fallback: tentar os parsers canonicos em ordem.
     * Para não descartar linhas corrompidas ou com prefixos atípicos,
     * invoca-se cada parser completo; o primeiro que retornar 0 ganha. */
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
 * @brief Processa uma linha de log no formato indicado e preenche uma LogEntry.
 *
 * @details Esta função constitui o segundo passo do pipeline:
 *  1. Invoca o parser específico do formato (log_parser.c) para extrair campos.
 *  2. Classifica o evento com o classificador correspondente (event_classifier.c),
 *     obtendo tipo (SECURITY/PERFORMANCE/TRAFFIC/ERROR) e severidade (0–4).
 *  3. Verifica se o evento corresponde ao modo de análise activo (g_mode);
 *     se não corresponder, descarta a linha retornando -1.
 *  4. Normaliza o resultado para a estrutura LogEntry, que é independente do
 *     formato e pode ser processada uniformemente por update_metrics().
 *
 * O processo é idêntico para os quatro formatos; a diferença está apenas nos
 * tipos de estrutura usados nos passos 1–2.
 *
 * @param line   Linha de texto terminada em '\0'.
 * @param format Formato detectado por detect_format().
 * @param entry  Estrutura de saída a preencher.
 * @return 0 em caso de sucesso; -1 se o parsing falhar, o modo não coincidir,
 *         ou os parâmetros forem NULL.
 */
int parse_line(const char *line, LogFormat format, LogEntry *entry) {
    if (line == NULL || entry == NULL) return -1;

    /* Inicializar a entrada com valores neutros antes de preencher */
    entry->format = format;
    entry->level = LEVEL_UNKNOWN;
    entry->http_status = 0;
    entry->ip[0] = '\0';
    entry->message[0] = '\0';

    /* Evento classificado (tipo + severidade + descrição) */
    ClassifiedEvent event;

    switch (format) {
        case FORMAT_APACHE: {
            ApacheLogEntry a;
            /* Passo 1: tentar extrair campos estruturados do log Apache */
            if (parse_apache_log(line, &a) != 0) return -1;
            /* Passo 2: classificar o evento (tipo + severidade) */
            (void)classify_apache_event(&a, &event);
            /* Passo 3: descartar se o evento não pertence ao modo activo */
            if (!event_matches_mode(&event, g_mode)) return -1;

            /* Passo 4: normalizar para LogEntry */
            entry->level = level_from_severity(event.severity);
            entry->http_status = a.status_code;
            strncpy(entry->ip, a.ip, IP_LEN - 1);
            entry->ip[IP_LEN - 1] = '\0';
            copy_description_or_fallback(entry, event.description, a.url);
            return 0;
        }
        /* O processo repete-se para os outros formatos:
         * parsear → classificar → verificar modo → normalizar */
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
            /* Syslog não tem campo IP dedicado; tentar extrair da mensagem */
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

/**
 * @brief Actualiza os contadores de métricas com base numa LogEntry processada.
 *
 * @details Esta função é chamada após cada `parse_line()` bem-sucedido e
 * acumula estatísticas em quatro áreas:
 *
 *  1. **Contagem por nível**: incrementa o contador correspondente ao nível
 *     da entrada (DEBUG/INFO/WARN/ERROR/CRITICAL).
 *
 *  2. **Contagem de erros HTTP**: incrementa count_5xx (≥500) ou count_4xx
 *     (≥400) quando o log tem um código HTTP associado.
 *
 *  3. **Alertas**: para entradas de nível ERROR ou CRITICAL, copia a mensagem
 *     para o array de alertas (até MAX_ALERTS entradas).
 *
 *  4. **Lista de IPs**: mantém uma tabela de endereços únicos com contagem
 *     de ocorrências (até MAX_IPS endereços distintos).
 *
 * @param m Estrutura Metrics a actualizar (deve ter sido inicializada por init_metrics()).
 * @param e Entrada de log normalizada produzida por parse_line().
 */
void update_metrics(Metrics *m, const LogEntry *e) {
    /* Incrementar o total de linhas processadas com sucesso */
    m->total_lines++;

    /* Actualizar o contador do nível de log correspondente */
    switch (e->level) {
        case LEVEL_DEBUG: m->count_debug++; break;
        case LEVEL_INFO: m->count_info++; break;
        case LEVEL_WARN: m->count_warn++; break;
        case LEVEL_ERROR: m->count_error++; break;
        case LEVEL_CRITICAL: m->count_critical++; break;
        default: break;
    }

    /* Contagem de respostas HTTP de erro (apenas para logs com código HTTP) */
    if (e->http_status >= 500) m->count_5xx++;
    else if (e->http_status >= 400) m->count_4xx++;

    /* Armazenar alertas para eventos ERROR/CRITICAL (até MAX_ALERTS) */
    if ((e->level == LEVEL_ERROR || e->level == LEVEL_CRITICAL) && m->num_alerts < MAX_ALERTS) {
        const char *alert = e->message[0] != '\0' ? e->message : "Evento critico sem descricao";
        strncpy(m->alerts[m->num_alerts], alert, ALERT_LEN - 1);
        m->alerts[m->num_alerts][ALERT_LEN - 1] = '\0';
        m->num_alerts++;
    }

    /* Actualizar tabela de IPs: incrementar contador se já existe,
     * ou adicionar novo IP se ainda houver espaço (até MAX_IPS) */
    if (e->ip[0] != '\0') {
        int found = 0;
        for (int i = 0; i < m->ip_num; i++) {
            if (strcmp(m->ip_list[i], e->ip) == 0) {
                m->ip_count[i]++; /* IP já conhecido — incrementar contagem */
                found = 1;
                break;
            }
        }
        if (!found && m->ip_num < MAX_IPS) {
            /* IP novo — registar e inicializar com 1 ocorrência */
            strncpy(m->ip_list[m->ip_num], e->ip, IP_LEN - 1);
            m->ip_list[m->ip_num][IP_LEN - 1] = '\0';
            m->ip_count[m->ip_num] = 1;
            m->ip_num++;
        }
    }
}

/**
 * @brief Inicializa a estrutura Metrics a zeros antes do início da análise.
 *
 * @details Usa memset para garantir que todos os contadores, arrays e campos
 * de texto ficam a zero/nulo. Deve ser chamada exactamente uma vez antes de
 * qualquer chamada a update_metrics().
 *
 * @param m Ponteiro para a estrutura Metrics a inicializar.
 */
void init_metrics(Metrics *m) {
    memset(m, 0, sizeof(Metrics));
}
