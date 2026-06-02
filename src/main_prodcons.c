/** // Início do bloco de documentação Doxygen do arquivo. | Dicionário: / * * = delimitador de início de comentários de bloco Doxygen.
 * @file main_prodcons.c // Nome deste ficheiro de código fonte. | Dicionário: @file = tag Doxygen para especificar o nome do ficheiro.
 * @brief Ponto de entrada do analisador de logs com o padrão Produtor-Consumidor. // Resumo breve do arquivo. | Dicionário: @brief = tag Doxygen para descrição curta.
 * // Linha de espaçamento da documentação. | Dicionário: N/A
 * @details // Detalhes da orquestração do programa. | Dicionário: @details = tag Doxygen para descrição detalhada.
 * Este ficheiro implementa a orquestração completa do padrão // Explicação da função principal. | Dicionário: orquestração = coordenação de tarefas ou fluxos.
 * **Produtor-Consumidor com bounded buffer**: // Refere o padrão de concorrência. | Dicionário: bounded buffer = buffer circular com limite máximo de tamanho.
 * // Linha de espaçamento da documentação. | Dicionário: N/A
 *  - **Produtores** (threads `run_producer`): cada produtor recebe uma fatia // Descreve a função dos produtores. | Dicionário: threads = linhas de execução concorrentes; fatia = intervalo lógico de dados.
 *    (intervalo de bytes) dos ficheiros de log e insere `LogEntry` no buffer // Explica a atribuição de quota de leitura. | Dicionário: LogEntry = estrutura de registo de log analisada; buffer = área de armazenamento temporário.
 *    circular partilhado, usando o semáforo `empty` para garantir que não // Explica a sincronização de escrita. | Dicionário: semáforo = primitivo de controlo de concorrência.
 *    escreve quando o buffer está cheio. // Explica a lógica de bloqueio no buffer cheio. | Dicionário: N/A
 * // Linha de espaçamento da documentação. | Dicionário: N/A
 *  - **Consumidores** (threads `run_consumer`): retiram entradas do buffer e // Descreve a função dos consumidores. | Dicionário: run_consumer = função executada pela thread consumidora.
 *    actualizam as métricas globais, usando o semáforo `full` para bloquear // Explica a acumulação e o semáforo de dados. | Dicionário: métricas = contadores acumulados de estatísticas; full = semáforo de posições cheias.
 *    enquanto o buffer estiver vazio. // Explica a lógica de bloqueio no buffer vazio. | Dicionário: N/A
 * // Linha de espaçamento da documentação. | Dicionário: N/A
 *  - O buffer circular tem capacidade fixa `BUFFER_SIZE = 30`. // Mostra a capacidade máxima constante. | Dicionário: BUFFER_SIZE = macro que define o tamanho do buffer.
 *    Os índices `in`, `out` e o contador `count` avançam em módulo // Mostra a aritmética de avanço de índices. | Dicionário: in = índice de escrita; out = índice de leitura; count = número de elementos; módulo = operação matemática de resto (%).
 *    `BUFFER_SIZE`, garantindo a reutilização cíclica das posições. // Detalha o reaproveitamento das posições. | Dicionário: cíclica = comportamento repetitivo e circular.
 * // Linha de espaçamento da documentação. | Dicionário: N/A
 *  - A variável global `produtores_ativos` (protegida pelo mutex do buffer) // Explica o papel da variável global. | Dicionário: mutex = trinco de exclusão mútua (mutual exclusion lock).
 *    permite que o último produtor a terminar sinalize a todos os consumidores // Explica a notificação no final do processo. | Dicionário: N/A
 *    que não chegará mais nenhuma entrada, evitando que fiquem bloqueados // Explica o escape dos loops dos consumidores. | Dicionário: N/A
 *    indefinidamente em `sem_wait(&buffer.full)`. // Detalha a prevenção de suspensão perpétua. | Dicionário: sem_wait = chamada de sistema para decrementar/esperar em semáforo.
 * // Linha de espaçamento da documentação. | Dicionário: N/A
 *  - Um monitor dedicado (`run_monitor_thread`) actualiza o dashboard no // Descreve a presença da thread visualizadora. | Dicionário: run_monitor_thread = função de controlo do monitor.
 *    terminal a cada 100 ms enquanto os produtores trabalham. // Especifica a cadência de atualização e o dashboard. | Dicionário: dashboard = painel de progresso na consola.
 *  */ // Fim do bloco de comentário Doxygen. | Dicionário: */ = terminador de comentário.
 
#include <stdio.h> // Inclui funções de entrada/saída padrão da linguagem C. | Dicionário: #include = diretiva para carregar arquivos de cabeçalho; stdio.h = biblioteca padrão de entrada/saída.
#include <stdlib.h> // Inclui funções de sistema e de gestão de memória. | Dicionário: stdlib.h = biblioteca para conversões, malloc, exit e utilitários.
#include <string.h> // Inclui rotinas para comparação e manipulação de strings. | Dicionário: string.h = biblioteca para tratamento de vetores de caracteres.
#include <dirent.h> // Inclui funções de leitura de diretórios (como opendir). | Dicionário: dirent.h = biblioteca Unix para manipulação e travessia de pastas.
#include <pthread.h> // Inclui a API de Threads POSIX para execução concorrente. | Dicionário: pthread.h = biblioteca com rotinas para criação e gestão de threads.
#include <time.h> // Inclui rotinas para medir tempo e obter instantes do relógio. | Dicionário: time.h = biblioteca para relógio e calendarização.
#include <unistd.h> // Inclui chamadas de sistema POSIX básicas (como usleep). | Dicionário: unistd.h = biblioteca de chamadas de sistema do padrão POSIX.
#include <fcntl.h> // Inclui definições para abertura de descritores de ficheiro. | Dicionário: fcntl.h = biblioteca para controlo de ficheiros.
#include <sys/stat.h> // Inclui funções para ver metadados de ficheiros (como stat). | Dicionário: sys/stat.h = metadados de estado de ficheiros.
 
#include "parser.h" // Inclui funções de parser e deteção de formato de logs. | Dicionário: parser.h = cabeçalho do analisador de texto de logs.
#include "posix_io.h" // Inclui wrappers seguros para escrita com POSIX write. | Dicionário: posix_io.h = cabeçalho utilitário personalizado de I/O.
#include "worker_prodcons.h" // Inclui declarações de produtor-consumidor e buffer. | Dicionário: worker_prodcons.h = cabeçalho que define funções e tipos da fila circular.
 
/* Número máximo de produtores suportados */ // Comentário informativo de limite estático. | Dicionário: N/A
#define MAX_WORKERS 64 // Define o limite máximo de threads produtoras (64). | Dicionário: #define = diretiva do pré-processador para definir macros; MAX_WORKERS = macro de limite máximo.
 
/* Arrays de progresso — um slot por produtor, indexado pelo worker_index */ // Descrição dos vetores de monitorização de progresso. | Dicionário: N/A
static long   g_bytes_done[MAX_WORKERS];   /**< Bytes já processados por cada produtor */ // Array local para contar bytes processados individualmente. | Dicionário: static = visibilidade restrita ao ficheiro atual; long = tipo de dados inteiro de 32/64 bits; g_bytes_done = array de progresso.
static long   g_bytes_total[MAX_WORKERS];  /**< Total de bytes atribuído a cada produtor */ // Array local com o tamanho de quota atribuído a cada produtor. | Dicionário: g_bytes_total = array de tamanhos totais.
static int    g_num_workers  = 0;          /**< Número efectivo de produtores lançados */ // Variável local que guarda a quantidade efetiva de produtores. | Dicionário: int = tipo inteiro; g_num_workers = contador global interno de workers.
static time_t g_start_time   = 0;          /**< Instante em que main() iniciou os threads */ // Guarda o instante (timestamp) de arranque do processamento. | Dicionário: time_t = tipo padrão para instantes temporais em Unix.
static volatile int g_all_done = 0;        /**< Flag: 1 quando todos os produtores terminaram (sinaliza o monitor para parar) */ // Flag que comunica o fim dos produtores para parar o monitor. | Dicionário: volatile = impede otimizações de cache de registadores pelo compilador; g_all_done = flag de término.
 
