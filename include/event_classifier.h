/**
 * @file event_classifier.h
 * @brief Classificador de eventos de log por tipo e severidade.
 *
 * Define o enumerado de tipos de evento (EventType) com flags de bit,
 * o enumerado de modos de análise (AnalysisMode), a estrutura que representa
 * um evento já classificado (ClassifiedEvent) e os protótipos das funções de
 * classificação por formato de log e de consulta de metadados de eventos.
 *
 * Os tipos de evento são bitmasks e podem ser combinados com OR bitwise,
 * permitindo que um único evento seja classificado com múltiplas categorias
 * em simultâneo (ex.: um erro HTTP 5xx pode ser EVENT_ERROR | EVENT_TRAFFIC).
 */

#ifndef EVENT_CLASSIFIER_H
#define EVENT_CLASSIFIER_H

#include "log_parser.h"
#include <stdbool.h>
#include <stddef.h>

/* ============================================================================
 * TIPOS DE EVENTOS
 * ============================================================================ */

/**
 * @brief Enumerado de tipos de evento representados como flags de bit.
 *
 * Cada valor é uma potência de 2, permitindo combinações com OR bitwise
 * para representar eventos com múltiplas categorias.
 */
typedef enum {
    EVENT_SECURITY    = 1 << 0, /**< @brief Evento de segurança (ex.: falha de autenticação, bloqueio de firewall). */
    EVENT_PERFORMANCE = 1 << 1, /**< @brief Evento de desempenho (ex.: latência elevada, timeout, erro de memória). */
    EVENT_TRAFFIC     = 1 << 2, /**< @brief Evento de tráfego de rede (ex.: pedido HTTP, transferência de dados). */
    EVENT_ERROR       = 1 << 3, /**< @brief Evento de erro genérico (ex.: código HTTP 5xx, falha de parse). */
    EVENT_NORMAL      = 1 << 4  /**< @brief Evento de operação normal, sem categorias de alerta. */
} EventType;

/**
 * @brief Modo de análise que define quais os tipos de evento a processar.
 *
 * Cada modo é definido como uma combinação de flags EventType. O modo
 * MODE_FULL ativa todas as categorias em simultâneo.
 */
typedef enum {
    MODE_SECURITY    = EVENT_SECURITY,                                                      /**< @brief Processa apenas eventos de segurança. */
    MODE_PERFORMANCE = EVENT_PERFORMANCE,                                                   /**< @brief Processa apenas eventos de desempenho. */
    MODE_TRAFFIC     = EVENT_TRAFFIC,                                                       /**< @brief Processa apenas eventos de tráfego. */
    MODE_FULL        = EVENT_SECURITY | EVENT_PERFORMANCE | EVENT_TRAFFIC | EVENT_ERROR     /**< @brief Processa todos os tipos de evento (segurança, desempenho, tráfego e erros). */
} AnalysisMode;

/* ============================================================================
 * ESTRUTURAS DE EVENTOS CLASSIFICADOS
 * ============================================================================ */

/**
 * @brief Evento de log classificado, com tipo, severidade e dados originais.
 *
 * Após a classificação por uma das funções classify_*_event(), esta estrutura
 * contém o tipo (bitmask de EventType), a severidade numérica, uma descrição
 * textual, os dados originais no formato de origem e um timestamp normalizado
 * para ordenação cronológica.
 */
typedef struct {
    int  event_types;   /**< @brief Bitmask de EventType que indica as categorias do evento (pode ter múltiplas flags). */
    int  severity;      /**< @brief Nível de severidade numérico: 0=INFO, 1=LOW, 2=MEDIUM, 3=HIGH, 4=CRITICAL. */
    char description[256]; /**< @brief Descrição textual gerada pelo classificador sobre o evento. */

    /**
     * @brief Dados originais da entrada de log, na union do formato de origem.
     *
     * Apenas o campo correspondente ao formato que gerou o evento está válido;
     * os restantes campos da union contêm dados indeterminados.
     */
    union {
        ApacheLogEntry  apache; /**< @brief Dados originais quando o evento provém de um log Apache Combined. */
        JSONLogEntry    json;   /**< @brief Dados originais quando o evento provém de um log JSON estruturado. */
        SyslogEntry     syslog; /**< @brief Dados originais quando o evento provém de um log Syslog. */
        NginxErrorEntry nginx;  /**< @brief Dados originais quando o evento provém de um log Nginx Error. */
    } data;

    time_t timestamp;   /**< @brief Timestamp Unix normalizado (segundos desde epoch) para ordenação cronológica. */
} ClassifiedEvent;

