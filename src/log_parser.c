/**
 * @file log_parser.c
 * @brief Parsers concretos para os quatro formatos de log suportados.
 *
 * @details
 * Este ficheiro implementa o parsing de baixo nível para cada formato de log,
 * sem recorrer a expressões regulares (mais portável e sem dependências externas).
 * Cada função extrai campos estruturados de uma linha de texto bruta e preenche
 * a estrutura correspondente.
 *
 * Formatos suportados e respectivas estruturas de saída:
 *
 *  - **Apache Combined Log** (`parse_apache_log`)  → `ApacheLogEntry`
 *    Formato: `IP - - [timestamp] "METHOD URL HTTP/ver" status size`
 *
 *  - **JSON Log** (`parse_json_log`)               → `JSONLogEntry`
 *    Formato: `{"timestamp":..., "level":..., "service":..., "message":..., "metadata":{...}}`
 *
 *  - **Syslog BSD (RFC 3164)** (`parse_syslog`)    → `SyslogEntry`
 *    Formato: `[<priority>] Mmm DD HH:MM:SS hostname service[pid]: message`
 *
 *  - **Nginx Error Log** (`parse_nginx_error`)     → `NginxErrorEntry`
 *    Formato: `YYYY/MM/DD HH:MM:SS [level] pid#tid: *connid message, client: IP`
 *
 * Funções auxiliares de timestamp:
 *  - `parse_apache_timestamp`  — "DD/Mmm/YYYY:HH:MM:SS +offset"
 *  - `parse_iso8601_timestamp` — "YYYY-MM-DDTHH:MM:SS"
 *  - `parse_syslog_timestamp`  — "Mmm DD HH:MM:SS"
 *
 * Todas as funções de parsing retornam 0 em caso de sucesso e -1 em caso de
 * falha, nunca modificando a estrutura de saída parcialmente em caso de erro
 * (usam memset inicial).
 */

// src/log_parser_simple.c - Versão sem regex (mais portável)
#include "log_parser.h"
#include <ctype.h>

// ============================================================================
// PARSER: Apache Combined Log (SEM REGEX) - Extrai campos de logs Apache
// ============================================================================

/**
 * @brief Faz o parsing de uma linha no formato Apache Combined Log.
 *
 * @details O formato Apache Combined Log tem a estrutura:
 * @code
 * 192.168.1.1 - - [13/Feb/2024:10:23:45 +0000] "GET /index.html HTTP/1.1" 200 1234
 * @endcode
 *
 * O parsing é feito campo a campo, avançando um ponteiro pela linha:
 *  1. IP do cliente (até ao primeiro espaço)
 *  2. Dois campos ignorados (" - - ", ident e authuser)
 *  3. Timestamp entre '[' e ']'
 *  4. Request line entre aspas: METHOD URL HTTP/versão
 *  5. Código de status HTTP (inteiro após as aspas de fecho)
 *  6. Tamanho da resposta em bytes
 *
 * @param line  Linha de log terminada em '\0'.
 * @param entry Estrutura de saída a preencher.
 * @return 0 em caso de sucesso; -1 se o formato não for reconhecido ou os
 *         parâmetros forem NULL.
 */
