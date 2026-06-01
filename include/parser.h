/**
 * @file parser.h
 * @brief Estruturas principais e protótipos do parser de logs do logAnalyzer.
 *
 * Define os enumerados de formato (LogFormat) e nível (LogLevel),
 * a estrutura normalizada de uma entrada de log (LogEntry), o acumulador
 * de métricas por worker (Metrics) e os protótipos das funções de deteção,
 * parse, acumulação e configuração de modo de análise.
 */

#ifndef PARSER_H
#define PARSER_H

/* =========================================================
 * parser.h  –  Estruturas e protótipos do parser de logs
 * ========================================================= */

#include <stddef.h>  /* size_t */

/* ---------------------------------------------------------
 * Formatos de log suportados
 * --------------------------------------------------------- */
/**
 * @brief Enumerado que identifica o formato de uma linha de log.
 *
 * Cada valor corresponde a um formato textual distinto que o sistema
 * é capaz de reconhecer e processar.
 */
typedef enum {
    FORMAT_UNKNOWN = 0,  /**< @brief Formato não reconhecido ou ainda não detetado. */
    FORMAT_APACHE,       /**< @brief Apache Combined Log — ex.: 192.168.1.1 - - [13/Feb/2024:...] "GET /..." 200 1234 */
    FORMAT_JSON,         /**< @brief Log estruturado em JSON — ex.: {"timestamp":..., "level":..., "message":...} */
    FORMAT_SYSLOG,       /**< @brief Syslog (RFC3164) — ex.: Feb 13 10:23:45 hostname service[pid]: message */
    FORMAT_NGINX_ERROR   /**< @brief Nginx Error Log — ex.: 2024/02/13 10:23:45 [error] 12345#0: ... */
} LogFormat;

/* ------------------------------------------------------------
 * Níveis / tipos de evento
 * --------------------------------------------------------- */
/**
 * @brief Nível de severidade de uma entrada de log.
 *
 * Os valores estão ordenados por gravidade crescente, permitindo
 * comparações numéricas entre níveis.
 */
typedef enum {
    LEVEL_UNKNOWN  = 0, /**< @brief Nível não reconhecido ou ausente na linha de log. */
    LEVEL_DEBUG,        /**< @brief Mensagem de depuração, utilidade apenas em desenvolvimento. */
    LEVEL_INFO,         /**< @brief Informação operacional normal. */
    LEVEL_WARN,         /**< @brief Aviso de situação potencialmente problemática. */
    LEVEL_ERROR,        /**< @brief Erro que impede uma operação mas não termina o processo. */
    LEVEL_CRITICAL      /**< @brief Erro crítico de alta severidade; pode comprometer o sistema. */
} LogLevel;

/* ---------------------------------------------------------
 * Uma entrada de log já parseada
 * --------------------------------------------------------- */
#define IP_LEN      16   /**< @brief Tamanho máximo de um endereço IPv4 em texto, incluindo '\0' ("255.255.255.255\0"). */
#define MSG_LEN    256   /**< @brief Tamanho máximo da mensagem/URL/descrição de uma entrada de log. */
#define MAX_ALERTS  20   /**< @brief Número máximo de alertas críticos que um worker pode registar. */
#define ALERT_LEN  128   /**< @brief Tamanho máximo de cada string de alerta armazenada. */

/**
 * @brief Representação normalizada de uma linha de log já processada.
 *
 * Independentemente do formato de origem (Apache, JSON, Syslog, Nginx),
 * os campos relevantes são mapeados para esta estrutura por parse_line().
 */
typedef struct {
    LogFormat format;          /**< @brief Formato de log de onde a entrada foi extraída. */
    LogLevel  level;           /**< @brief Nível de severidade da entrada. */
    int       http_status;     /**< @brief Código de estado HTTP (ex.: 200, 404); 0 se não aplicável. */
    char      ip[IP_LEN];      /**< @brief Endereço IP do cliente, quando presente na linha; string vazia caso contrário. */
    char      message[MSG_LEN];/**< @brief Mensagem principal, URL pedida ou descrição do evento. */
} LogEntry;

/* ---------------------------------------------------------
 * Acumulador de métricas por worker
 * --------------------------------------------------------- */
#define MAX_IPS 1024   /**< @brief Número máximo de endereços IP distintos rastreados por worker. */

