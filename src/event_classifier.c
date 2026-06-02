/**
 * @file event_classifier.c
 * @brief Classificação de eventos de log por tipo (segurança/performance/tráfego) e severidade.
 *
 * @details
 * Este módulo recebe entradas de log já parseadas e determina a que categorias
 * pertencem e qual a sua severidade, preenchendo uma estrutura ClassifiedEvent.
 *
 * Existem quatro classificadores, um por formato de log:
 *
 *  - **classify_apache_event()** — analisa URL, método, status code e user-agent
 *    para detectar ataques (SQLi, XSS, path traversal, scanners) e problemas de performance.
 *
 *  - **classify_json_event()** — usa o campo level e palavras-chave na mensagem/serviço
 *    para mapear eventos de aplicação a categorias.
 *
 *  - **classify_syslog_event()** — combina flags pré-calculadas pelo parser (is_auth_failure,
 *    is_sudo_attempt, is_firewall_block) com o campo de prioridade syslog (RFC 3164).
 *
 *  - **classify_nginx_event()** — mapeia o nível de severidade Nginx e palavras-chave
 *    na mensagem a categorias de segurança/performance/tráfego.
 *
 * A severidade interna usa a escala 0–4: INFO(0) < LOW(1) < MEDIUM(2) < HIGH(3) < CRITICAL(4).
 * O campo event_types é uma máscara de bits (OR de EVENT_SECURITY, EVENT_PERFORMANCE, etc.).
 */

#define _GNU_SOURCE
#include "event_classifier.h"
#include <string.h>
#include <strings.h>
#include <stdarg.h>

/**
 * @brief Cria uma cópia mutável de @p tm e invoca mktime(3) sobre ela.
 *
 * @details mktime(3) modifica a estrutura que recebe (normaliza campos fora de alcance
 * e preenche tm_wday/tm_yday). Como os parsers devolvem ponteiros const, é necessário
 * copiar antes de chamar mktime para evitar undefined behaviour numa área de memória
 * read-only.
 *
 * @param tm Ponteiro constante para a estrutura de tempo a converter.
 * @return time_t correspondente à data/hora representada por @p tm.
 */
static time_t safe_mktime(const struct tm *tm) {
    struct tm tmp = *tm; /* cópia local para que mktime(3) possa normalizar os campos */
    return mktime(&tmp);
}

/**
 * @brief Classifica uma entrada Apache detectando padrões de ataque e anomalias de performance.
 *
 * @details A classificação é aditiva por máscara de bits: um único pedido pode pertencer
 * simultaneamente a EVENT_SECURITY e EVENT_TRAFFIC. A severidade final é a mais alta
 * encontrada durante a análise; snprintf sobrescreve event->description com a descrição
 * do padrão mais crítico detectado.
 *
 * Padrões de segurança detectados (por ordem de severidade crescente):
 *  - Status 401/403 → acesso não autorizado (MEDIUM)
 *  - Path traversal (`../`) → leitura fora da raiz web (HIGH)
 *  - XSS (`<script>`, `javascript:`, handlers on*) (HIGH)
 *  - User-agents de scanners conhecidos (nikto, sqlmap, nmap, masscan) (HIGH)
 *  - SQL Injection (`UNION SELECT`, `DROP TABLE`, `' OR '`, etc.) (CRITICAL)
 *
 * @param entry Entrada Apache parseada pelo módulo log_parser.
 * @param event Estrutura de saída a preencher com tipo, severidade e descrição.
 * @return Máscara de bits dos tipos de evento detectados (combinação de EVENT_*).
 */