/* Número fixo de threads consumidoras */ // Comentário informativo sobre threads. | Dicionário: N/A
#define NUM_CONSUMERS 5 // Define a quantidade fixa de threads consumidoras como 5. | Dicionário: NUM_CONSUMERS = macro que define o número fixo de threads consumidoras.
 
/** // Início de comentário Doxygen para a função imprimir_relatorio. | Dicionário: / * * = início do bloco.
 * @brief Imprime o relatório final de métricas para o stdout. // Resumo da rotina de escrita do relatório. | Dicionário: stdout = fluxo de saída padrão.
 * // Linha de espaçamento. | Dicionário: N/A
 * @details // Detalhes da impressão com base no parâmetro modo. | Dicionário: @details = detalhes.
 * A secção a imprimir depende do argumento `modo`: // Explica a lógica condicional de apresentação. | Dicionário: N/A
 *  - `"security"`   → contagens de níveis DEBUG/INFO/WARN/ERROR/CRITICAL // Modos suportados. | Dicionário: N/A
 *  - `"performance"` → contagens de HTTP 4xx/5xx // Modos suportados. | Dicionário: HTTP = protocolo de transferência de hipertexto.
 *  - `"traffic"`    → contagens de HTTP 4xx/5xx (reutiliza os mesmos campos) // Modos suportados. | Dicionário: N/A
 *  - `"full"`       → todas as secções anteriores // Apresentação completa. | Dicionário: N/A
 * // Linha de espaçamento. | Dicionário: N/A
 * O top-10 de IPs é ordenado por bubble sort directamente sobre os arrays // Detalha o algoritmo de ordenação usado. | Dicionário: bubble sort = algoritmo de ordenação simples por trocas consecutivas; IP = protocolo de internet (endereço).
 * dentro de `m`, sem alocações adicionais. // Refere a eficiência de memória da ordenação. | Dicionário: alocações = reservas de memória dinâmica.
 * // Linha de espaçamento. | Dicionário: N/A
 * @param m    Apontador para a estrutura `Metrics` com os totais acumulados // Documenta o parâmetro m. | Dicionário: @param = parâmetro; Metrics = tipo da estrutura de contadores.
 *             por todos os consumidores. // Continuação do detalhe do parâmetro. | Dicionário: N/A
 * @param modo String que selecciona que secções do relatório são impressas. // Documenta o parâmetro modo. | Dicionário: N/A
 */ // Fim do comentário Doxygen. | Dicionário: */ = fim do comentário.
