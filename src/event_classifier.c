/**
 * @file event_classifier.c
 * @brief Classificação de eventos de log por tipo e severidade.
 *
 * @details
 * Este módulo é o segundo passo do pipeline de análise: recebe as estruturas
 * preenchidas pelos parsers (`log_parser.c`) e decide:
 *
 *  1. **Que tipo de evento é** — usando o campo de bits `event_types`:
 *     - `EVENT_SECURITY`    (0x01) — ataques, falhas de autenticação, scanners
 *     - `EVENT_PERFORMANCE` (0x02) — erros de servidor, timeouts, OOM
 *     - `EVENT_TRAFFIC`     (0x04) — pedidos HTTP, conexões de rede
 *     - `EVENT_ERROR`       (0x08) — erros genéricos (nível ERROR ou superior)
 *     - `EVENT_NORMAL`      (0x10) — eventos sem classificação específica
 *     Um evento pode pertencer a múltiplos tipos (campo de bits).
 *
 *  2. **Qual a severidade** — escala interna de 0 a 4:
 *     - 0 → INFO     (operação normal, sem impacto)
 *     - 1 → LOW      (anomalia menor)
 *     - 2 → MEDIUM   (impacto moderado)
 *     - 3 → HIGH     (impacto elevado, requer atenção)
 *     - 4 → CRITICAL (incidente grave, gera alerta imediato)
 *
 *  3. **Uma descrição textual** — gravada em `event->description` para ser
 *     apresentada nos alertas ao utilizador.
 *
 * Existe uma função de classificação por formato:
 *  - `classify_apache_event`  — para `ApacheLogEntry`
 *  - `classify_json_event`    — para `JSONLogEntry`
 *  - `classify_syslog_event`  — para `SyslogEntry`
 *  - `classify_nginx_event`   — para `NginxErrorEntry`
 *
 * Funções auxiliares públicas:
 *  - `event_matches_mode`     — filtra um evento pelo modo de análise activo
 *  - `get_event_type_name`    — converte o campo de bits em string legível
 *  - `get_severity_name`      — converte a severidade numérica em string
 */

// src/event_classifier.c
#define _GNU_SOURCE
#include "event_classifier.h"
#include <string.h>
#include <strings.h>
#include <stdarg.h>

/**
 * @brief Chama mktime() sem modificar a estrutura tm de entrada (que é const).
 *
 * @details `mktime()` pode modificar a estrutura `struct tm` passada (por
 * exemplo, normalizando campos fora de intervalo). Como os campos de entrada
 * são `const`, esta função cria uma cópia local antes de chamar `mktime`.
 *
 * @param tm Ponteiro para uma estrutura tm de somente leitura.
 * @return Timestamp Unix correspondente, ou (time_t)-1 em caso de erro.
 */
static time_t safe_mktime(const struct tm* tm) {
    struct tm tmp = *tm; /* cópia local para mktime poder modificar */
    return mktime(&tmp);
}

// ============================================================================
// CLASSIFICAÇÃO APACHE - Detecta e classifica eventos de segurança/performa
// ============================================================================

/**
 * @brief Classifica um evento Apache por tipo e severidade.
 *
 * @details Analisa uma entrada Apache já parseada e detecta padrões de:
 *
 *  - **Segurança**: códigos 401/403 (acesso não autorizado), padrões de SQL
 *    Injection na URL (UNION SELECT, DROP TABLE, etc.), XSS (`<script>`,
 *    `javascript:`), path traversal (`../`), e user-agents de scanners
 *    conhecidos (nikto, sqlmap, nmap, masscan).
 *
 *  - **Performance**: respostas muito grandes (>10MB), erros 5xx (problemas
 *    de servidor), código 503 (sobrecarga / serviço indisponível).
 *
 *  - **Tráfego**: todos os pedidos são classificados como tráfego; pedidos
 *    POST/PUT/DELETE e respostas 404 têm marcação adicional.
 *
 * A severidade é actualizada pelo padrão mais grave encontrado (SQL Injection
 * e 503 são os mais críticos, com severidade 4).
 *
 * @param entry Entrada Apache preenchida por `parse_apache_log()`.
 * @param event Estrutura de saída com tipo, severidade e descrição.
 * @return Campo de bits `event_types` com os tipos detectados; 0 se parâmetros NULL.
 */