int classify_apache_event(const ApacheLogEntry *entry, ClassifiedEvent *event) {
    if (!entry || !event) return 0;

    memset(event, 0, sizeof(ClassifiedEvent));
    event->data.apache = *entry;
    event->timestamp   = safe_mktime(&entry->timestamp);

    int types = 0;
    event->severity = 0; /* INFO por defeito; actualizado a cada padrão encontrado */

    /* ── Segurança ── */

    if (entry->status_code == 401 || entry->status_code == 403) {
        types |= EVENT_SECURITY;
        event->severity = 2; /* MEDIUM */
        /* Truncar URL para não exceder o espaço disponível em description */
        char url_truncated[128];
        strncpy(url_truncated, entry->url, sizeof(url_truncated) - 1);
        url_truncated[sizeof(url_truncated) - 1] = '\0';
        snprintf(event->description, sizeof(event->description),
                 "Unauthorized access: %.100s from %.40s",
                 url_truncated, entry->ip);
    }

    /* Padrões de SQL Injection na URL — severidade máxima pois indica exfiltração activa */
    if (strcasestr(entry->url, "' OR '")      ||
        strcasestr(entry->url, "UNION SELECT") ||
        strcasestr(entry->url, "DROP TABLE")   ||
        strcasestr(entry->url, "1=1")          ||
        strcasestr(entry->url, "admin'--")) {
        types |= EVENT_SECURITY;
        event->severity = 4; /* CRITICAL */
        snprintf(event->description, sizeof(event->description),
                 "SQL Injection attempt: %.220s", entry->url);
    }

    if (strcasestr(entry->url, "<script")   ||
        strcasestr(entry->url, "javascript:") ||
        strcasestr(entry->url, "onerror=")  ||
        strcasestr(entry->url, "onload=")) {
        types |= EVENT_SECURITY;
        event->severity = 3; /* HIGH */
        snprintf(event->description, sizeof(event->description),
                 "XSS attempt: %.230s", entry->url);
    }

    if (strstr(entry->url, "../") || strstr(entry->url, "..\\")) {
        types |= EVENT_SECURITY;
        event->severity = 3; /* HIGH */
        snprintf(event->description, sizeof(event->description),
                 "Path traversal: %.228s", entry->url);
    }

    /* Ferramentas de reconhecimento activo identificadas pelo User-Agent */
    if (strcasestr(entry->user_agent, "nikto")   ||
        strcasestr(entry->user_agent, "sqlmap")  ||
        strcasestr(entry->user_agent, "nmap")    ||
        strcasestr(entry->user_agent, "masscan")) {
        types |= EVENT_SECURITY;
        event->severity = 3; /* HIGH */
        snprintf(event->description, sizeof(event->description),
                 "Scanner detected: %.230s", entry->user_agent);
    }

    /* ── Performance ── */

    /* Respostas maiores que 10 MB são anómalas e podem indicar DoS ou data leak */
    if (entry->response_size > 10 * 1024 * 1024) {
        types |= EVENT_PERFORMANCE;
        event->severity = 2; /* MEDIUM */
        snprintf(event->description, sizeof(event->description),
                 "Large response: %ld bytes for %.180s",
                 entry->response_size, entry->url);
    }

    if (entry->status_code >= 500 && entry->status_code < 600) {
        types |= EVENT_PERFORMANCE | EVENT_ERROR;
        event->severity = 3; /* HIGH */
        snprintf(event->description, sizeof(event->description),
                 "Server error %d: %.230s", entry->status_code, entry->url);
    }

    /* 503 Service Unavailable sobrepõe o 5xx genérico: indica sobrecarga crítica */
    if (entry->status_code == 503) {
        types |= EVENT_PERFORMANCE;
        event->severity = 4; /* CRITICAL */
        snprintf(event->description, sizeof(event->description),
                 "Service unavailable: possible overload");
    }

    /* ── Tráfego ── */

    /* Todos os pedidos HTTP são tráfego, independentemente do resultado */
    types |= EVENT_TRAFFIC;

    if (entry->status_code == 404) {
        types |= EVENT_TRAFFIC;
        event->severity = 1; /* LOW */
        if (event->description[0] == '\0') {
            snprintf(event->description, sizeof(event->description),
                     "Not found: %.240s", entry->url);
        }
    }

    /* Métodos modificadores (POST/PUT/DELETE) são relevantes para análise de tráfego */
    if (strcmp(entry->method, "POST")   == 0 ||
        strcmp(entry->method, "PUT")    == 0 ||
        strcmp(entry->method, "DELETE") == 0) {
        types |= EVENT_TRAFFIC;
    }

    /* Descrição genérica de fallback quando nenhum padrão específico foi detectado */
    if (event->description[0] == '\0') {
        snprintf(event->description, sizeof(event->description),
                 "%.15s %.220s - Status %d",
                 entry->method, entry->url, entry->status_code);
    }

    event->event_types = types;
    return types;
}