static void imprimir_relatorio(Metrics *m, char *modo) { // Declara função interna estática para gerar relatório estatístico. | Dicionário: static = visibilidade local; void = sem retorno; Metrics = estrutura de métricas; char = tipo de caractere.
    posix_writef(STDOUT_FILENO, "\n=== RELATORIO FINAL PRODCONS (%s) ===\n", modo); // Escreve o cabeçalho principal do relatório. | Dicionário: STDOUT_FILENO = descritor de saída padrão; posix_writef = escrita segura na consola.
    posix_writef(STDOUT_FILENO, "Total de linhas  : %ld\n", m->total_lines); // Escreve a quantidade total de linhas de logs lidas e analisadas. | Dicionário: total_lines = campo com total de linhas; %ld = formato para inteiro longo.
 
    /* Secção de segurança: mostra a distribuição de níveis de severidade */ // Comentário sobre a distribuição de logs. | Dicionário: N/A
    if (strcmp(modo, "security") == 0 || strcmp(modo, "full") == 0) { // Se o modo de relatório for segurança ou totalizador. | Dicionário: strcmp = função para comparar strings; if = condicional.
        posix_writef(STDOUT_FILENO, "\n--- EVENTOS DE SEGURANCA ---\n"); // Escreve o subtítulo da secção de segurança. | Dicionário: STDOUT_FILENO = descritor da consola.
        posix_writef(STDOUT_FILENO, "DEBUG            : %ld\n", m->count_debug); // Imprime a quantidade acumulada de logs em nível DEBUG. | Dicionário: count_debug = campo das métricas.
        posix_writef(STDOUT_FILENO, "INFO             : %ld\n", m->count_info); // Imprime a quantidade acumulada de logs em nível INFO. | Dicionário: count_info = campo das métricas.
        posix_writef(STDOUT_FILENO, "WARNINGS         : %ld\n", m->count_warn); // Imprime a quantidade acumulada de logs em nível WARNING. | Dicionário: count_warn = campo das métricas.
        posix_writef(STDOUT_FILENO, "ERRORS           : %ld\n", m->count_error); // Imprime a quantidade acumulada de logs em nível ERROR. | Dicionário: count_error = campo das métricas.
        posix_writef(STDOUT_FILENO, "CRITICAL         : %ld\n", m->count_critical); // Imprime a quantidade acumulada de logs em nível CRITICAL. | Dicionário: count_critical = campo das métricas.
    } // Fim do bloco condicional de segurança. | Dicionário: } = fecho de bloco.
 
    /* Secção de performance: mostra erros HTTP do lado do cliente e do servidor */ // Comentário informativo sobre HTTP. | Dicionário: N/A
    if (strcmp(modo, "performance") == 0 || strcmp(modo, "full") == 0) { // Se o modo for performance ou completo. | Dicionário: strcmp = comparação de vetores de caracteres.
        posix_writef(STDOUT_FILENO, "\n--- PERFORMANCE ---\n"); // Escreve o subtítulo da secção de performance. | Dicionário: N/A
        posix_writef(STDOUT_FILENO, "HTTP 4xx         : %ld\n", m->count_4xx); // Imprime o total de erros de cliente (HTTP 4xx). | Dicionário: count_4xx = campo acumulador de erros HTTP do cliente.
        posix_writef(STDOUT_FILENO, "HTTP 5xx         : %ld\n", m->count_5xx); // Imprime o total de erros de servidor (HTTP 5xx). | Dicionário: count_5xx = campo acumulador de erros HTTP do servidor.
    } // Fim do bloco condicional de performance. | Dicionário: } = fecho de bloco.
 
    /* Secção de tráfego: mostra os mesmos contadores de HTTP */ // Comentário sobre reutilização de campos. | Dicionário: N/A
    if (strcmp(modo, "traffic") == 0 || strcmp(modo, "full") == 0) { // Se o modo for tráfego ou completo. | Dicionário: strcmp = compara duas strings.
        posix_writef(STDOUT_FILENO, "\n--- TRAFEGO ---\n"); // Escreve o cabeçalho da secção de tráfego. | Dicionário: N/A
        posix_writef(STDOUT_FILENO, "HTTP 4xx         : %ld\n", m->count_4xx); // Imprime a contagem de pedidos com código 4xx. | Dicionário: count_4xx = campo.
        posix_writef(STDOUT_FILENO, "HTTP 5xx         : %ld\n", m->count_5xx); // Imprime a contagem de pedidos com código 5xx. | Dicionário: count_5xx = campo.
    } // Fim do bloco condicional de tráfego. | Dicionário: } = fecho de bloco.
 
    posix_writef(STDOUT_FILENO, "\n--- TOP 10 IPs ---\n"); // Imprime o cabeçalho da secção dos IPs mais frequentes. | Dicionário: STDOUT_FILENO = descritor de stdout.
 
    /* // Início de comentário interno explicativo sobre o bubble sort in-place. | Dicionário: N/A
     * Ordenação por bubble sort dos IPs por contagem descendente. // Lógica de ordenação clássica de array. | Dicionário: N/A
     * O sort é feito in-place nos arrays ip_count[] e ip_list[] da estrutura // Explica que não se usa memória extra temporária. | Dicionário: in-place = operação executada diretamente nos arrays originais.
     * Metrics — troca simultaneamente o contador e a string correspondente. // Explica a necessidade de sincronia na troca de dados. | Dicionário: Metrics = estrutura de estatísticas.
     */ // Fim do comentário interno. | Dicionário: N/A
    for (int i = 0; i < m->ip_num - 1; i++) { // Loop exterior do bubble sort que controla as passagens no vetor de IPs. | Dicionário: for = loop de repetição; ip_num = contagem de IPs únicos encontrados.
        for (int j = 0; j < m->ip_num - i - 1; j++) { // Loop interno que compara elementos vizinhos no vetor. | Dicionário: j = índice do loop interno.
            if (m->ip_count[j] < m->ip_count[j + 1]) { // Se a contagem do IP atual for menor que a do IP seguinte, faz a troca para obter ordem descendente. | Dicionário: ip_count = array contendo as contagens de acessos de cada IP.
                /* Trocar contadores */ // Fase 1 da troca (swap). | Dicionário: N/A
                long tmp_c = m->ip_count[j]; // Guarda temporariamente o contador do IP j. | Dicionário: tmp_c = variável temporária de contagem.
                m->ip_count[j] = m->ip_count[j + 1]; // Move a contagem maior (de j+1) para a posição anterior j. | Dicionário: N/A
                m->ip_count[j + 1] = tmp_c; // Restaura o contador menor na posição seguinte j+1. | Dicionário: N/A
                /* Trocar strings de IP correspondentes */ // Fase 2 da troca (swap). | Dicionário: N/A
                char tmp_ip[IP_LEN]; // Cria buffer temporário local para alojar a string de IP. | Dicionário: IP_LEN = tamanho da string reservada para IP; tmp_ip = buffer temporário.
                strncpy(tmp_ip, m->ip_list[j], IP_LEN); // Copia o IP da posição j para o buffer temporário. | Dicionário: strncpy = cópia segura de strings limitando o número de caracteres; ip_list = array bidimensional de caracteres que armazena IPs.
                strncpy(m->ip_list[j], m->ip_list[j + 1], IP_LEN); // Copia o IP da posição j+1 para a posição j. | Dicionário: N/A
                strncpy(m->ip_list[j + 1], tmp_ip, IP_LEN); // Restaura o IP copiado do buffer temporário para a posição j+1. | Dicionário: N/A
            } // Fim do if de troca. | Dicionário: } = fecho de bloco.
        } // Fim do loop interno. | Dicionário: } = fecho de bloco.
    } // Fim do loop de ordenação. | Dicionário: } = fecho de bloco.
 
    /* Limitar a saída aos primeiros 10 (ou menos, se houver menos IPs únicos) */ // Determinação do teto de amostragem. | Dicionário: N/A
    int limite = m->ip_num < 10 ? m->ip_num : 10; // Aplica operador condicional para definir o limite como no máximo 10. | Dicionário: limite = variável que limita o print; ip_num = total de IPs registados.
    if (limite == 0) { // Se não foi registado nenhum IP no parser. | Dicionário: if = condicional.
        posix_writef(STDOUT_FILENO, "Nenhum IP encontrado.\n"); // Informa no terminal que a lista está vazia. | Dicionário: posix_writef = escrita segura na consola.
    } else { // Caso existam IPs registados na estrutura. | Dicionário: else = caso contrário.
        for (int i = 0; i < limite; i++) // Ciclo para imprimir os IPs no ecrã. | Dicionário: i = índice.
            posix_writef(STDOUT_FILENO, "%2d. %-16s (%ld pedidos)\n", // Escreve o IP ordenado e a sua respetiva contagem de pedidos. | Dicionário: %2d = formato inteiro com 2 espaços de largura; %-16s = formato de string alinhada à esquerda com 16 caracteres.
                         i + 1, m->ip_list[i], m->ip_count[i]); // Parâmetros de impressão (posição, string do IP, contador do IP). | Dicionário: ip_list = array de IPs; ip_count = array de contadores.
    } // Fim do bloco if-else de listagem de IPs. | Dicionário: } = fecho de bloco.
 
    posix_writef(STDOUT_FILENO, "\n--- ALERTAS CRITICOS ---\n"); // Imprime o cabeçalho para os alertas críticos. | Dicionário: STDOUT_FILENO = descritor de stdout.
    if (m->num_alerts == 0) { // Se nenhum alerta crítico foi gerado pelas threads consumidoras. | Dicionário: num_alerts = contador de alertas presentes na estrutura de métricas.
        posix_writef(STDOUT_FILENO, "Sem alertas criticos.\n"); // Escreve que não foram detetados problemas graves. | Dicionário: posix_writef = escrita síncrona.
    } else { // Se existirem alertas registados na estrutura. | Dicionário: else = caso contrário.
        for (int i = 0; i < m->num_alerts; i++) // Itera sobre cada um dos alertas presentes na lista. | Dicionário: num_alerts = contagem de alertas.
            posix_writef(STDOUT_FILENO, "%2d. %s\n", i + 1, m->alerts[i]); // Imprime o índice e o texto do alerta. | Dicionário: alerts = array contendo as mensagens de alertas críticos.
    } // Fim do bloco if-else de alertas. | Dicionário: } = fecho de bloco.
 
    posix_writef(STDOUT_FILENO, "=================================\n"); // Imprime a linha final de fecho do relatório. | Dicionário: STDOUT_FILENO = saída padrão.
} // Fim da função imprimir_relatorio. | Dicionário: } = fecho de escopo da função.
 
/** // Início de comentário Doxygen para a função draw_dashboard. | Dicionário: / * * = início do bloco.
 * @brief Redesenha o dashboard de progresso no terminal. // Resumo da rotina de desenho gráfico. | Dicionário: N/A
 * // Linha de espaçamento. | Dicionário: N/A
 * @details // Detalhes sobre sequências ANSI e cálculos de progresso global. | Dicionário: @details = detalhes.
 * Usa sequências de escape ANSI para subir `linhas` linhas e limpar a área, // Explica o posicionamento do cursor no terminal. | Dicionário: sequências de escape ANSI = comandos de texto especiais para controlar o aspeto do terminal.
 * depois imprime uma barra de progresso por produtor e uma barra global. // Explica a disposição visual das barras. | Dicionário: N/A
 * É chamada periodicamente pelo thread monitor e uma última vez depois de // Indica quem invoca esta rotina. | Dicionário: monitor = thread que acompanha o progresso e desenha na consola.
 * `g_all_done = 1` para mostrar 100% antes do relatório. // Justifica a chamada final de fecho. | Dicionário: g_all_done = flag boleana de conclusão de tarefas.
 * // Linha de espaçamento. | Dicionário: N/A
 * Não recebe parâmetros — lê directamente as variáveis globais // Descreve a dependência de dados globais. | Dicionário: N/A
 * `g_bytes_done`, `g_bytes_total`, `g_num_workers` e `g_start_time`. // Lista as variáveis globais utilizadas. | Dicionário: N/A
 */ // Fim do comentário Doxygen. | Dicionário: */ = fim do comentário.