int classify_apache_event(const ApacheLogEntry* entry, ClassifiedEvent* event) {
    if (!entry || !event) return 0;

    /* Inicializar o evento e copiar os dados brutos para referência futura */
    memset(event, 0, sizeof(ClassifiedEvent));
    event->data.apache = *entry;
    /* Converter o timestamp para time_t (necessário para ordenação e expiração) */
    event->timestamp = safe_mktime(&entry->timestamp);

    int types = 0;
    event->severity = 0;  /* INFO por defeito — será sobrescrito se necessário */

    // ========== SECURITY ==========

    /* Status 401 (não autenticado) ou 403 (proibido) indicam tentativa de
     * acesso a recurso protegido — severidade MEDIUM */
    if (entry->status_code == 401 || entry->status_code == 403) {
        types |= EVENT_SECURITY;
        event->severity = 2;  /* MEDIUM */
        /* Truncar URL para evitar overflow no snprintf */
        char url_truncated[128];
        strncpy(url_truncated, entry->url, sizeof(url_truncated) - 1);
        url_truncated[sizeof(url_truncated) - 1] = '\0';

        snprintf(event->description, sizeof(event->description),
                 "Unauthorized access: %.100s from %.40s",
                 url_truncated, entry->ip);
    }

    /* SQL Injection patterns na URL — severidade CRITICAL (4)
     * Estes padrões indicam tentativa de manipular queries SQL via URL */
    if (strcasestr(entry->url, "' OR '") ||
        strcasestr(entry->url, "UNION SELECT") ||
        strcasestr(entry->url, "DROP TABLE") ||
        strcasestr(entry->url, "1=1") ||
        strcasestr(entry->url, "admin'--")) {
        types |= EVENT_SECURITY;
        event->severity = 4;  /* CRITICAL — SQLi pode comprometer a base de dados */
        snprintf(event->description, sizeof(event->description),
                 "SQL Injection attempt: %.220s", entry->url);
    }

    /* XSS (Cross-Site Scripting) — injecção de JavaScript na URL
     * Severidade HIGH (3): afecta outros utilizadores, não o servidor */
    if (strcasestr(entry->url, "<script") ||
        strcasestr(entry->url, "javascript:") ||
        strcasestr(entry->url, "onerror=") ||
        strcasestr(entry->url, "onload=")) {
        types |= EVENT_SECURITY;
        event->severity = 3;  /* HIGH */
        snprintf(event->description, sizeof(event->description),
                 "XSS attempt: %.230s", entry->url);
    }

    /* Path traversal — tentativa de aceder a ficheiros fora do webroot
     * "../" sobe um nível no sistema de ficheiros */
    if (strstr(entry->url, "../") || strstr(entry->url, "..\\")) {
        types |= EVENT_SECURITY;
        event->severity = 3;  /* HIGH */
        snprintf(event->description, sizeof(event->description),
                 "Path traversal: %.228s", entry->url);
    }

    /* User-agents de ferramentas de scanning automático
     * A presença destes strings indica reconhecimento activo do servidor */
    if (strcasestr(entry->user_agent, "nikto") ||
        strcasestr(entry->user_agent, "sqlmap") ||
        strcasestr(entry->user_agent, "nmap") ||
        strcasestr(entry->user_agent, "masscan")) {
        types |= EVENT_SECURITY;
        event->severity = 3;  /* HIGH */
        snprintf(event->description, sizeof(event->description),
                 "Scanner detected: %.230s", entry->user_agent);
    }

    // ========== PERFORMANCE ==========

    /* Respostas muito grandes (>10MB) podem indicar download abusivo ou DoS */
    if (entry->response_size > 10 * 1024 * 1024) {  /* > 10MB */
        types |= EVENT_PERFORMANCE;
        event->severity = 2;  /* MEDIUM */
        snprintf(event->description, sizeof(event->description),
                 "Large response: %ld bytes for %.180s",
                 entry->response_size, entry->url);
    }

    /* Erros 5xx indicam falha no servidor (não no cliente) */
    if (entry->status_code >= 500 && entry->status_code < 600) {
        types |= EVENT_PERFORMANCE | EVENT_ERROR; /* é tanto performance como erro */
        event->severity = 3;  /* HIGH */
        snprintf(event->description, sizeof(event->description),
                 "Server error %d: %.230s", entry->status_code, entry->url);
    }

    /* Status 503 (Service Unavailable) — servidor sobrecarregado ou em manutenção */
    if (entry->status_code == 503) {
        types |= EVENT_PERFORMANCE;
        event->severity = 4;  /* CRITICAL — o serviço está inacessível */
        snprintf(event->description, sizeof(event->description),
                 "Service unavailable: possible overload");
    }

    // ========== TRAFFIC ==========

    /* Todo o pedido HTTP constitui tráfego, independentemente do resultado */
    types |= EVENT_TRAFFIC;

    /* 404 Not Found: recurso não existe — relevante para análise de tráfego
     * (pode indicar links quebrados ou scanning de URLs) */
    if (entry->status_code == 404) {
        types |= EVENT_TRAFFIC;
        event->severity = 1;  /* LOW */
        /* Só preencher a descrição se nenhum padrão de segurança a preencheu */
        if (event->description[0] == '\0') {
            snprintf(event->description, sizeof(event->description),
                     "Not found: %.240s", entry->url);
        }
    }

    /* Métodos modificadores (POST/PUT/DELETE) — relevantes para auditoria */
    if (strcmp(entry->method, "POST") == 0 ||
        strcmp(entry->method, "PUT") == 0 ||
        strcmp(entry->method, "DELETE") == 0) {
        types |= EVENT_TRAFFIC;
    }

    /* Descrição genérica de fallback para pedidos normais sem padrão específico */
    if (event->description[0] == '\0') {
        snprintf(event->description, sizeof(event->description),
                 "%.15s %.220s - Status %d",
                 entry->method, entry->url, entry->status_code);
    }

    event->event_types = types;
    return types;
}