int parse_apache_log(const char* line, ApacheLogEntry* entry) {
    if (!line || !entry) return -1;

    /* Inicializar toda a estrutura a zero para evitar lixo nos campos não preenchidos */
    memset(entry, 0, sizeof(ApacheLogEntry));

    const char* ptr = line;

    /* Passo 1: Extrair IP do cliente — avançar até ao primeiro espaço */
    int i = 0;
    while (*ptr && *ptr != ' ' && i < MAX_IP_LENGTH - 1) {
        entry->ip[i++] = *ptr++;
    }
    entry->ip[i] = '\0';

    /* O IP deve ser seguido de um espaço; caso contrário, a linha está mal formada */
    if (*ptr != ' ') return -1;

    /* Passo 2: Saltar os campos "ident" e "authuser" (tipicamente " - - ") */
    while (*ptr == ' ') ptr++;
    if (*ptr == '-') ptr++;   /* ident */
    while (*ptr == ' ') ptr++;
    if (*ptr == '-') ptr++;   /* authuser */
    while (*ptr == ' ') ptr++;

    /* Passo 3: Extrair timestamp no formato [13/Feb/2024:10:23:45 +0000] */
    if (*ptr != '[') return -1;
    ptr++; /* saltar '[' */

    char timestamp_str[64];
    i = 0;
    /* Copiar tudo até ao ']' para uma string auxiliar */
    while (*ptr && *ptr != ']' && i < 63) {
        timestamp_str[i++] = *ptr++;
    }
    timestamp_str[i] = '\0';

    if (*ptr != ']') return -1;
    ptr++; /* saltar ']' */

    /* Delegar o parsing do timestamp numa função auxiliar */
    parse_apache_timestamp(timestamp_str, &entry->timestamp);

    /* Passo 4: Avançar até às aspas que iniciam a "request line" */
    while (*ptr == ' ') ptr++;

    /* Extrair método, URL e versão HTTP de dentro das aspas */
    if (*ptr != '"') return -1;
    ptr++; /* saltar '"' de abertura */

    /* Método HTTP (GET, POST, etc.) — até ao próximo espaço */
    i = 0;
    while (*ptr && *ptr != ' ' && i < 15) {
        entry->method[i++] = *ptr++;
    }
    entry->method[i] = '\0';

    while (*ptr == ' ') ptr++;

    /* URL — até ao próximo espaço ou às aspas de fecho */
    i = 0;
    while (*ptr && *ptr != ' ' && *ptr != '"' && i < MAX_URL_LENGTH - 1) {
        entry->url[i++] = *ptr++;
    }
    entry->url[i] = '\0';

    while (*ptr == ' ') ptr++;

    /* Versão do protocolo HTTP (ex.: "1.1" depois de "HTTP/") */
    if (strncmp(ptr, "HTTP/", 5) == 0) {
        ptr += 5;
        i = 0;
        while (*ptr && *ptr != '"' && i < 15) {
            entry->http_version[i++] = *ptr++;
        }
        entry->http_version[i] = '\0';
    }

    /* Saltar até ao fecho das aspas da request line */
    while (*ptr && *ptr != '"') ptr++;
    if (*ptr == '"') ptr++;

    /* Passo 5: Extrair o código de status HTTP (ex.: 200, 404, 500) */
    while (*ptr == ' ') ptr++;
    entry->status_code = atoi(ptr);

    /* Avançar para além dos dígitos do status */
    while (*ptr && isdigit(*ptr)) ptr++;

    /* Passo 6: Extrair o tamanho da resposta em bytes */
    while (*ptr == ' ') ptr++;
    entry->response_size = atol(ptr);

    return 0;
}

// ============================================================================
// PARSER: JSON Log (simplificado)
// ============================================================================

/**
 * @brief Extrai o valor de uma chave de um objecto JSON simples (sem aninhamento).
 *
 * @details Esta função de uso interno procura a padrão `"key":value` ou
 * `"key":"value"` dentro da string @p json. Suporta:
 *  - Valores string (entre aspas duplas)
 *  - Valores escalares (número, booleano — terminados por `,`, `}` ou newline)
 *
 * Não é um parser JSON completo; funciona para a estrutura plana dos logs
 * desta aplicação. Não lida com escapes `\"` dentro de strings.
 *
 * @param json    String JSON de entrada onde pesquisar.
 * @param key     Nome da chave a procurar (sem aspas).
 * @param value   Buffer de saída onde escrever o valor.
 * @param max_len Tamanho máximo do buffer de saída.
 * @return Ponteiro para @p value em caso de sucesso; NULL se a chave não existir.
 */