static void draw_dashboard(void) { // Declara função estática local que desenha o painel gráfico de progresso das threads. | Dicionário: static = visibilidade limitada ao arquivo; void = sem parâmetros de entrada e sem retorno.
    /* Subir o cursor o número de linhas ocupado pelo dashboard anterior */ // Comentário informativo de controlo do cursor ANSI. | Dicionário: N/A
    int linhas = g_num_workers + 8; // Calcula o total de linhas ocupadas pela moldura e produtores. | Dicionário: g_num_workers = quantidade de threads produtoras.
    posix_writef(STDOUT_FILENO, "\033[%dA", linhas);  /* ESC[<n>A — cursor up */ // Envia sequência ANSI de cursor up para posicionar a escrita. | Dicionário: posix_writef = escrita segura; \033[%dA = código ANSI para subir o cursor.
    posix_writef(STDOUT_FILENO, "\033[J");             /* ESC[J   — limpar até ao fim do ecrã */ // Envia sequência ANSI para apagar texto antigo debaixo do cursor. | Dicionário: \033[J = código ANSI para limpar o terminal a partir da posição atual do cursor.
 
    /* Calcular progresso global somando bytes de todos os produtores */ // Comentário explicativo sobre cálculo cumulativo. | Dicionário: N/A
    long total_done  = 0; // Cria acumulador para somar os bytes lidos por todos os produtores. | Dicionário: total_done = contador local acumulado de bytes feitos.
    long total_total = 0; // Cria acumulador para somar os bytes totais de quotas de todos os produtores. | Dicionário: total_total = contador local de bytes totais.
    for (int i = 0; i < g_num_workers; i++) { // Itera pelos slots de dados de cada produtor. | Dicionário: for = loop; g_num_workers = número total de workers ativos.
        total_done  += g_bytes_done[i]; // Soma os bytes já processados pelo produtor i. | Dicionário: g_bytes_done = array de progresso individual por produtor.
        total_total += g_bytes_total[i]; // Soma a quota total de bytes do produtor i. | Dicionário: g_bytes_total = array de quotas individuais.
    } // Fim do ciclo de acumulação. | Dicionário: } = fecho de bloco.
    int total_pct = (total_total > 0) ? (int)(total_done * 100 / total_total) : 0; // Calcula a percentagem global, evitando divisões por zero. | Dicionário: ? : = operador condicional ternário; total_pct = percentagem total calculada.
    if (total_pct > 100) total_pct = 100;  /* Garantir que não ultrapassa 100% */ // Clampa o valor caso haja pequenos desvios acima do teto de bytes. | Dicionário: total_pct = percentagem global.
 
    posix_writef(STDOUT_FILENO, "╔══════════════════════════════════════════╗\n"); // Imprime o bordo superior da caixa de monitorização. | Dicionário: STDOUT_FILENO = descritor de stdout.
    posix_writef(STDOUT_FILENO, "║   LOG ANALYZER - PRODCONS MONITOR        ║\n"); // Imprime o título da caixa de monitorização. | Dicionário: N/A
    posix_writef(STDOUT_FILENO, "╠══════════════════════════════════════════╣\n"); // Imprime a linha divisória superior da caixa. | Dicionário: N/A
 
    /* Uma linha por produtor com barra de progresso individual */ // Processamento visual iterativo. | Dicionário: N/A
    for (int i = 0; i < g_num_workers; i++) { // Percorre a lista de produtores. | Dicionário: for = loop de repetição; g_num_workers = número de workers.
        int pct = (g_bytes_total[i] > 0) // Calcula a percentagem individual se houver quota atribuída. | Dicionário: N/A
                  ? (int)(g_bytes_done[i] * 100 / g_bytes_total[i]) : 0; // Realiza a divisão inteira de bytes feitos pelo total. | Dicionário: g_bytes_done = progresso; g_bytes_total = total.
        if (pct > 100) pct = 100; // Clampa o limite individual a 100%. | Dicionário: N/A
 
        /* Barra de 20 caracteres: '#' preenchido, '.' vazio */ // Criação de string da barra gráfica. | Dicionário: N/A
        char bar[21]; // Reserva espaço local para a string de caracteres e terminação nula. | Dicionário: bar = array de caracteres local.
        int filled = pct / 5;  /* Cada '#' representa 5% */ // Define quantos '#' serão mostrados (cada um equivale a 5%). | Dicionário: filled = contagem de marcas cheias.
        for (int b = 0; b < 20; b++) bar[b] = (b < filled) ? '#' : '.'; // Preenche com '#' as posições completas e '.' as restantes. | Dicionário: for = ciclo; b = índice; bar = string.
        bar[20] = '\0'; // Insere o terminador de string C na última posição. | Dicionário: '\0' = caractere de fim de string.
 
        posix_writef(STDOUT_FILENO, "║ Produtor %-2d [%s] %3d%%        ║\n", // Imprime a linha formatada com o progresso do produtor i. | Dicionário: %-2d = inteiro com 2 posições alinhadas à esquerda; %3d%% = percentagem formatada com 3 dígitos.
                     i + 1, bar, pct); // Valores para substituição (índice legível 1-based, barra textual, percentagem). | Dicionário: bar = string da barra; pct = percentagem individual.
    } // Fim do loop de desenho individual. | Dicionário: } = fecho de bloco.
 
    posix_writef(STDOUT_FILENO, "╠══════════════════════════════════════════╣\n"); // Imprime a linha divisória intermédia da caixa. | Dicionário: STDOUT_FILENO = descritor de stdout.
 
    /* Barra de progresso global (soma de todos os produtores) */ // Montagem gráfica global. | Dicionário: N/A
    char tot_bar[21]; // Reserva espaço local para a string da barra de progresso acumulado. | Dicionário: tot_bar = array temporário.
    int tot_filled = total_pct / 5; // Determina quantos caracteres '#' representam o avanço global. | Dicionário: tot_filled = contagem de marcas cheias do progresso geral.
    for (int b = 0; b < 20; b++) tot_bar[b] = (b < tot_filled) ? '#' : '.'; // Popula a barra global com caracteres '#' ou '.'. | Dicionário: for = loop.
    tot_bar[20] = '\0'; // Termina a string C. | Dicionário: '\0' = terminador de string.
 
    /* Calcular tempo decorrido desde o início */ // Medição temporal. | Dicionário: N/A
    time_t elapsed = time(NULL) - g_start_time; // Calcula o tempo em segundos desde o arranque da aplicação. | Dicionário: time = função que lê a hora atual; g_start_time = timestamp inicial; elapsed = intervalo temporal.
    int hh = (int)(elapsed / 3600); // Converte segundos para horas. | Dicionário: hh = horas.
    int mm = (int)((elapsed % 3600) / 60); // Converte os segundos restantes para minutos. | Dicionário: mm = minutos.
    int ss = (int)(elapsed % 60); // Obtém os segundos que restam. | Dicionário: ss = segundos.
 
    posix_writef(STDOUT_FILENO, "║ Total     [%s] %3d%%           ║\n", // Imprime a linha de progresso geral. | Dicionário: N/A
                 tot_bar, total_pct); // Passa a barra de progresso geral e a respetiva percentagem total. | Dicionário: tot_bar = barra total; total_pct = percentagem total.
    posix_writef(STDOUT_FILENO, "║ Elapsed: %02d:%02d:%02d                      ║\n", // Imprime o tempo decorrido formatado no estilo relógio HH:MM:SS. | Dicionário: %02d = inteiro com 2 dígitos e preenchimento de zeros à esquerda se necessário.
                 hh, mm, ss); // Passa os valores de horas, minutos e segundos. | Dicionário: hh = horas; mm = minutos; ss = segundos.
    posix_writef(STDOUT_FILENO, "╚══════════════════════════════════════════╝\n"); // Imprime o bordo inferior da caixa do dashboard. | Dicionário: STDOUT_FILENO = descritor de stdout.
} // Fim da função draw_dashboard. | Dicionário: } = fecho de escopo da função.
 
/** // Início de comentário Doxygen para run_monitor_thread. | Dicionário: / * * = início do bloco.
 * @brief Thread dedicado à actualização periódica do dashboard. // Resumo da rotina da thread monitora. | Dicionário: N/A
 * // Linha de espaçamento. | Dicionário: N/A
 * @details // Detalhes da rotina com loop temporal de 100 ms. | Dicionário: @details = detalhes.
 * Corre em loop enquanto `g_all_done` for 0, redesenhando o dashboard a cada // Explica o critério de manutenção ativa do loop. | Dicionário: N/A
 * 100 ms (100 000 µs). Quando `g_all_done` passa a 1 (após todos os // Especifica a cadência e o tempo de repouso por ciclo. | Dicionário: µs = microsegundos.
 * produtores terminarem), faz um último redesenho a 100% e termina. // Explica o desenho final após fecho de produtores. | Dicionário: N/A
 * // Linha de espaçamento. | Dicionário: N/A
 * @param arg  Não utilizado (NULL). // Documenta o argumento. | Dicionário: @param = parâmetro.
 * @return     NULL (convenção pthread). // Documenta o retorno da thread. | Dicionário: @return = retorno.
 */ // Fim do comentário Doxygen. | Dicionário: */ = fim do comentário.