// ============================================================================
// CLASSIFICAÇÃO JSON
// ============================================================================

/**
 * @brief Classifica um evento de log JSON por tipo e severidade.
 *
 * @details Para logs JSON, a severidade é derivada directamente do campo
 * `level` do log (DEBUG=0 ... CRITICAL=4), sem heurísticas adicionais.
 *
 * Padrões detectados:
 *  - **Segurança**: mensagens de falha de autenticação/autorização, serviços
 *    de autenticação/firewall, rate limiting.
 *  - **Performance**: mensagens de timeout/slow query/latência, serviços de
 *    base de dados (postgres, mysql, redis), mensagens de CPU/memória.
 *  - **Tráfego**: serviços de API/gateway/proxy.
 *  - **Erro**: qualquer log com nível >= LOG_ERROR.
 *
 * @param entry Entrada JSON preenchida por `parse_json_log()`.
 * @param event Estrutura de saída com tipo, severidade e descrição.
 * @return Campo de bits `event_types` com os tipos detectados; 0 se parâmetros NULL.
 */
int classify_json_event(const JSONLogEntry* entry, ClassifiedEvent* event) {
    if (!entry || !event) return 0;

    memset(event, 0, sizeof(ClassifiedEvent));
    event->data.json = *entry;
    event->timestamp = safe_mktime(&entry->timestamp);

    int types = 0;

    /* Para JSON, o nível de log já é a severidade directamente
     * (DEBUG=0, INFO=1, WARN=2, ERROR=3, CRITICAL=4) */
    event->severity = entry->level;

    // ========== SECURITY ==========

    /* Palavras-chave de falha de autenticação/autorização na mensagem */
    if (strcasestr(entry->message, "authentication failed") ||
        strcasestr(entry->message, "invalid credentials") ||
        strcasestr(entry->message, "unauthorized") ||
        strcasestr(entry->message, "access denied") ||
        strcasestr(entry->message, "permission denied")) {
        types |= EVENT_SECURITY;
        event->severity = 3;  /* HIGH — falha de acesso é sempre relevante */
    }

    /* Serviços de segurança conhecidos elevam a prioridade do evento */
    if (strcasestr(entry->service, "auth") ||
        strcasestr(entry->service, "security") ||
        strcasestr(entry->service, "firewall")) {
        types |= EVENT_SECURITY;
    }

    // ========== PERFORMANCE ==========

    /* Palavras-chave que indicam degradação de performance:
     * "took X ms" é um padrão comum em logs de query lenta */
    /* Corrigir precedência de operadores com parênteses */
    if (strcasestr(entry->message, "timeout") ||
        strcasestr(entry->message, "slow query") ||
        strcasestr(entry->message, "high latency") ||
        strcasestr(entry->message, "connection pool exhausted") ||
        strcasestr(entry->message, "out of memory") ||
        strcasestr(entry->message, "cpu") ||
        (strcasestr(entry->message, "took") && strcasestr(entry->message, "ms"))) {
        types |= EVENT_PERFORMANCE;
    }

    /* Serviços de persistência e cache são relevantes para performance */
    if (strcasestr(entry->service, "database") ||
        strcasestr(entry->service, "cache") ||
        strcasestr(entry->service, "redis") ||
        strcasestr(entry->service, "postgres") ||
        strcasestr(entry->service, "mysql")) {
        types |= EVENT_PERFORMANCE;
    }

    // ========== TRAFFIC ==========

    /* Serviços de API, gateway e proxy lidam directamente com tráfego */
    if (strcasestr(entry->service, "api") ||
        strcasestr(entry->service, "gateway") ||
        strcasestr(entry->service, "proxy")) {
        types |= EVENT_TRAFFIC;
    }

    /* Rate limiting é tanto tráfego (volume excessivo) como segurança
     * (pode indicar ataque de força bruta ou abuso da API) */
    if (strcasestr(entry->message, "rate limit") ||
        strcasestr(entry->message, "throttled")) {
        types |= EVENT_TRAFFIC | EVENT_SECURITY;
    }

    // ========== ERROR ==========

    /* Qualquer log com nível ERROR ou superior é classificado como erro */
    if (entry->level >= LOG_ERROR) {
        types |= EVENT_ERROR;
    }

    /* Descrição incluindo serviço, nível e mensagem (truncar para evitar overflow) */
    snprintf(event->description, sizeof(event->description),
             "[%.60s] %s: %.160s", entry->service,
             get_severity_name(entry->level), entry->message);

    event->event_types = types;
    return types;
}