/**
 * @brief Classifica uma entrada de log JSON por nível, serviço e palavras-chave na mensagem.
 *
 * @details Os logs JSON transportam o nível de severidade directamente no campo level
 * (constantes LOG_DEBUG..LOG_CRITICAL). A classificação complementa esse nível com
 * análise semântica da mensagem e do nome do serviço emissor:
 *
 *  - Serviços `auth`/`security`/`firewall` → sempre EVENT_SECURITY.
 *  - Serviços `database`/`cache`/`redis`/`postgres`/`mysql` → sempre EVENT_PERFORMANCE.
 *  - Serviços `api`/`gateway`/`proxy` → sempre EVENT_TRAFFIC.
 *  - Palavras-chave de falha de autenticação → EVENT_SECURITY com severidade HIGH.
 *  - Palavras-chave de lentidão/timeout/OOM → EVENT_PERFORMANCE.
 *  - Rate limiting → EVENT_TRAFFIC | EVENT_SECURITY.
 *
 * @param entry Entrada JSON parseada pelo módulo log_parser.
 * @param event Estrutura de saída a preencher.
 * @return Máscara de bits dos tipos de evento detectados.
 */
int classify_json_event(const JSONLogEntry *entry, ClassifiedEvent *event) {
    if (!entry || !event) return 0;

    memset(event, 0, sizeof(ClassifiedEvent));
    event->data.json = *entry;
    event->timestamp = safe_mktime(&entry->timestamp);

    int types = 0;

    /* O campo level do JSON já codifica a severidade na escala interna (0=DEBUG … 4=CRITICAL) */
    event->severity = entry->level;

    /* ── Segurança ── */

    if (strcasestr(entry->message, "authentication failed") ||
        strcasestr(entry->message, "invalid credentials")  ||
        strcasestr(entry->message, "unauthorized")         ||
        strcasestr(entry->message, "access denied")        ||
        strcasestr(entry->message, "permission denied")) {
        types |= EVENT_SECURITY;
        event->severity = 3; /* HIGH: falha de autenticação sempre merece atenção */
    }

    if (strcasestr(entry->service, "auth")     ||
        strcasestr(entry->service, "security") ||
        strcasestr(entry->service, "firewall")) {
        types |= EVENT_SECURITY;
    }

    /* ── Performance ── */

    /*
     * A condição composta "took" + "ms" captura mensagens de latência do tipo
     * "query took 1500ms" sem false-positives para "took" isolado.
     */
    if (strcasestr(entry->message, "timeout")                   ||
        strcasestr(entry->message, "slow query")                ||
        strcasestr(entry->message, "high latency")              ||
        strcasestr(entry->message, "connection pool exhausted") ||
        strcasestr(entry->message, "out of memory")             ||
        strcasestr(entry->message, "cpu")                       ||
        (strcasestr(entry->message, "took") && strcasestr(entry->message, "ms"))) {
        types |= EVENT_PERFORMANCE;
    }

    if (strcasestr(entry->service, "database") ||
        strcasestr(entry->service, "cache")    ||
        strcasestr(entry->service, "redis")    ||
        strcasestr(entry->service, "postgres") ||
        strcasestr(entry->service, "mysql")) {
        types |= EVENT_PERFORMANCE;
    }

    /* ── Tráfego ── */

    if (strcasestr(entry->service, "api")     ||
        strcasestr(entry->service, "gateway") ||
        strcasestr(entry->service, "proxy")) {
        types |= EVENT_TRAFFIC;
    }

    /* Rate limiting implica tanto controlo de tráfego como política de segurança */
    if (strcasestr(entry->message, "rate limit") ||
        strcasestr(entry->message, "throttled")) {
        types |= EVENT_TRAFFIC | EVENT_SECURITY;
    }

    /* ── Erro ── */

    if (entry->level >= LOG_ERROR) {
        types |= EVENT_ERROR;
    }

    /* Truncar campos longos para não exceder sizeof(event->description) */
    snprintf(event->description, sizeof(event->description),
             "[%.60s] %s: %.160s", entry->service,
             get_severity_name(entry->level), entry->message);

    event->event_types = types;
    return types;
}