void *run_monitor_thread(void *arg) { // Ponto de entrada da thread monitora que retorna ponteiro genérico. | Dicionário: void* = tipo de ponteiro genérico; run_monitor_thread = nome da função.
    (void)arg;  /* Suprimir aviso de parâmetro não usado */ // Silencia avisos do compilador de que a variável arg não é usada no corpo da função. | Dicionário: (void) = conversão de tipo para silenciar avisos; arg = parâmetro.
    while (!g_all_done) { // Corre repetidamente enquanto a flag g_all_done for zero. | Dicionário: while = ciclo de repetição; ! = operador de negação lógica; g_all_done = flag indicadora de fim dos produtores.
        draw_dashboard(); // Desenha a tabela com as barras de progresso atualizadas na consola. | Dicionário: draw_dashboard = função de renderização.
        usleep(100000);  /* Pausa de 100 ms entre redesenhos */ // Suspende a execução da thread monitor por 100.000 microsegundos (100 milisegundos). | Dicionário: usleep = chamada POSIX para suspender a thread por um intervalo em microsegundos.
    } // Fim do ciclo. | Dicionário: } = fecho de loop.
    /* Redesenho final para mostrar 100% após todos os produtores terminarem */ // Correção visual pós-produção. | Dicionário: N/A
    draw_dashboard(); // Executa o dashboard uma última vez para garantir que as barras se fixam no estado de completado. | Dicionário: draw_dashboard = redesenha o painel.
    pthread_exit(NULL); // Termina a execução da thread corrente de forma limpa. | Dicionário: pthread_exit = chamada POSIX para encerrar a própria thread; NULL = valor de retorno.
} // Fim da função run_monitor_thread. | Dicionário: } = fecho de escopo da função.
 
/** // Início de comentário Doxygen para o main. | Dicionário: / * * = início do bloco.
 * @brief Ponto de entrada do programa. // Resumo do orquestrador principal do analisador de logs. | Dicionário: N/A
 * // Linha de espaçamento. | Dicionário: N/A
 * @details // Detalhes passo a passo do fluxo principal executado pelo main(). | Dicionário: @details = detalhes.
 * Fluxo de execução: // Descrição geral do encadeamento lógico de passos. | Dicionário: N/A
 *  1. Validar argumentos e configurar o modo de análise. // Passo 1 do programa. | Dicionário: N/A
 *  2. Varrer o directório e recolher todos os ficheiros `.log` e `.json`. // Passo 2 do programa. | Dicionário: N/A
 *  3. Calcular o total de bytes e dividir em fatias iguais (uma por produtor). // Passo 3 do programa. | Dicionário: N/A
 *  4. Inicializar o **bounded buffer** (`init_bounded_buffer`) e definir // Passo 4 do programa. | Dicionário: init_bounded_buffer = inicia a fila concorrente.
 *     `produtores_ativos = num_prod`. // Define a contagem inicial de produtores ativos. | Dicionário: produtores_ativos = contador; num_prod = número total de produtores.
 *  5. Lançar o thread monitor e os threads produtores. // Passo 5 do programa. | Dicionário: N/A
 *  6. Lançar os threads consumidores. // Passo 6 do programa. | Dicionário: N/A
 *  7. Aguardar (`pthread_join`) todos os produtores; sinalizar o monitor para // Passo 7 do programa. | Dicionário: pthread_join = espera fim de thread.
 *     parar; aguardar todos os consumidores. // Sincronização e encerramento. | Dicionário: N/A
 *  8. Destruir o buffer, imprimir o relatório e libertar memória. // Passo 8 do programa. | Dicionário: N/A
 * // Linha de espaçamento. | Dicionário: N/A
 * @param argc  Número de argumentos da linha de comandos. // Parâmetro argc. | Dicionário: @param = parâmetro; argc = contagem de argumentos na linha de comandos.
 * @param argv  Vetor de argumentos: // Parâmetro argv. | Dicionário: argv = vetor de strings com os argumentos da linha de comandos.
 *              `argv[1]` — directório com os ficheiros de log, // Argumento 1. | Dicionário: N/A
 *              `argv[2]` — número de produtores, // Argumento 2. | Dicionário: N/A
 *              `argv[3]` — modo (`security` | `performance` | `traffic` | `full`), // Argumento 3. | Dicionário: N/A
 *              `argv[4]` — (opcional) `--verbose`. // Argumento 4. | Dicionário: N/A
 * @return 0 em caso de sucesso; 1 em caso de erro. // Valor devolvido ao sistema operativo. | Dicionário: @return = valor de retorno.
 */ // Fim do comentário Doxygen. | Dicionário: */ = fim do comentário.