// ============================================================================
// CLASSIFICAÇÃO SYSLOG
// ============================================================================

/**
 * @brief Classifica um evento syslog por tipo e severidade.
 *
 * @details A severidade base é derivada dos bits 0-2 do campo `priority`
 * (facility × 8 + severity), seguindo a escala syslog (0=emerg ... 7=debug),
 * que é invertida em relação à escala interna (0=info ... 4=critical).
 *
 * Os flags `is_auth_failure`, `is_sudo_attempt` e `is_firewall_block`
 * pré-computados pelo parser são usados aqui para classificação rápida
 * sem nova pesquisa de strings.
 *
 * Padrões adicionais detectados:
 *  - **Segurança**: sshd/sudo/pam/iptables, "Failed password", "invalid user",
 *    sudo com falha de autenticação (eleva para CRITICAL).
 *  - **Performance**: kernel panic, OOM killer, segmentation fault, crash.
 *  - **Tráfego**: serviços nginx/apache/http, mensagens de conexão.
 *
 * @param entry Entrada syslog preenchida por `parse_syslog()`.
 * @param event Estrutura de saída com tipo, severidade e descrição.
 * @return Campo de bits `event_types` com os tipos detectados; 0 se parâmetros NULL.
 */
int classify_syslog_event(const SyslogEntry* entry, ClassifiedEvent* event) {
    if (!entry || !event) return 0;

    memset(event, 0, sizeof(ClassifiedEvent));
    event->data.syslog = *entry;
    event->timestamp = safe_mktime(&entry->timestamp);

    int types = 0;

    /* Extrair os 3 bits menos significativos do campo de prioridade:
     * priority = facility * 8 + severity
     * facility identifica o subsistema (kern=0, auth=4, cron=9, etc.)
     * severity vai de 0 (emerg) a 7 (debug) — escala inversa à interna */
    int severity_level = entry->priority & 0x07;  /* Últimos 3 bits = severity syslog */

    /* Mapear a escala syslog (0=pior) para a escala interna (4=pior) */
    if (severity_level <= 1) event->severity = 4;      /* emerg(0)/alert(1) -> CRITICAL */
    else if (severity_level <= 3) event->severity = 3; /* crit(2)/err(3)    -> HIGH     */
    else if (severity_level <= 4) event->severity = 2; /* warning(4)        -> MEDIUM   */
    else if (severity_level <= 6) event->severity = 1; /* notice(5)/info(6) -> LOW      */
    else event->severity = 0;                          /* debug(7)          -> INFO     */

    // ========== SECURITY ==========

    /* Flag pré-calculada pelo parser: falha de autenticação PAM/sshd */
    if (entry->is_auth_failure) {
        types |= EVENT_SECURITY;
        event->severity = 3;  /* HIGH — falha de autenticação é sempre relevante */
        snprintf(event->description, sizeof(event->description),
                 "Auth failure: %.228s", entry->message);
    }

    /* Flag pré-calculada: o serviço é sudo (escalonamento de privilégios) */
    if (entry->is_sudo_attempt) {
        types |= EVENT_SECURITY;
        /* Sudo falhado é mais grave — pode indicar ataque de força bruta */
        if (strcasestr(entry->message, "authentication failure") ||
            strcasestr(entry->message, "incorrect password")) {
            event->severity = 4;  /* CRITICAL — tentativa falhada de obter root */
        }
    }

    /* Flag pré-calculada: pacote bloqueado pelo firewall (REJECT/DROP) */
    if (entry->is_firewall_block) {
        types |= EVENT_SECURITY;
        event->severity = 2;  /* MEDIUM — tráfego bloqueado é esperado em firewalls */
    }

    /* Serviços de segurança do sistema: sempre relevantes para segurança */
    if (strcasecmp(entry->service, "sshd") == 0 ||    /* SSH daemon */
        strcasecmp(entry->service, "sudo") == 0 ||    /* escalamento de privilégios */
        strcasecmp(entry->service, "pam") == 0 ||     /* Pluggable Authentication Modules */
        strcasestr(entry->service, "firewall") ||     /* qualquer serviço de firewall */
        strcasestr(entry->service, "iptables")) {     /* regras de firewall Linux */
        types |= EVENT_SECURITY;
    }

    /* Tentativas repetidas de password incorrecta — possível brute force */
    if (strcasestr(entry->message, "Failed password") ||
        strcasestr(entry->message, "invalid user")) {
        types |= EVENT_SECURITY;
        event->severity = 4;  /* CRITICAL — indicador forte de ataque de força bruta */
    }

    // ========== PERFORMANCE ==========

    /* Kernel panic: o sistema operativo encontrou um erro irrecuperável */
    if (strcasestr(entry->message, "kernel panic") ||
        strcasestr(entry->message, "out of memory") ||  /* OOM killer activo */
        strcasestr(entry->message, "OOM")) {
        types |= EVENT_PERFORMANCE | EVENT_ERROR;
        event->severity = 4;  /* CRITICAL — o sistema pode ficar indisponível */
    }

    /* Crashes de processo: segfault, core dump ou serviço "morreu" */
    if (strcasestr(entry->message, "segmentation fault") ||
        strcasestr(entry->message, "core dumped") ||
        strcasestr(entry->message, "died") ||
        strcasestr(entry->message, "crashed")) {
        types |= EVENT_PERFORMANCE | EVENT_ERROR;
        event->severity = 4;  /* CRITICAL — perda de serviço */
    }

    // ========== TRAFFIC ==========

    /* Serviços web geram tráfego de rede */
    if (strcasestr(entry->service, "nginx") ||
        strcasestr(entry->service, "apache") ||
        strcasestr(entry->service, "http")) {
        types |= EVENT_TRAFFIC;
    }

    /* Mensagens sobre conexões de rede são tráfego */
    if (strcasestr(entry->message, "connection") ||
        strcasestr(entry->message, "connect")) {
        types |= EVENT_TRAFFIC;
    }

    /* Descrição de fallback: serviço[pid]: mensagem */
    if (event->description[0] == '\0') {
        snprintf(event->description, sizeof(event->description),
                 "%.60s[%d]: %.180s", entry->service, entry->pid, entry->message);
    }

    event->event_types = types;
    return types;
}