static char* extract_json_value(const char* json, const char* key, char* value, size_t max_len) {
    /* Construir o padrão de pesquisa: "key": */
    char search_key[128];
    snprintf(search_key, sizeof(search_key), "\"%s\":", key);

    /* Localizar a chave dentro do JSON */
    const char* pos = strstr(json, search_key);
    if (!pos) return NULL;

    /* Avançar para depois do ':' e saltar espaços opcionais */
    pos += strlen(search_key);
    while (*pos && isspace(*pos)) pos++;

    if (*pos == '"') {
        /* Valor entre aspas: copiar tudo até à aspa de fecho */
        pos++;
        const char* end = strchr(pos, '"');
        if (!end) return NULL;

        size_t len = end - pos;
        if (len >= max_len) len = max_len - 1;
        strncpy(value, pos, len);
        value[len] = '\0';
    } else {
        /* Valor escalar: termina na vírgula, '}' ou fim de linha */
        const char* end = pos;
        while (*end && *end != ',' && *end != '}' && *end != '\n') end++;

        size_t len = end - pos;
        if (len >= max_len) len = max_len - 1;
        strncpy(value, pos, len);
        value[len] = '\0';
    }

    return value;
}

/**
 * @brief Faz o parsing de uma linha de log em formato JSON.
 *
 * @details Espera um objecto JSON com os campos:
 * @code
 * {
 *   "timestamp": "2024-02-13T10:23:45",
 *   "level": "ERROR",
 *   "service": "auth-service",
 *   "message": "Login failed",
 *   "metadata": { "ip": "192.168.1.1", "user_id": 42 }
 * }
 * @endcode
 *
 * O campo "level" é mapeado para as constantes LOG_* definidas em log_parser.h.
 * Os campos de metadados são extraídos do sub-objecto "metadata" se existir.
 *
 * @param line  Linha de log (deve começar com '{').
 * @param entry Estrutura de saída a preencher.
 * @return 0 em caso de sucesso; -1 se a linha não começar com '{' ou parâmetros NULL.
 */
int parse_json_log(const char* line, JSONLogEntry* entry) {
    if (!line || !entry) return -1;
    /* Verificação rápida: JSON começa sempre com '{' */
    if (line[0] != '{') return -1;

    /* Inicializar a estrutura a zero antes de preencher campos individuais */
    memset(entry, 0, sizeof(JSONLogEntry));

    char value[MAX_MSG_LENGTH];

    /* Extrair e converter o timestamp ISO 8601 */
    if (extract_json_value(line, "timestamp", value, sizeof(value))) {
        parse_iso8601_timestamp(value, &entry->timestamp);
    }

    /* Mapear a string do nível para a constante LOG_* correspondente */
    if (extract_json_value(line, "level", value, sizeof(value))) {
        if (strcmp(value, "DEBUG") == 0) entry->level = LOG_DEBUG;
        else if (strcmp(value, "INFO") == 0) entry->level = LOG_INFO;
        else if (strcmp(value, "WARN") == 0 || strcmp(value, "WARNING") == 0)
            entry->level = LOG_WARN;
        else if (strcmp(value, "ERROR") == 0) entry->level = LOG_ERROR;
        else if (strcmp(value, "CRITICAL") == 0 || strcmp(value, "FATAL") == 0)
            entry->level = LOG_CRITICAL;
    }

    /* Extrair o nome do serviço que gerou o log */
    if (extract_json_value(line, "service", value, sizeof(value))) {
    strncpy(entry->service, value, sizeof(entry->service) - 1);
    entry->service[sizeof(entry->service) - 1] = '\0';  /* Garantir null terminator */
    }

    /* Extrair a mensagem de log principal */
    if (extract_json_value(line, "message", value, sizeof(value))) {
    strncpy(entry->message, value, sizeof(entry->message) - 1);
    entry->message[sizeof(entry->message) - 1] = '\0';  /* Garantir null terminator */
    }

    /* Extrair campos do sub-objecto "metadata" (IP e user_id), se presente */
    const char* metadata = strstr(line, "\"metadata\"");
    if (metadata) {
        /* Pesquisar IP apenas dentro do bloco metadata para evitar falsos positivos */
        if (extract_json_value(metadata, "ip", value, sizeof(value))) {
    	strncpy(entry->ip, value, sizeof(entry->ip) - 1);
    	entry->ip[sizeof(entry->ip) - 1] = '\0';  /* Garantir null terminator */
    }

        /* user_id é um inteiro; atoi converte a string extraída */
        if (extract_json_value(metadata, "user_id", value, sizeof(value))) {
            entry->user_id = atoi(value);
        }
    }

    return 0;
}