int main(int argc, char *argv[]) { // Ponto de partida do processo executável em C. | Dicionário: int = tipo de retorno (inteiro); main = nome do ponto de entrada; argc = quantidade de argumentos; argv = array de strings dos argumentos.
    if (argc < 4) { // Se a quantidade de argumentos introduzidos for menor que a necessária (mínimo 4). | Dicionário: if = condicional; < = menor que.
        posix_writef(STDOUT_FILENO, // Imprime mensagem indicadora de como utilizar o executável. | Dicionário: STDOUT_FILENO = descritor de saída padrão; posix_writef = escrita segura baseada no write() do POSIX.
                     "Uso: %s <diretorio> <num_produtores> <modo> [--verbose]\n", // String indicadora de uso. | Dicionário: N/A
                     argv[0]); // Nome do próprio programa executado (primeiro elemento dos argumentos). | Dicionário: argv = array de strings.
        exit(1); // Aborta a execução do programa devolvendo o código de erro 1 ao sistema operativo. | Dicionário: exit = chamada padrão de término do processo.
    } // Fim do bloco condicional. | Dicionário: } = fecho de bloco.
 
    char *diretorio = argv[1]; // Aponta para a string com o caminho do diretório contendo logs. | Dicionário: char = tipo de caractere; diretorio = variável contendo a string de caminho.
    int   num_prod  = atoi(argv[2]); // Converte a string com o número de produtores para inteiro. | Dicionário: int = tipo inteiro; num_prod = número de produtores; atoi = função utilitária para converter strings ASCII em inteiros.
    char *modo      = argv[3]; // Aponta para a string que dita o modo de exibição das métricas. | Dicionário: modo = string com o tipo de análise.
    int   verbose   = 0; // Cria variável inteira como flag boleana indicando o modo detalhado, inicialmente falsa. | Dicionário: verbose = variável flag.
 
    /* Verificar se o utilizador pediu saída detalhada */ // Pesquisa de flags adicionais nos argumentos passados. | Dicionário: N/A
    for (int i = 4; i < argc; i++) // Itera sobre os argumentos extra. | Dicionário: for = loop de repetição; i = índice; argc = contagem de argumentos.
        if (strcmp(argv[i], "--verbose") == 0) verbose = 1; // Se encontrar a string correspondente, liga a flag verbose. | Dicionário: strcmp = compara vetores de caracteres; verbose = flag.
 
    /* Configurar o parser com o modo escolhido; abortar se for inválido */ // Inicialização das configurações do parser de logs. | Dicionário: N/A
    if (parser_set_mode_from_string(modo) != 0) { // Tenta carregar o modo de parser. Se falhar (retorno diferente de 0). | Dicionário: parser_set_mode_from_string = função do parser.h que configura filtros de logs baseados na string; != = diferente de.
        posix_writef(STDERR_FILENO, // Imprime a mensagem informando que o modo passado é inválido. | Dicionário: STDERR_FILENO = descritor de erro padrão; posix_writef = escrita segura.
                     "Modo invalido: %s (use security|performance|traffic|full)\n", modo); // Texto explicativo. | Dicionário: N/A
        exit(1); // Encerra a aplicação retornando o código de erro 1. | Dicionário: exit = encerra processo.
    } // Fim do bloco condicional. | Dicionário: } = fecho de bloco.
 
    /* ── 1. Descobrir ficheiros ── */ // Comentário da primeira fase: listagem de pasta. | Dicionário: N/A
    int   capacidade = 10, total_ficheiros = 0; // Define a capacidade inicial do array dinâmico de caminhos e zera o contador. | Dicionário: capacidade = limite atual do vetor; total_ficheiros = contagem de ficheiros.
    char **ficheiros = malloc(capacidade * sizeof(char *)); // Aloca memória dinâmica na heap para guardar a lista de strings (vetor de ponteiros). | Dicionário: char** = ponteiro para ponteiro de caracteres (array de strings); malloc = reserva memória na heap; sizeof = retorna tamanho em bytes.
    if (!ficheiros) { perror("malloc"); exit(1); } // Se a alocação de memória falhar, reporta erro e encerra. | Dicionário: ! = negação lógica; perror = imprime o erro do sistema operativo.
 
    DIR *dir = opendir(diretorio); // Abre a pasta com o caminho indicado para iniciar a travessia. | Dicionário: DIR = tipo representativo de diretório; opendir = chamada do sistema para abrir diretórios.
    if (!dir) { perror("opendir"); exit(1); } // Se a pasta não puder ser aberta (inexistente ou sem acessos), reporta erro e encerra. | Dicionário: dir = pasta aberta.
 
    /* Iterar as entradas do directório e guardar os caminhos completos */ // Comentário informativo sobre o varrimento das pastas. | Dicionário: N/A
    struct dirent *entrada; // Declara ponteiro para a estrutura que representa um ficheiro ou subpasta na travessia. | Dicionário: struct dirent = estrutura POSIX para entrada de diretório.
    while ((entrada = readdir(dir)) != NULL) { // Lê cada ficheiro/pasta e mantém o ciclo ativo até atingir o fim da listagem. | Dicionário: while = ciclo; readdir = chamada para obter a próxima entrada da pasta; NULL = ponteiro nulo.
        int len = strlen(entrada->d_name); // Mede a extensão em caracteres do nome do ficheiro atual. | Dicionário: len = comprimento; strlen = função que calcula tamanho de string; d_name = campo de nome de ficheiro na estrutura dirent.
        int e_log  = (len > 4 && strcmp(entrada->d_name + len - 4, ".log")  == 0); // Verifica se os últimos 4 caracteres do nome coincidem com a extensão ".log". | Dicionário: e_log = flag boleana que indica extensão .log.
        int e_json = (len > 5 && strcmp(entrada->d_name + len - 5, ".json") == 0); // Verifica se os últimos 5 caracteres do nome coincidem com a extensão ".json". | Dicionário: e_json = flag boleana que indica extensão .json.
        if (!e_log && !e_json) continue;  /* Ignorar ficheiros que não sejam .log ou .json */ // Se não for ficheiro com extensão suportada, salta para o ficheiro seguinte. | Dicionário: continue = reinicia o ciclo de repetição.
 
        /* Redimensionar o array dinamicamente se necessário (crescimento x2) */ // Otimização de armazenamento da lista na heap. | Dicionário: N/A
        if (total_ficheiros == capacidade) { // Se atingir o limite atual de armazenamento dinâmico de strings. | Dicionário: N/A
            capacidade *= 2; // Duplica a capacidade limite do array dinâmico. | Dicionário: *= = operador de atribuição multiplicativa.
            ficheiros = realloc(ficheiros, capacidade * sizeof(char *)); // Realoca o bloco na heap expandindo o tamanho do vetor físico. | Dicionário: realloc = expande/reduz o bloco de memória dinâmica alocado.
            if (!ficheiros) { perror("realloc"); exit(1); } // Se falhar a realocação, reporta erro e aborta. | Dicionário: realloc = realocação.
        } // Fim do if de redimensionamento. | Dicionário: } = fecho de bloco.
        char caminho[512]; // Buffer local temporário para formar o caminho completo absoluto/relativo de cada ficheiro. | Dicionário: caminho = array de caracteres local; 512 = tamanho de buffer reservado.
        snprintf(caminho, sizeof(caminho), "%s/%s", diretorio, entrada->d_name); // Formata e concatena a pasta e o nome do ficheiro. | Dicionário: snprintf = escrita formatada segura limitando número de bytes.
        ficheiros[total_ficheiros++] = strdup(caminho);  /* Cópia própria do caminho */ // Cria uma cópia da string na heap e guarda o endereço no array dinâmico. | Dicionário: strdup = duplica string alocando memória nova; total_ficheiros++ = incrementa contagem.
    } // Fim do loop de travessia do diretório. | Dicionário: } = fecho de bloco.
    closedir(dir); // Encerra o acesso à pasta aberta no sistema operativo e liberta recursos de leitura. | Dicionário: closedir = fecha pasta de diretório.
 
    if (total_ficheiros == 0) { // Se não foi encontrado nenhum ficheiro com as extensões suportadas. | Dicionário: total_ficheiros = contagem de logs.
        posix_writef(STDOUT_FILENO, "Nenhum ficheiro .log ou .json encontrado.\n"); // Informa o utilizador no terminal. | Dicionário: posix_writef = escrita segura.
        free(ficheiros); // Desaloca a memória do vetor de strings dinâmica inicial para evitar fugas. | Dicionário: free = desaloca memória dinâmica na heap.
        exit(0); // Termina a aplicação sem erros (código zero). | Dicionário: exit = termina processo.
    } // Fim do bloco condicional de pasta vazia. | Dicionário: } = fecho de bloco.
 
    /* Limitar o número de produtores ao máximo suportado */ // Garantia de limites de execução concorrente. | Dicionário: N/A
    if (num_prod > MAX_WORKERS) num_prod = MAX_WORKERS; // Reduz a contagem se o utilizador pedir mais threads do que o teto suportado. | Dicionário: MAX_WORKERS = constante de limite.
 
    /* ── 2. Calcular total de bytes e dividir em fatias iguais ── */ // Comentário da segunda fase de partição de trabalho. | Dicionário: N/A
    /* // Início de comentário interno sobre fatiamento de dados. | Dicionário: N/A
     * Cada produtor fica responsável por uma fatia de [byte_inicio, byte_fim[ // Explica o mapeamento da quota. | Dicionário: N/A
     * do espaço de endereçamento virtual dos ficheiros concatenados. // Explica a abstração linear do espaço de bytes. | Dicionário: N/A
     * O produtor trata de mapear essa fatia para os ficheiros reais. // Explica a responsabilidade local do worker. | Dicionário: N/A
     */ // Fim do comentário. | Dicionário: N/A
    struct stat st; // Variável para ler metadados dos ficheiros da lista. | Dicionário: struct stat = estrutura que contém tamanho do ficheiro e metadados.
    off_t total_bytes = 0; // Acumulador do tamanho conjunto de todos os ficheiros a processar. | Dicionário: total_bytes = tamanho acumulado.
    for (int i = 0; i < total_ficheiros; i++) { // Percorre a lista de ficheiros registados. | Dicionário: for = loop.
        if (stat(ficheiros[i], &st) == 0) // Obtém os metadados do ficheiro atual. Se tiver sucesso (retorno igual a zero). | Dicionário: stat = lê metadados.
            total_bytes += st.st_size; // Adiciona o tamanho físico em bytes ao acumulador total. | Dicionário: st_size = campo de tamanho na estrutura stat.
    } // Fim do loop de medição. | Dicionário: } = fecho de bloco.
 
    off_t bytes_por_prod = total_bytes / num_prod;  /* Fatia de bytes por produtor */ // Calcula a quota aproximada em bytes para cada produtor. | Dicionário: bytes_por_prod = tamanho da fatia de cada thread.
 
    posix_writef(STDOUT_FILENO, // Imprime informações gerais de arranque do analisador de logs. | Dicionário: posix_writef = escrita formatada segura.
                 "Ficheiros: %d | Produtores: %d | Consumidores: %d | Modo: %s\n\n", // Padrão textual. | Dicionário: N/A
                 total_ficheiros, num_prod, NUM_CONSUMERS, modo); // Parâmetros (número de ficheiros, produtores, consumidores, modo). | Dicionário: NUM_CONSUMERS = macro; modo = string.
 
    /* ── 3. Inicializar buffer e métricas ── */ // Terceira fase do fluxo de inicialização concorrente. | Dicionário: N/A
    /* // Início de comentário interno descritivo da inicialização. | Dicionário: N/A
     * init_bounded_buffer() inicializa: // Detalhes da rotina do buffer circular. | Dicionário: N/A
     *   - buffer.in = buffer.out = buffer.count = 0 // Inicializa os ponteiros. | Dicionário: N/A
     *   - pthread_mutex_init(&buffer.mutex)      → exclusão mútua no acesso ao buffer // Inicializa o mutex do buffer. | Dicionário: N/A
     *   - sem_init(&buffer.empty, 0, BUFFER_SIZE) → conta espaços livres (começa cheio de espaços) // Inicializa o semáforo de posições vazias. | Dicionário: N/A
     *   - sem_init(&buffer.full,  0, 0)           → conta entradas disponíveis (começa a zero) // Inicializa o semáforo de dados ativos. | Dicionário: N/A
     * // Linha de espaçamento. | Dicionário: N/A
     * produtores_ativos é usado pelos consumidores para saber quando parar: // Lógica de paragem. | Dicionário: N/A
     * quando chega a 0 e o buffer está vazio, não chegará mais nenhuma entrada. // Justificação da condição. | Dicionário: N/A
     */ // Fim do comentário. | Dicionário: N/A
    init_bounded_buffer(); // Cria e inicializa os semáforos e o mutex do buffer circular. | Dicionário: init_bounded_buffer = inicializa a fila.
    produtores_ativos = num_prod;  /* Número de produtores em execução */ // Inicializa o contador partilhado com a quantidade total de produtores que serão lançados. | Dicionário: produtores_ativos = global de produtores ativos.
 
    Metrics global_metrics; // Declara a estrutura global que guardará as métricas conjuntas. | Dicionário: Metrics = tipo da estrutura de métricas.
    init_metrics(&global_metrics);  /* Zerrar todos os contadores das métricas */ // Inicia todos os contadores e listas das métricas a zero. | Dicionário: init_metrics = função para inicializar contadores a zero.
 
    g_num_workers = num_prod; // Configura o total de produtores a monitorizar no dashboard global. | Dicionário: g_num_workers = global de contagem de workers.
    g_start_time  = time(NULL); // Regista o timestamp do arranque físico das threads. | Dicionário: time = relógio Unix.
    g_all_done    = 0; // Garante que a flag de paragem do monitor arranca desativada (zero). | Dicionário: g_all_done = flag de paragem.
    memset(g_bytes_done,  0, sizeof(g_bytes_done)); // Preenche o array de progresso individual com zeros. | Dicionário: memset = inicializa bloco de memória com um valor específico.
    memset(g_bytes_total, 0, sizeof(g_bytes_total)); // Preenche o array de quotas individuais com zeros. | Dicionário: memset = limpa memória.
 
    /* Reservar linhas no terminal para o dashboard poder redesenhar no lugar */ // Preparação do ecrã para evitar quebras. | Dicionário: N/A
    for (int i = 0; i < g_num_workers + 8; i++) // Ciclo para empurrar a saída padrão com quebras de linha em branco. | Dicionário: N/A
        posix_writef(STDOUT_FILENO, "\n"); // Escreve linha em branco. | Dicionário: STDOUT_FILENO = descritor de stdout.
 
    /* Lançar o thread monitor que actualizará o dashboard a cada 100 ms */ // Arranque da thread visual. | Dicionário: N/A
    pthread_t monitor_thread; // Cria variável para guardar o identificador da thread do monitor. | Dicionário: pthread_t = tipo identificador de thread POSIX; monitor_thread = identificador.
    pthread_create(&monitor_thread, NULL, run_monitor_thread, NULL); // Cria a thread do monitor passando a respetiva função de desenho de progresso. | Dicionário: pthread_create = chamada POSIX de criação de thread; run_monitor_thread = função alvo; NULL = atributos por omissão / argumento nulo.
 
    /* ── 4. Lançar produtores com fatias de bytes ── */ // Quarta fase: arranque de produtores concorrentes. | Dicionário: N/A
    /* // Início de comentário sobre argumentos dos produtores. | Dicionário: N/A
     * Cada ProducerArgs define a fatia [byte_inicio, byte_fim[ que o produtor // Explica a quota passada na estrutura de argumentos. | Dicionário: N/A
     * deve processar. O último produtor recebe os bytes restantes para cobrir // Tratamento especial de arredondamentos de divisão. | Dicionário: N/A
     * eventuais erros de arredondamento da divisão inteira. // Corrige o resto. | Dicionário: N/A
     */ // Fim do comentário. | Dicionário: N/A
    pthread_t    *prod_threads = malloc(num_prod * sizeof(pthread_t)); // Aloca array para armazenar identificadores de threads produtoras na heap. | Dicionário: prod_threads = vetor dinâmico de pthread_t.
    ProducerArgs *prod_args    = malloc(num_prod * sizeof(ProducerArgs)); // Aloca array para armazenar as estruturas de parâmetros de cada produtor. | Dicionário: ProducerArgs = estrutura de argumentos; prod_args = vetor dinâmico.
    if (!prod_threads || !prod_args) { perror("malloc"); exit(1); } // Se alguma alocação dinâmica na heap falhar, reporta erro e aborta. | Dicionário: ! = negação.
 
    for (int i = 0; i < num_prod; i++) { // Loop de criação física das threads produtoras. | Dicionário: i = índice.
        prod_args[i].ficheiros       = ficheiros; // Passa a referência da lista completa de ficheiros de logs a cada produtor. | Dicionário: ficheiros = array de caminhos.
        prod_args[i].total_ficheiros = total_ficheiros; // Passa a contagem de ficheiros a analisar. | Dicionário: total_ficheiros = total de logs.
        prod_args[i].byte_inicio     = (off_t)i * bytes_por_prod; // Define o ponto inicial absoluto da fatia para o produtor i. | Dicionário: byte_inicio = offset inicial.
        /* O último produtor estende a fatia até ao fim para cobrir o resto */ // Explicação de correção de teto final. | Dicionário: N/A
        prod_args[i].byte_fim        = (i == num_prod - 1) ? total_bytes // Se for o último produtor, estende a fatia até à última posição total_bytes. | Dicionário: total_bytes = total de bytes agregados.
                                                            : (off_t)(i + 1) * bytes_por_prod; // Caso contrário, termina no início da quota do produtor seguinte. | Dicionário: N/A
        prod_args[i].worker_index    = i; // Identifica de forma numérica o índice (0-based) deste produtor. | Dicionário: worker_index = identificador.
        prod_args[i].verbose         = verbose; // Passa o parâmetro boleano do modo verboso de log. | Dicionário: verbose = modo detalhado.
        prod_args[i].bytes_done      = &g_bytes_done[i];   /* Ponteiro para o slot de progresso */ // Associa o endereço do slot do array de monitorização de progresso. | Dicionário: bytes_done = apontador de avanço.
        prod_args[i].bytes_total     = &g_bytes_total[i]; // Associa o endereço do slot de quota total. | Dicionário: bytes_total = apontador de total.
 
        if (pthread_create(&prod_threads[i], NULL, run_producer, &prod_args[i]) != 0) { // Cria a thread i passando a função de produção. Se falhar (retorno diferente de zero). | Dicionário: pthread_create = cria thread; run_producer = função produtora.
            perror("pthread_create produtor"); // Reporta o erro de criação ocorrido no sistema operativo. | Dicionário: perror = erro padrão.
            exit(1); // Encerra a aplicação retornando o código de erro 1. | Dicionário: exit = aborta.
        } // Fim do bloco if. | Dicionário: } = fecho de bloco.
    } // Fim do ciclo de criação de produtores. | Dicionário: } = fecho de bloco.
 
    /* ── 5. Lançar consumidores ── */ // Quinta fase: arranque de consumidores concorrentes. | Dicionário: N/A
    /* // Início de comentário descritivo dos consumidores. | Dicionário: N/A
     * Todos os consumidores partilham a mesma estrutura global_metrics, // Explica a partilha de métricas entre threads. | Dicionário: N/A
     * protegida por metrics_mutex para evitar condições de corrida nas // Explica a utilidade do trinco exclusivo. | Dicionário: N/A
     * actualizações dos contadores. // Explica o perigo de race condition. | Dicionário: N/A
     */ // Fim do comentário. | Dicionário: N/A
    pthread_t    *cons_threads = malloc(NUM_CONSUMERS * sizeof(pthread_t)); // Reservado espaço na heap para armazenar identificadores das threads consumidoras. | Dicionário: cons_threads = vetor dinâmico de identificadores.
    ConsumerArgs *cons_args    = malloc(NUM_CONSUMERS * sizeof(ConsumerArgs)); // Reservado espaço na heap para as estruturas de argumentos das threads consumidoras. | Dicionário: ConsumerArgs = estrutura de argumentos; cons_args = vetor de argumentos.
    if (!cons_threads || !cons_args) { perror("malloc"); exit(1); } // Se falhar a reserva na heap, avisa erro e aborta. | Dicionário: ! = negação lógica.
 
    for (int i = 0; i < NUM_CONSUMERS; i++) { // Loop de criação física das threads consumidoras. | Dicionário: NUM_CONSUMERS = contagem fixa.
        cons_args[i].global_metrics = &global_metrics; // Associa a referência da estrutura global de contagem de estatísticas. | Dicionário: global_metrics = endereço de estatísticas.
        cons_args[i].metrics_mutex  = &metrics_mutex;  /* Mutex partilhado para as métricas */ // Associa o mutex dedicado de exclusão mútua das métricas. | Dicionário: metrics_mutex = endereço de mutex de métricas.
        cons_args[i].worker_index   = i; // Define o identificador numérico deste consumidor. | Dicionário: worker_index = identificador numérico de thread.
 
        if (pthread_create(&cons_threads[i], NULL, run_consumer, &cons_args[i]) != 0) { // Cria a thread consumidora i. Se falhar o arranque. | Dicionário: pthread_create = cria thread; run_consumer = função de consumo.
            perror("pthread_create consumidor"); // Reporta o erro ocorrido ao sistema operativo. | Dicionário: perror = erro de sistema.
            exit(1); // Encerra a aplicação retornando o código de erro 1. | Dicionário: exit = aborta.
        } // Fim do bloco if. | Dicionário: } = fecho de bloco.
    } // Fim do ciclo de criação de consumidores. | Dicionário: } = fecho de bloco.
 
    /* ── 6. Esperar produtores, parar dashboard, esperar consumidores ── */ // Sexta fase: sincronização no fecho das threads. | Dicionário: N/A
    /* // Início de comentário sobre ordem deliberada de joins. | Dicionário: N/A
     * Ordem de join deliberada: // Explica a lógica de fecho em camadas. | Dicionário: N/A
     *  a) Esperar todos os produtores → garantir que inseriram tudo no buffer. // Sincronização da fase de escrita. | Dicionário: N/A
     *  b) Sinalizar g_all_done = 1 e esperar o monitor → dashboard final a 100%. // Sincronização do painel gráfico. | Dicionário: N/A
     *  c) Esperar consumidores → garantir que processaram todas as entradas. // Sincronização da fase de leitura e métricas. | Dicionário: N/A
     *     (Os consumidores terminam sozinhos quando vêem buffer.count==0 && // Detalhes da condição limite de fim. | Dicionário: N/A
     *      produtores_ativos==0, sinalizado pelo último produtor.) // Notificação em cascata do fim de dados. | Dicionário: N/A
     */ // Fim do comentário. | Dicionário: N/A
    for (int i = 0; i < num_prod; i++) // Ciclo para esperar por cada uma das threads produtoras lançadas. | Dicionário: num_prod = total de produtores.
        pthread_join(prod_threads[i], NULL); // Suspende o main() até que a thread produtora i termine e saia. | Dicionário: pthread_join = espera pela thread correspondente; NULL = ignora o valor de retorno da thread.
 
    g_all_done = 1;                          /* Avisar o monitor para parar */ // Ativa a flag que indica à thread monitora para sair do seu ciclo e parar. | Dicionário: g_all_done = flag de encerramento do monitor.
    pthread_join(monitor_thread, NULL); // Suspende o main() até que a thread do monitor redesenhe o estado a 100% e saia. | Dicionário: pthread_join = espera.
 
    for (int i = 0; i < NUM_CONSUMERS; i++) // Ciclo para esperar por todas as threads consumidoras lançadas. | Dicionário: NUM_CONSUMERS = contagem.
        pthread_join(cons_threads[i], NULL); // Suspende o main() até que a thread consumidora i processe todos os logs restantes do buffer e saia. | Dicionário: pthread_join = espera.
 
    /* ── 7. Relatório final ── */ // Sétima fase: limpeza de primitivos e apresentação de resultados. | Dicionário: N/A
    destroy_bounded_buffer();  /* Libertar mutex e semáforos do buffer */ // Destrói o mutex e semáforos usados no buffer circular para evitar fugas de recursos do OS. | Dicionário: destroy_bounded_buffer = destrói recursos do buffer.
 
    long elapsed = (long)(time(NULL) - g_start_time); // Calcula a duração total de execução em segundos. | Dicionário: elapsed = intervalo temporal em segundos.
    imprimir_relatorio(&global_metrics, modo); // Mostra o relatório estatístico final detalhado na saída padrão. | Dicionário: imprimir_relatorio = escreve o relatório.
    posix_writef(STDOUT_FILENO, "Tempo de processamento: %ldmin %02lds\n", // Imprime o tempo decorrido no formato minutos e segundos. | Dicionário: STDOUT_FILENO = descritor de stdout; %ld = formato para inteiro longo; %02ld = formato com preenchimento a 2 casas.
                 elapsed / 60, elapsed % 60); // Argumentos da divisão e resto para obter minutos e segundos respetivamente. | Dicionário: N/A
 
    /* Libertar toda a memória alocada dinamicamente */ // Limpeza final de memória alocada na heap. | Dicionário: N/A
    for (int i = 0; i < total_ficheiros; i++) free(ficheiros[i]); // Desaloca individualmente cada string contendo caminhos de ficheiros. | Dicionário: free = liberta memória.
    free(ficheiros); // Desaloca o vetor dinâmico de strings. | Dicionário: free = desalocação.
    free(prod_threads); // Desaloca o array de identificadores de threads produtoras. | Dicionário: free = desalocação.
    free(prod_args); // Desaloca o array de argumentos de produtores. | Dicionário: free = desalocação.
    free(cons_threads); // Desaloca o array de identificadores de threads consumidoras. | Dicionário: free = desalocação.
    free(cons_args); // Desaloca o array de argumentos de consumidores. | Dicionário: free = desalocação.
 
    return 0; // Termina o programa principal indicando sucesso (código zero). | Dicionário: return = retorno; 0 = código de sucesso em C.
} // Fim da função main. | Dicionário: } = fecho de escopo da função.