// ============================================================================
// CLASSIFICAÇÃO NGINX
// ============================================================================

/**
 * @brief Classifica um evento do Nginx Error Log por tipo e severidade.
 *
 * @details A severidade é mapeada directamente a partir do nível Nginx:
 *  - NGINX_EMERG / NGINX_ALERT → CRITICAL (4)
 *  - NGINX_CRIT  / NGINX_ERROR → HIGH     (3)
 *  - NGINX_WARN                → MEDIUM   (2)
 *  - NGINX_NOTICE/ NGINX_INFO  → LOW      (1)
 *  - NGINX_DEBUG               → INFO     (0)
 *
 * Padrões detectados:
 *  - **Segurança**: "access forbidden", "denied", SSL/TLS errors.
 *  - **Performance**: upstream timed out, connection refused/reset,
 *    "no live upstreams" (todos os backends inacessíveis).
 *  - **Tráfego**: rate limiting, body too large; todos os erros nginx são tráfego.
 *
 * @param entry Entrada nginx preenchida por `parse_nginx_error()`.
 * @param event Estrutura de saída com tipo, severidade e descrição.
 * @return Campo de bits `event_types` com os tipos detectados; 0 se parâmetros NULL.
 */
int classify_nginx_event(const NginxErrorEntry* entry, ClassifiedEvent* event) {
    if (!entry || !event) return 0;

    memset(event, 0, sizeof(ClassifiedEvent));
    event->data.nginx = *entry;
    event->timestamp = safe_mktime(&entry->timestamp);

    int types = 0;

    /* Converter o nível Nginx para a escala interna de severidade (0-4) */
    if (entry->level == NGINX_EMERG || entry->level == NGINX_ALERT) {
        event->severity = 4;  /* CRITICAL — sistema inutilizável */
    } else if (entry->level == NGINX_CRIT || entry->level == NGINX_ERROR) {
        event->severity = 3;  /* HIGH — erro com impacto nos utilizadores */
    } else if (entry->level == NGINX_WARN) {
        event->severity = 2;  /* MEDIUM — potencial problema */
    } else if (entry->level == NGINX_NOTICE || entry->level == NGINX_INFO) {
        event->severity = 1;  /* LOW — informação relevante */
    } else {
        event->severity = 0;  /* INFO — debug, sem impacto */
    }

    // ========== SECURITY ==========

    /* Erros de acesso proibido — tentativa de aceder a recurso não autorizado */
    if (strcasestr(entry->message, "access forbidden") ||
        strcasestr(entry->message, "denied") ||
        strcasestr(entry->message, "not allowed")) {
        types |= EVENT_SECURITY;
        event->severity = 2;  /* MEDIUM */
    }

    /* Erros SSL/TLS — certificados inválidos ou falhas no handshake */
    if (strcasestr(entry->message, "SSL") ||
        strcasestr(entry->message, "certificate")) {
        types |= EVENT_SECURITY;
        /* Manter a severidade calculada pelo nível nginx */
    }

    // ========== PERFORMANCE ==========

    /* Problemas com servidores upstream (backends inacessíveis ou lentos) */
    if (strcasestr(entry->message, "upstream timed out") ||
        strcasestr(entry->message, "connection refused") ||   /* backend recusou conexão */
        strcasestr(entry->message, "connection reset") ||     /* conexão interrompida */
        strcasestr(entry->message, "no live upstreams")) {    /* todos os backends em baixo */
        types |= EVENT_PERFORMANCE;
        event->severity = 3;  /* HIGH — afecta disponibilidade do serviço */
    }

    /* Rate limiting activo — nginx está a limitar pedidos por excesso de tráfego */
    if (strcasestr(entry->message, "limiting requests")) {
        types |= EVENT_PERFORMANCE | EVENT_TRAFFIC;
        event->severity = 2;  /* MEDIUM */
    }

    /* Body demasiado grande — possível upload abusivo ou misconfiguraçao */
    if (strcasestr(entry->message, "too large")) {
        types |= EVENT_PERFORMANCE | EVENT_TRAFFIC;
        /* Manter a severidade actual */
    }

    // ========== TRAFFIC ==========

    /* Todos os erros nginx são relevantes para análise de tráfego */
    types |= EVENT_TRAFFIC;

    /* Descrição formatada com nível, mensagem e IP do cliente (truncar para evitar overflow) */
    snprintf(event->description, sizeof(event->description),
             "Nginx [%s]: %.180s (%.40s)",
             get_severity_name(event->severity),
             entry->message, entry->client_ip);

    event->event_types = types;
    return types;
}