// ============================================================================
// PARSER: Syslog
// ============================================================================

/**
 * @brief Faz o parsing de uma linha no formato Syslog BSD (RFC 3164).
 *
 * @details O formato Syslog BSD tem a estrutura:
 * @code
 * [<priority>] Mmm DD HH:MM:SS hostname service[pid]: message
 * @endcode
 *
 * O campo de prioridade `<nnn>` é opcional (RFC 3164). A prioridade codifica
 * facility e severity: `priority = facility * 8 + severity`. Os três bits
 * menos significativos são a severity (0=emerg ... 7=debug).
 *
 * Depois do timestamp e hostname, o campo de serviço pode ter um PID entre
 * `[` e `]`. A função também detecta automaticamente padrões de segurança
 * na mensagem (falhas de autenticação, sudo, firewall).
 *
 * @param line  Linha de log terminada em '\0'.
 * @param entry Estrutura de saída a preencher.
 * @return 0 em caso de sucesso; -1 se os parâmetros forem NULL.
 */
int parse_syslog(const char* line, SyslogEntry* entry) {
    if (!line || !entry) return -1;

    /* Inicializar toda a estrutura a zero */
    memset(entry, 0, sizeof(SyslogEntry));

    const char* ptr = line;

    /* Passo 1: Extrair campo de prioridade opcional <nnn>
     * priority = (facility << 3) | severity
     * Os bits 0-2 codificam a severidade syslog (0=emerg, 7=debug) */
    if (*ptr == '<') {
        ptr++;
        entry->priority = atoi(ptr);
        /* Avançar até ao '>' que fecha o campo de prioridade */
        while (*ptr && *ptr != '>') ptr++;
        if (*ptr == '>') ptr++;
    }

    /* Passo 2: Extrair timestamp no formato syslog "Mmm DD HH:MM:SS" */
    char timestamp_str[32];
    int i = 0;

    /* Mês: exactamente 3 letras (ex.: "Feb") */
    while (*ptr && i < 3 && isalpha(*ptr)) {
        timestamp_str[i++] = *ptr++;
    }
    timestamp_str[i++] = ' ';

    /* Saltar espaços antes do dia (o dia pode ter espaço de padding: " 3") */
    while (*ptr == ' ') ptr++;

    /* Dia do mês: um ou dois dígitos */
    while (*ptr && isdigit(*ptr) && i < 31) {
        timestamp_str[i++] = *ptr++;
    }
    timestamp_str[i++] = ' ';

    /* Saltar espaços antes da hora */
    while (*ptr == ' ') ptr++;

    /* Hora no formato HH:MM:SS — copiar até ter dois ':' */
    int colon_count = 0;
    while (*ptr && colon_count < 2 && i < 31) {
        timestamp_str[i++] = *ptr;
        if (*ptr == ':') colon_count++;
        ptr++;
    }
    /* Copiar os dígitos dos segundos (depois do segundo ':') */
    while (*ptr && isdigit(*ptr) && i < 31) {
        timestamp_str[i++] = *ptr++;
    }
    timestamp_str[i] = '\0';

    /* Converter a string de timestamp para struct tm */
    parse_syslog_timestamp(timestamp_str, &entry->timestamp);

    /* Passo 3: Saltar espaços e extrair o hostname */
    while (*ptr && isspace(*ptr)) ptr++;

    i = 0;
    while (*ptr && *ptr != ' ' && i < 255) {
        entry->hostname[i++] = *ptr++;
    }
    entry->hostname[i] = '\0';

    while (*ptr && isspace(*ptr)) ptr++;

    /* Passo 4: Extrair o nome do serviço (até '[', ':', ou espaço) */
    i = 0;
    while (*ptr && *ptr != '[' && *ptr != ':' && i < 63) {
        entry->service[i++] = *ptr++;
    }
    entry->service[i] = '\0';

    /* Passo 5: Extrair o PID se existir no formato service[1234] */
    if (*ptr == '[') {
        ptr++;
        entry->pid = atoi(ptr);
        /* Avançar até ao ']' de fecho */
        while (*ptr && *ptr != ']') ptr++;
        if (*ptr == ']') ptr++;
    }

    /* Passo 6: Saltar ':' e whitespace antes da mensagem */
    if (*ptr == ':') ptr++;
    while (*ptr && isspace(*ptr)) ptr++;

    /* O resto da linha é a mensagem de log */
    strncpy(entry->message, ptr, sizeof(entry->message) - 1);

    /* Passo 7: Detectar padrões de segurança na mensagem (pré-computado
     * aqui para evitar pesquisas repetidas no classificador) */

    /* Falha de autenticação: palavras-chave comuns em PAM/sshd */
    entry->is_auth_failure = (strstr(entry->message, "authentication failure") != NULL ||
                               strstr(entry->message, "Failed password") != NULL ||
                               strstr(entry->message, "invalid user") != NULL);

    /* Tentativa de sudo: o serviço é "sudo" */
    entry->is_sudo_attempt = (strstr(entry->service, "sudo") != NULL);

    /* Bloqueio de firewall: pacotes REJECT ou DROP (iptables/nftables) */
    entry->is_firewall_block = (strstr(entry->message, "REJECT") != NULL ||
                                 strstr(entry->message, "DROP") != NULL);

    return 0;
}