/**
 * @brief Classifica uma entrada syslog usando prioridade RFC 3164 e flags do parser.
 *
 * @details O campo priority do syslog codifica facilidade (facility) nos bits 3–7 e
 * severidade (severity) nos bits 0–2. Esta função extrai apenas os 3 bits de severidade
 * e mapeia a escala syslog (0=emerg, 7=debug) para a escala interna (0=INFO, 4=CRITICAL).
 *
 * Adicionalmente, o parser de syslog pré-calcula flags booleanas (is_auth_failure,
 * is_sudo_attempt, is_firewall_block) que permitem detecção directa sem re-análise
 * de texto. Estas flags têm precedência sobre a severidade derivada da prioridade.
 *
 * @param entry Entrada syslog parseada pelo módulo log_parser.
 * @param event Estrutura de saída a preencher.
 * @return Máscara de bits dos tipos de evento detectados.
 */
int classify_syslog_event(const SyslogEntry *entry, ClassifiedEvent *event) {
    if (!entry || !event) return 0;

    memset(event, 0, sizeof(ClassifiedEvent));
    event->data.syslog = *entry;
    event->timestamp   = safe_mktime(&entry->timestamp);

    int types = 0;

    /*
     * RFC 3164: priority = facility * 8 + severity.
     * Os 3 bits menos significativos isolam a severidade (0=emerg … 7=debug).
     * Mapear para escala interna (0=INFO … 4=CRITICAL):
     */
    int severity_level = entry->priority & 0x07;
    if      (severity_level <= 1) event->severity = 4; /* emerg/alert  → CRITICAL */
    else if (severity_level <= 3) event->severity = 3; /* crit/err     → HIGH     */
    else if (severity_level <= 4) event->severity = 2; /* warning      → MEDIUM   */
    else if (severity_level <= 6) event->severity = 1; /* notice/info  → LOW      */
    else                          event->severity = 0; /* debug        → INFO     */

    /* ── Segurança ── */

    if (entry->is_auth_failure) {
        types |= EVENT_SECURITY;
        event->severity = 3; /* HIGH: falha de autenticação mesmo que priority seja baixa */
        snprintf(event->description, sizeof(event->description),
                 "Auth failure: %.228s", entry->message);
    }

    if (entry->is_sudo_attempt) {
        types |= EVENT_SECURITY;
        /* Escalar para CRITICAL se a tentativa de sudo foi mal-sucedida */
        if (strcasestr(entry->message, "authentication failure") ||
            strcasestr(entry->message, "incorrect password")) {
            event->severity = 4; /* CRITICAL */
        }
    }

    if (entry->is_firewall_block) {
        types |= EVENT_SECURITY;
        event->severity = 2; /* MEDIUM */
    }

    /* Serviços de sistema habitualmente associados a eventos de segurança */
    if (strcasecmp(entry->service, "sshd")    == 0 ||
        strcasecmp(entry->service, "sudo")    == 0 ||
        strcasecmp(entry->service, "pam")     == 0 ||
        strcasestr(entry->service, "firewall")     ||
        strcasestr(entry->service, "iptables")) {
        types |= EVENT_SECURITY;
    }

    /* "Failed password" e "invalid user" são indicadores primários de brute force SSH */
    if (strcasestr(entry->message, "Failed password") ||
        strcasestr(entry->message, "invalid user")) {
        types |= EVENT_SECURITY;
        event->severity = 4; /* CRITICAL */
    }

    /* ── Performance ── */

    /* Eventos de kernel que indicam falha catastrófica do sistema */
    if (strcasestr(entry->message, "kernel panic") ||
        strcasestr(entry->message, "out of memory") ||
        strcasestr(entry->message, "OOM")) {
        types |= EVENT_PERFORMANCE | EVENT_ERROR;
        event->severity = 4; /* CRITICAL */
    }

    if (strcasestr(entry->message, "segmentation fault") ||
        strcasestr(entry->message, "core dumped")        ||
        strcasestr(entry->message, "died")               ||
        strcasestr(entry->message, "crashed")) {
        types |= EVENT_PERFORMANCE | EVENT_ERROR;
        event->severity = 4; /* CRITICAL */
    }

    /* ── Tráfego ── */

    if (strcasestr(entry->service, "nginx")  ||
        strcasestr(entry->service, "apache") ||
        strcasestr(entry->service, "http")) {
        types |= EVENT_TRAFFIC;
    }

    if (strcasestr(entry->message, "connection") ||
        strcasestr(entry->message, "connect")) {
        types |= EVENT_TRAFFIC;
    }

    /* Descrição de fallback quando nenhuma flag específica gerou descrição */
    if (event->description[0] == '\0') {
        snprintf(event->description, sizeof(event->description),
                 "%.60s[%d]: %.180s", entry->service, entry->pid, entry->message);
    }

    event->event_types = types;
    return types;
}