// ============================================================================
// FUNÇÕES AUXILIARES
// ============================================================================

/**
 * @brief Verifica se um evento classificado deve ser incluído no modo activo.
 *
 * @details O modo de análise determina que categorias de eventos são
 * processadas. Em `MODE_FULL` todos os eventos passam; nos outros modos,
 * o campo de bits `event_types` é testado contra o modo usando AND binário.
 *
 * Os valores de `AnalysisMode` são definidos como potências de 2 para
 * coincidirem com os bits de `event_types`, pelo que um único AND basta.
 *
 * @param event Evento classificado com o campo `event_types` preenchido.
 * @param mode  Modo de análise activo (MODE_FULL, MODE_SECURITY, etc.).
 * @return `true` se o evento deve ser processado; `false` caso contrário.
 */
bool event_matches_mode(const ClassifiedEvent* event, AnalysisMode mode) {
    if (mode == MODE_FULL) return true;
    /* Testar se o evento tem o bit correspondente ao modo ligado */
    return (event->event_types & mode) != 0;
}

/**
 * @brief Acrescenta texto a um buffer de string de forma segura.
 *
 * @details Função interna usada por `get_event_type_name` para concatenar
 * os nomes dos tipos de evento. Usa `strlen` + `strncat` para garantir que
 * nunca escreve além do fim do buffer.
 *
 * @param buffer   Buffer de destino (terminado em '\0').
 * @param buf_size Tamanho total do buffer.
 * @param text     Texto a acrescentar.
 */