/**
 * @brief Acumulador de métricas produzido por cada processo/thread worker.
 *
 * Contém contadores de linhas por nível, contadores de erros HTTP,
 * tabela dos IPs mais frequentes e lista de alertas críticos encontrados
 * durante o processamento da fatia de log atribuída ao worker.
 */
typedef struct {
    long total_lines;              /**< @brief Total de linhas processadas pelo worker. */
    long count_debug;              /**< @brief Número de entradas com nível DEBUG. */
    long count_info;               /**< @brief Número de entradas com nível INFO. */
    long count_warn;               /**< @brief Número de entradas com nível WARN. */
    long count_error;              /**< @brief Número de entradas com nível ERROR. */
    long count_critical;           /**< @brief Número de entradas com nível CRITICAL. */
    long count_4xx;                /**< @brief Número de respostas HTTP com código 4xx (erros de cliente). */
    long count_5xx;                /**< @brief Número de respostas HTTP com código 5xx (erros de servidor). */

    /* Top IPs: guardamos pares (ip, contagem) */
    char ip_list[MAX_IPS][IP_LEN]; /**< @brief Lista dos endereços IP observados (até MAX_IPS entradas distintas). */
    long ip_count[MAX_IPS];        /**< @brief Número de ocorrências de cada IP em ip_list (índice correspondente). */
    int  ip_num;                   /**< @brief Número de IPs distintos atualmente registados em ip_list/ip_count. */

    /* Alertas críticos/alta severidade guardados por worker */
    char alerts[MAX_ALERTS][ALERT_LEN]; /**< @brief Mensagens de alertas críticos ou de alta severidade encontrados. */
    int  num_alerts;                    /**< @brief Número de alertas atualmente guardados no array alerts. */
} Metrics;

/* ---------------------------------------------------------
 * Protótipos
 * --------------------------------------------------------- */

/**
 * @brief Deteta o formato de uma linha de log.
 *
 * Analisa a string @p line heuristicamente para determinar a que
 * formato de log pertence.
 *
 * @param line Linha de log (string terminada em '\0').
 * @return O LogFormat correspondente, ou FORMAT_UNKNOWN se não reconhecido.
 */
LogFormat detect_format(const char *line);

/**
 * @brief Parseia uma linha de log de acordo com o formato indicado.
 *
 * Interpreta @p line segundo o @p format especificado e preenche
 * a estrutura apontada por @p entry com os campos extraídos.
 *
 * @param line   Linha de log (string terminada em '\0').
 * @param format Formato de log a usar para a interpretação.
 * @param entry  Ponteiro para a estrutura LogEntry a preencher.
 * @return 0 em caso de sucesso; -1 se a linha não puder ser interpretada.
 */
int parse_line(const char *line, LogFormat format, LogEntry *entry);

/**
 * @brief Acumula os dados de uma entrada nas métricas do worker.
 *
 * Atualiza os contadores de @p m (linhas, níveis, HTTP, IPs, alertas)
 * com a informação contida em @p entry.
 *
 * @param m     Ponteiro para a estrutura Metrics a atualizar.
 * @param entry Ponteiro para a LogEntry já parseada.
 */
void update_metrics(Metrics *m, const LogEntry *entry);

/**
 * @brief Inicializa uma estrutura Metrics a zeros.
 *
 * Deve ser chamada antes de qualquer utilização de @p m para garantir
 * que todos os contadores e buffers estão no estado inicial correto.
 *
 * @param m Ponteiro para a estrutura Metrics a inicializar.
 */
void init_metrics(Metrics *m);

/**
 * @brief Converte uma string de nível textual para o enumerado LogLevel.
 *
 * A comparação é insensível a maiúsculas/minúsculas, permitindo strings
 * como "ERROR", "error" ou "Error".
 *
 * @param s String com o nome do nível (ex.: "ERROR", "warn", "info").
 * @return O LogLevel correspondente, ou LEVEL_UNKNOWN se não reconhecido.
 */
LogLevel level_from_string(const char *s);

/**
 * @brief Define o modo de análise a partir de um argumento da linha de comandos.
 *
 * O modo afeta o comportamento de parse_line(): entradas que não correspondam
 * ao modo selecionado são descartadas (parse_line devolve -1).
 * Modos válidos: "security", "performance", "traffic", "full".
 *
 * @param mode_str String com o nome do modo de análise.
 * @return 0 em caso de sucesso; -1 se a string for inválida.
 */
int parser_set_mode_from_string(const char *mode_str);

#endif /* PARSER_H */