/* ============================================================================
 * FUNÇÕES DE CLASSIFICAÇÃO
 * ============================================================================ */

/**
 * @brief Classifica um evento proveniente de um log Apache Combined.
 *
 * Analisa @p entry e determina o tipo (EventType) e a severidade do evento,
 * preenchendo @p event com os resultados da classificação e copiando os dados
 * originais para event->data.apache.
 *
 * @param entry Ponteiro para a entrada Apache já parseada.
 * @param event Ponteiro para a estrutura ClassifiedEvent a preencher.
 * @return Bitmask de EventType com os tipos atribuídos ao evento.
 */
int classify_apache_event(const ApacheLogEntry* entry, ClassifiedEvent* event);

/**
 * @brief Classifica um evento proveniente de um log JSON estruturado.
 *
 * Analisa @p entry e determina o tipo (EventType) e a severidade do evento,
 * preenchendo @p event com os resultados da classificação e copiando os dados
 * originais para event->data.json.
 *
 * @param entry Ponteiro para a entrada JSON já parseada.
 * @param event Ponteiro para a estrutura ClassifiedEvent a preencher.
 * @return Bitmask de EventType com os tipos atribuídos ao evento.
 */
int classify_json_event(const JSONLogEntry* entry, ClassifiedEvent* event);

/**
 * @brief Classifica um evento proveniente de um log Syslog.
 *
 * Analisa @p entry (incluindo as flags heurísticas is_auth_failure,
 * is_sudo_attempt e is_firewall_block) e determina o tipo e severidade,
 * preenchendo @p event e copiando os dados para event->data.syslog.
 *
 * @param entry Ponteiro para a entrada Syslog já parseada.
 * @param event Ponteiro para a estrutura ClassifiedEvent a preencher.
 * @return Bitmask de EventType com os tipos atribuídos ao evento.
 */
int classify_syslog_event(const SyslogEntry* entry, ClassifiedEvent* event);

/**
 * @brief Classifica um evento proveniente de um log Nginx Error.
 *
 * Analisa @p entry e determina o tipo (EventType) e a severidade do evento,
 * preenchendo @p event com os resultados da classificação e copiando os dados
 * originais para event->data.nginx.
 *
 * @param entry Ponteiro para a entrada Nginx Error já parseada.
 * @param event Ponteiro para a estrutura ClassifiedEvent a preencher.
 * @return Bitmask de EventType com os tipos atribuídos ao evento.
 */
int classify_nginx_event(const NginxErrorEntry* entry, ClassifiedEvent* event);

/**
 * @brief Verifica se um evento classificado corresponde ao modo de análise ativo.
 *
 * Compara o bitmask event->event_types com o bitmask @p mode usando AND bitwise.
 * Devolve true se pelo menos uma flag coincidir, indicando que o evento deve
 * ser incluído no processamento do modo selecionado.
 *
 * @param event Ponteiro para o evento classificado a testar.
 * @param mode  Modo de análise ativo (combinação de flags AnalysisMode).
 * @return true se o evento corresponder ao modo; false caso contrário.
 */
bool event_matches_mode(const ClassifiedEvent* event, AnalysisMode mode);

/**
 * @brief Obtém o nome textual de um tipo de evento, usando um buffer fornecido pelo chamador.
 *
 * Converte o valor de @p event_type (uma única flag EventType) para a sua
 * representação textual (ex.: "SECURITY", "PERFORMANCE") e escreve-a em
 * @p buffer. O buffer deve ter pelo menos @p buf_size bytes.
 *
 * @param event_type Valor de uma flag EventType (deve ser uma única flag, não uma combinação).
 * @param buffer     Buffer de destino fornecido pelo chamador.
 * @param buf_size   Tamanho em bytes do buffer de destino.
 * @return Ponteiro para @p buffer com o nome textual, ou "UNKNOWN" se não reconhecido.
 */
const char* get_event_type_name(int event_type, char* buffer, size_t buf_size);

/**
 * @brief Obtém o nome textual de um nível de severidade.
 *
 * Converte o valor numérico @p severity para a sua representação textual
 * (ex.: 0 → "INFO", 4 → "CRITICAL").
 *
 * @param severity Nível de severidade numérico (0=INFO, 1=LOW, 2=MEDIUM, 3=HIGH, 4=CRITICAL).
 * @return Ponteiro para uma string estática com o nome da severidade,
 *         ou "UNKNOWN" se o valor estiver fora do intervalo esperado.
 */
const char* get_severity_name(int severity);

#endif /* EVENT_CLASSIFIER_H */