// ============================================================================
// PARSER: Nginx Error Log
// ============================================================================

/**
 * @brief Faz o parsing de uma linha no formato Nginx Error Log.
 *
 * @details O formato Nginx Error Log tem a estrutura:
 * @code
 * 2024/02/13 10:23:45 [error] 1234#5678: *42 message, client: 192.168.1.1, ...
 * @endcode
 *
 * Campos extraídos:
 *  - **timestamp**: data/hora no formato YYYY/MM/DD HH:MM:SS (via sscanf)
 *  - **level**: nível de severidade entre '[' e ']'
 *  - **pid#tid**: PID e TID do worker nginx separados por '#'
 *  - **connection_id**: número de ligação prefixado por '*'
 *  - **client_ip**: endereço IP após "client: "
 *  - **message**: texto entre o ':' do pid#tid e o ", client:"
 *
 * @param line  Linha de log terminada em '\0'.
 * @param entry Estrutura de saída a preencher.
 * @return 0 em caso de sucesso; -1 se os parâmetros forem NULL.
 */
int parse_nginx_error(const char* line, NginxErrorEntry* entry) {
    if (!line || !entry) return -1;

    /* Inicializar toda a estrutura a zero */
    memset(entry, 0, sizeof(NginxErrorEntry));

    /* Passo 1: Extrair timestamp "YYYY/MM/DD HH:MM:SS" com sscanf
     * Nota: sscanf é seguro aqui porque os campos são inteiros */
    struct tm tm = {0};
    if (sscanf(line, "%d/%d/%d %d:%d:%d",
               &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
               &tm.tm_hour, &tm.tm_min, &tm.tm_sec) == 6) {
        /* Ajustar para representação de struct tm (ano desde 1900, mês 0-11) */
        tm.tm_year -= 1900;
        tm.tm_mon -= 1;
        entry->timestamp = tm;
    }

    /* Passo 2: Extrair o nível de severidade do campo [level] */
    const char* level_start = strchr(line, '[');
    if (level_start) {
        level_start++; /* avançar para depois de '[' */
        /* Comparar prefixos por ordem do mais específico para o mais genérico */
        if (strncmp(level_start, "debug", 5) == 0) entry->level = NGINX_DEBUG;
        else if (strncmp(level_start, "info", 4) == 0) entry->level = NGINX_INFO;
        else if (strncmp(level_start, "notice", 6) == 0) entry->level = NGINX_NOTICE;
        else if (strncmp(level_start, "warn", 4) == 0) entry->level = NGINX_WARN;
        else if (strncmp(level_start, "error", 5) == 0) entry->level = NGINX_ERROR;
        else if (strncmp(level_start, "crit", 4) == 0) entry->level = NGINX_CRIT;
        else if (strncmp(level_start, "alert", 5) == 0) entry->level = NGINX_ALERT;
        else if (strncmp(level_start, "emerg", 5) == 0) entry->level = NGINX_EMERG;
    }

    /* Passo 3: Extrair PID e TID do padrão "pid#tid:" após o ']' */
    const char* pid_start = strchr(line, ']');
    if (pid_start) {
        pid_start += 2; /* saltar "] " */
        sscanf(pid_start, "%d#%d", &entry->pid, &entry->tid);
    }

    /* Passo 4: Extrair o ID de ligação do padrão "*nnnnn" */
    const char* conn_start = strchr(line, '*');
    if (conn_start) {
        entry->connection_id = atol(conn_start + 1);
    }

    /* Passo 5: Extrair o IP do cliente do campo "client: IP" */
    const char* client_start = strstr(line, "client: ");
    if (client_start) {
        client_start += 8; /* saltar "client: " (8 caracteres) */
        int i = 0;
        /* Copiar o IP até à vírgula, espaço, ou fim da string */
        while (*client_start && *client_start != ',' && *client_start != ' ' && i < MAX_IP_LENGTH - 1) {
            entry->client_ip[i++] = *client_start++;
        }
        entry->client_ip[i] = '\0';
    }

    /* Passo 6: Extrair a mensagem entre o ':' do pid#tid e o ", client:" */
    const char* msg_start = strchr(pid_start, ':');
    if (msg_start) {
        msg_start += 2; /* saltar ": " */
        /* A mensagem termina antes de ", client:" ou no fim da linha */
        const char* msg_end = strstr(msg_start, ", client:");
        if (!msg_end) msg_end = msg_start + strlen(msg_start);

        size_t len = msg_end - msg_start;
        if (len >= sizeof(entry->message)) len = sizeof(entry->message) - 1;
        strncpy(entry->message, msg_start, len);
        entry->message[len] = '\0';
    }

    return 0;
}