static void append_event_type(char* buffer, size_t buf_size, const char* text) {
    size_t len = strlen(buffer);
    /* Só escrever se ainda há espaço no buffer */
    if (len < buf_size - 1) {
        strncat(buffer, text, buf_size - len - 1);
    }
}

/**
 * @brief Converte o campo de bits de tipos de evento numa string legível.
 *
 * @details O chamador fornece o buffer de destino. Isto torna a função
 * reentrante e thread-safe (sem estado global ou buffers estáticos).
 * Os tipos são concatenados com espaço entre eles; o espaço final é removido.
 *
 * Exemplo de saída para `EVENT_SECURITY | EVENT_TRAFFIC`: `"SECURITY TRAFFIC"`
 *
 * @param event_type Campo de bits com os tipos de evento (EVENT_SECURITY, etc.).
 * @param buffer     Buffer de destino fornecido pelo chamador.
 * @param buf_size   Tamanho do buffer de destino.
 * @return Ponteiro para @p buffer; NULL se buffer for NULL ou buf_size for 0.
 */
const char* get_event_type_name(int event_type, char* buffer, size_t buf_size) {
    /* 1. O chamador fornece o buffer para eliminar memória estática partilhada entre threads. */
    if (buffer == NULL || buf_size == 0) {
        return NULL;
    }

    /* 2. Inicializar o buffer recebido torna a função reentrante e evita Race Conditions exigidas pelo POSIX. */
    buffer[0] = '\0';

    /* 3. Usar strncat com espaço restante impede Buffer Overflows mesmo com buffers pequenos. */
    if (event_type & EVENT_SECURITY) append_event_type(buffer, buf_size, "SECURITY ");
    if (event_type & EVENT_PERFORMANCE) append_event_type(buffer, buf_size, "PERFORMANCE ");
    if (event_type & EVENT_TRAFFIC) append_event_type(buffer, buf_size, "TRAFFIC ");
    if (event_type & EVENT_ERROR) append_event_type(buffer, buf_size, "ERROR ");
    if (event_type & EVENT_NORMAL) append_event_type(buffer, buf_size, "NORMAL ");

    /* 4. Remover o espaço final conserva a apresentação antiga sem recorrer a memória global. */
    size_t len = strlen(buffer);
    if (len > 0 && buffer[len-1] == ' ') {
        buffer[len-1] = '\0';
    }

    return buffer;
}

/**
 * @brief Converte um valor de severidade numérico (0-4) no nome textual.
 *
 * @details Retorna sempre uma string válida (sem NULL), incluindo "UNKNOWN"
 * para valores fora do intervalo esperado.
 *
 * @param severity Valor de severidade entre 0 e 4.
 * @return String estática com o nome da severidade.
 */
const char* get_severity_name(int severity) {
    switch (severity) {
        case 0: return "INFO";
        case 1: return "LOW";
        case 2: return "MEDIUM";
        case 3: return "HIGH";
        case 4: return "CRITICAL";
        default: return "UNKNOWN";
    }
}