/**
 * @brief Classifica uma entrada Nginx Error Log mapeando nível e palavras-chave da mensagem.
 *
 * @details O Nginx usa uma escala de severidade própria (EMERG > ALERT > CRIT > ERROR >
 * WARN > NOTICE > INFO > DEBUG). Esta função converte essa escala para a interna (0–4)
 * e complementa com análise de palavras-chave na mensagem para categorização funcional.
 *
 * Todos os erros Nginx são considerados tráfego (types |= EVENT_TRAFFIC) porque
 * resultam de pedidos HTTP recebidos pelo servidor.
 *
 * @param entry Entrada Nginx parseada pelo módulo log_parser.
 * @param event Estrutura de saída a preencher.
 * @return Máscara de bits dos tipos de evento detectados.
 */
int classify_nginx_event(const NginxErrorEntry *entry, ClassifiedEvent *event) {
    if (!entry || !event) return 0;

    memset(event, 0, sizeof(ClassifiedEvent));
    event->data.nginx = *entry;
    event->timestamp  = safe_mktime(&entry->timestamp);

    int types = 0;

    /* Mapear nível de severidade Nginx para escala interna 0–4 */
    if      (entry->level == NGINX_EMERG || entry->level == NGINX_ALERT) event->severity = 4;
    else if (entry->level == NGINX_CRIT  || entry->level == NGINX_ERROR) event->severity = 3;
    else if (entry->level == NGINX_WARN)                                  event->severity = 2;
    else if (entry->level == NGINX_NOTICE || entry->level == NGINX_INFO)  event->severity = 1;
    else                                                                   event->severity = 0;

    /* ── Segurança ── */

    if (strcasestr(entry->message, "access forbidden") ||
        strcasestr(entry->message, "denied")           ||
        strcasestr(entry->message, "not allowed")) {
        types |= EVENT_SECURITY;
        event->severity = 2; /* MEDIUM */
    }

    /* Erros SSL/TLS podem indicar ataques de downgrade ou configuração inválida */
    if (strcasestr(entry->message, "SSL")         ||
        strcasestr(entry->message, "certificate")) {
        types |= EVENT_SECURITY;
    }

    /* ── Performance ── */

    /* Erros de upstream indicam problemas no backend ou na rede interna */
    if (strcasestr(entry->message, "upstream timed out")  ||
        strcasestr(entry->message, "connection refused")  ||
        strcasestr(entry->message, "connection reset")    ||
        strcasestr(entry->message, "no live upstreams")) {
        types |= EVENT_PERFORMANCE;
        event->severity = 3; /* HIGH */
    }

    if (strcasestr(entry->message, "limiting requests")) {
        types |= EVENT_PERFORMANCE | EVENT_TRAFFIC;
        event->severity = 2; /* MEDIUM */
    }

    if (strcasestr(entry->message, "too large")) {
        types |= EVENT_PERFORMANCE | EVENT_TRAFFIC;
    }

    /* ── Tráfego ── */

    /* Qualquer erro Nginx resulta de um pedido HTTP — é sempre tráfego */
    types |= EVENT_TRAFFIC;

    /* Truncar campos para não exceder sizeof(event->description) */
    snprintf(event->description, sizeof(event->description),
             "Nginx [%s]: %.180s (%.40s)",
             get_severity_name(event->severity),
             entry->message, entry->client_ip);

    event->event_types = types;
    return types;
}