// ============================================================================
// Funções auxiliares de timestamp (mantêm-se iguais)
// ============================================================================

/**
 * @brief Faz o parsing de um timestamp no formato Apache Combined Log.
 *
 * @details O formato é "DD/Mmm/YYYY:HH:MM:SS +offset", por exemplo:
 * @code
 * 13/Feb/2024:10:23:45 +0000
 * @endcode
 * O campo de offset (+0000) é ignorado; o timestamp é guardado em hora local.
 *
 * @param timestamp_str String com o timestamp (sem os '[' e ']').
 * @param tm_out        Estrutura `struct tm` a preencher.
 * @return 0 em caso de sucesso; -1 se o formato não for reconhecido.
 */
int parse_apache_timestamp(const char* timestamp_str, struct tm* tm_out) {
    char month_str[4];
    int day, year, hour, min, sec;

    /* Extrair todos os campos de uma vez com sscanf */
    if (sscanf(timestamp_str, "%d/%3s/%d:%d:%d:%d",
               &day, month_str, &year, &hour, &min, &sec) != 6) {
        return -1;
    }

    /* Tabela de meses para converter a abreviação de 3 letras para índice 0-11 */
    const char* months[] = {"Jan","Feb","Mar","Apr","May","Jun",
                            "Jul","Aug","Sep","Oct","Nov","Dec"};
    int month = -1;
    for (int i = 0; i < 12; i++) {
        if (strcmp(month_str, months[i]) == 0) {
            month = i;
            break;
        }
    }

    /* Mês não reconhecido — falhar de forma controlada */
    if (month == -1) return -1;

    /* Preencher struct tm com os valores convertidos para as convenções POSIX:
     * tm_year: anos desde 1900; tm_mon: 0-11 */
    tm_out->tm_mday = day;
    tm_out->tm_mon = month;
    tm_out->tm_year = year - 1900;
    tm_out->tm_hour = hour;
    tm_out->tm_min = min;
    tm_out->tm_sec = sec;

    return 0;
}

/**
 * @brief Faz o parsing de um timestamp ISO 8601 básico.
 *
 * @details O formato esperado é "YYYY-MM-DDTHH:MM:SS", por exemplo:
 * @code
 * 2024-02-13T10:23:45
 * @endcode
 * O sufixo de timezone (ex.: "Z", "+01:00") é ignorado.
 *
 * @param timestamp_str String com o timestamp ISO 8601.
 * @param tm_out        Estrutura `struct tm` a preencher.
 * @return 0 em caso de sucesso; -1 se o formato não for reconhecido.
 */
int parse_iso8601_timestamp(const char* timestamp_str, struct tm* tm_out) {
    int year, month, day, hour, min, sec;

    /* sscanf com o separador 'T' entre data e hora */
    if (sscanf(timestamp_str, "%d-%d-%dT%d:%d:%d",
               &year, &month, &day, &hour, &min, &sec) != 6) {
        return -1;
    }

    /* Ajustar para convenções de struct tm */
    tm_out->tm_year = year - 1900; /* anos desde 1900 */
    tm_out->tm_mon = month - 1;    /* meses 0-11 */
    tm_out->tm_mday = day;
    tm_out->tm_hour = hour;
    tm_out->tm_min = min;
    tm_out->tm_sec = sec;

    return 0;
}

/**
 * @brief Faz o parsing de um timestamp no formato Syslog BSD.
 *
 * @details O formato é "Mmm DD HH:MM:SS" sem ano, por exemplo:
 * @code
 * Feb 13 10:23:45
 * @endcode
 * Como o formato syslog não inclui o ano, este é inferido do ano actual
 * através de `localtime_r` (thread-safe, ao contrário de `localtime`).
 *
 * @note O uso de `localtime_r` em vez de `localtime` é deliberado: evita
 * condições de corrida em ambientes multi-thread, pois cada thread obtém
 * o resultado numa estrutura local em vez de num buffer estático global.
 *
 * @param timestamp_str String com o timestamp syslog (sem ano).
 * @param tm_out        Estrutura `struct tm` a preencher.
 * @return 0 em caso de sucesso; -1 se o formato não for reconhecido.
 */
int parse_syslog_timestamp(const char* timestamp_str, struct tm* tm_out) {
    char month_str[4];
    int day, hour, min, sec;

    /* Extrair todos os campos; o ano não faz parte do formato syslog */
    if (sscanf(timestamp_str, "%3s %d %d:%d:%d",
               month_str, &day, &hour, &min, &sec) != 5) {
        return -1;
    }

    /* Converter abreviação do mês para índice 0-11 */
    const char* months[] = {"Jan","Feb","Mar","Apr","May","Jun",
                            "Jul","Aug","Sep","Oct","Nov","Dec"};
    int month = -1;
    for (int i = 0; i < 12; i++) {
        if (strcmp(month_str, months[i]) == 0) {
            month = i;
            break;
        }
    }

    if (month == -1) return -1;

    /* Obter o ano actual de forma thread-safe:
     * localtime_r escreve para uma struct tm local (stack) em vez de um
     * buffer estático global, pelo que é seguro em contextos multi-thread */
    time_t now = time(NULL);
    struct tm now_tm;
    // Usar localtime_r com estrutura local para garantir Thread-Safety e evitar Race Conditions na Fase 2.
    if (localtime_r(&now, &now_tm) == NULL) {
        return -1;
    }

    /* Assumir que o log pertence ao ano corrente (limitação do formato syslog) */
    tm_out->tm_year = now_tm.tm_year;
    tm_out->tm_mon = month;
    tm_out->tm_mday = day;
    tm_out->tm_hour = hour;
    tm_out->tm_min = min;
    tm_out->tm_sec = sec;

    return 0;
}