/**
 * @brief Verifica se um evento classificado pertence ao modo de análise activo.
 *
 * @details MODE_FULL aceita todos os eventos sem filtragem. Para os outros modos,
 * a verificação é uma operação de AND bit a bit: o modo é uma máscara de bits do
 * mesmo tipo que event_types, pelo que basta verificar se há bits em comum.
 *
 * @param event Evento classificado a avaliar.
 * @param mode  Modo de análise activo (MODE_FULL, MODE_SECURITY, etc.).
 * @return true se o evento deve ser processado; false se deve ser descartado.
 */
bool event_matches_mode(const ClassifiedEvent *event, AnalysisMode mode) {
    if (mode == MODE_FULL) return true;
    return (event->event_types & mode) != 0;
}

/**
 * @brief Acrescenta @p text ao fim de @p buffer respeitando o limite @p buf_size.
 *
 * @details Função auxiliar que calcula o espaço restante antes de cada strncat para
 * evitar buffer overflow mesmo quando buf_size é pequeno. Usada exclusivamente por
 * get_event_type_name para construir a string de tipos de evento.
 *
 * @param buffer   Buffer de destino, já terminado em '\0'.
 * @param buf_size Tamanho total do buffer (incluindo o '\0').
 * @param text     String a concatenar.
 */
static void append_event_type(char *buffer, size_t buf_size, const char *text) {
    size_t len = strlen(buffer);
    if (len < buf_size - 1) {
        strncat(buffer, text, buf_size - len - 1);
    }
}

/**
 * @brief Constrói uma string legível com os nomes dos tipos de evento presentes na máscara.
 *
 * @details O chamador fornece o buffer de saída para tornar a função reentrante e
 * compatível com uso multi-thread (sem estado estático partilhado). O espaço final
 * após cada token é removido para manter a apresentação original.
 *
 * @param event_type Máscara de bits com os tipos de evento (combinação de EVENT_*).
 * @param buffer     Buffer fornecido pelo chamador onde escrever a string resultante.
 * @param buf_size   Tamanho do buffer em bytes.
 * @return Ponteiro para @p buffer, ou NULL se @p buffer for NULL ou @p buf_size for 0.
 */
const char *get_event_type_name(int event_type, char *buffer, size_t buf_size) {
    if (buffer == NULL || buf_size == 0) {
        return NULL;
    }

    buffer[0] = '\0'; /* inicializar antes das concatenações */

    if (event_type & EVENT_SECURITY)    append_event_type(buffer, buf_size, "SECURITY ");
    if (event_type & EVENT_PERFORMANCE) append_event_type(buffer, buf_size, "PERFORMANCE ");
    if (event_type & EVENT_TRAFFIC)     append_event_type(buffer, buf_size, "TRAFFIC ");
    if (event_type & EVENT_ERROR)       append_event_type(buffer, buf_size, "ERROR ");
    if (event_type & EVENT_NORMAL)      append_event_type(buffer, buf_size, "NORMAL ");

    /* Remover o espaço final inserido por append_event_type */
    size_t len = strlen(buffer);
    if (len > 0 && buffer[len - 1] == ' ') {
        buffer[len - 1] = '\0';
    }

    return buffer;
}

/**
 * @brief Converte uma severidade numérica (0–4) na string de etiqueta correspondente.
 *
 * @param severity Valor inteiro entre 0 e 4.
 * @return String estática com o nome da severidade; "UNKNOWN" para valores inválidos.
 */
const char *get_severity_name(int severity) {
    switch (severity) {
        case 0: return "INFO";
        case 1: return "LOW";
        case 2: return "MEDIUM";
        case 3: return "HIGH";
        case 4: return "CRITICAL";
        default: return "UNKNOWN";
    }
}
