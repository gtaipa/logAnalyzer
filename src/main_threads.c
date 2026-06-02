/**
 * @file main_threads.c
 * @brief Entry point for multithreaded log analyzer.
 *
 * Simplified header without detailed flow steps.
 */�rio: exit = fim do processo.
 * @endcode // Fim do bloco de código descritivo do fluxo. | Dicionário: @endcode = tag Doxygen para fechar código.
 */ // Fim do bloco de comentário Doxygen. | Dicionário: */ = fim do comentário.
 
#include <stdio.h> // Biblioteca padrão C para operações de escrita de texto. | Dicionário: #include = inclui biblioteca; stdio.h = cabeçalho de I/O padrão.
#include <stdlib.h> // Biblioteca padrão C para alocação de memória e controlo do processo. | Dicionário: stdlib.h = cabeçalho de funções de sistema.
#include <string.h> // Biblioteca padrão C para comparação e cópia de strings. | Dicionário: string.h = cabeçalho para vetores de caracteres.
#include <dirent.h> // Biblioteca para listagem e travessia de ficheiros em pastas. | Dicionário: dirent.h = cabeçalho de travessia de diretórios Unix.
#include <pthread.h> // Biblioteca POSIX Threads para criar e sincronizar execuções concorrentes. | Dicionário: pthread.h = cabeçalho de multi-threading.
#include <unistd.h> // Biblioteca de sistema POSIX com chamadas I/O e temporizadores. | Dicionário: unistd.h = cabeçalho POSIX de chamadas de sistema básicas.
#include <time.h> // Biblioteca C para ler e manipular horas e instantes temporais. | Dicionário: time.h = cabeçalho de relógio.
#include <fcntl.h> // Biblioteca para manipulação de descritores e modos de abertura. | Dicionário: fcntl.h = cabeçalho de controlo de ficheiros.
#include <sys/stat.h> // Biblioteca POSIX para obter metadados (como tamanhos) de ficheiros. | Dicionário: sys/stat.h = metadados de estado de ficheiros.
 
#include "posix_io.h" // Cabeçalho personalizado com funções robustas de escrita em consola. | Dicionário: posix_io.h = cabeçalho personalizado de entrada/saída.
#include "worker_threads.h" // Cabeçalho com as estruturas de argumentos e funções de workers. | Dicionário: worker_threads.h = cabeçalho declarativo das threads worker.
 
/** Número máximo de threads worker suportadas pelo programa. */ // Documentação Doxygen. | Dicionário: N/A
#define MAX_THREADS 64 // Define o limite máximo de workers permitidos (64). | Dicionário: #define = define macro; MAX_THREADS = macro de limite de threads.
 

 * // Linha de espaçamento. | Dicionário: N/A
 * Escrito pelas threads worker e lido pela thread monitor para calcular // Explica quem escreve e lê. | Dicionário: monitor = thread do painel visual.
 * a percentagem de progresso de cada thread. O acesso é feito sem mutex // Explica a ausência de semáforos/mutexes. | Dicionário: mutex = trinco exclusivo.
 * porque cada thread worker escreve apenas na sua própria posição do array, // Justifica o isolamento das escritas. | Dicionário: array = vetor de elementos.
 * eliminando conflitos de escrita simultânea. // Explica a prevenção de race condition de escrita. | Dicionário: N/A

static long   g_bytes_done[MAX_THREADS]; // Vetor estático local para contar os bytes concluídos por cada thread. | Dicionário: static = visibilidade limitada ao ficheiro; long = inteiro longo; g_bytes_done = array de bytes feitos.

 * // Linha de espaçamento. | Dicionário: N/A
 * Preenchido pelo `main()` antes de criar as threads e lido (só para leitura) // Explica o ciclo de escrita/leitura deste vetor. | Dicionário: main = processo principal.
 * pela thread monitor. Não há escrita concorrente depois da inicialização. // Justifica a inexistência de trincos. | Dicionário: N/A

static long   g_bytes_total[MAX_THREADS]; // Vetor estático local para reter o total de bytes de quota por worker. | Dicionário: g_bytes_total = array de tamanhos totais.
 
/** Número de threads worker efetivamente criadas nesta execução. */ // Documentação Doxygen. | Dicionário: N/A
static int    g_num_workers  = 0; // Armazena a contagem de threads operárias instanciadas. | Dicionário: int = tipo inteiro; g_num_workers = número total de workers ativos.
 
/** Instante (epoch) em que o processamento foi iniciado — usado pelo dashboard. */ // Documentação Doxygen. | Dicionário: N/A
static time_t g_start_time   = 0; // Armazena o timestamp de arranque do processamento de logs. | Dicionário: time_t = tipo temporário padrão de sistemas Unix.

 * // Linha de espaçamento. | Dicionário: N/A
 * Declarado `volatile` para que o compilador não otimize a leitura do valor // Justificação da palavra-chave volatile. | Dicionário: volatile = indica ao compilador que a variável pode mudar de valor externamente.
 * dentro do ciclo da thread monitor: a variável é escrita pelo `main()` e lida // Detalhes da interação entre main e monitor. | Dicionário: N/A
 * pela thread monitor em contextos de execução diferentes. // Explica a concorrência na variável. | Dicionário: N/A

static volatile int g_all_done = 0; // Flag boleana de paragem para a thread do dashboard. | Dicionário: static = visibilidade interna; volatile = impede caches; g_all_done = flag de paragem.
// Dicionário: ANSI = padrão de terminal com cores e sequências.
  // Linha de espaçamento. | Dicionário: N/A
  // Vale 1 se `STDOUT` for um TTY (verificado com `isatty`). Se a saída for // Explica a lógica baseada na verificação de TTY. | Dicionário: STDOUT = fluxo de saída padrão; TTY = terminal interativo; isatty = função de deteção.
  // redireccionada para um ficheiro, o dashboard é desativado. // Explica o motivo de desativação para ficheiros. | Dicionário: N/A

static int    g_dashboard_enabled = 0; // Flag que indica se devemos desenhar o painel (1 se TTY interativo, 0 caso contrário). | Dicionário: g_dashboard_enabled = flag boleana de dashboard.
 
/* ─────────────────────────────────────────────────────────────────────────── */ // Linha estética separadora de secção de código. | Dicionário: N/A
/*  Dashboard                                                                   */ // Comentário estético de início do módulo de painel. | Dicionário: N/A
/* ─────────────────────────────────────────────────────────────────────────── */ // Linha estética. | Dicionário: N/A
 
/** 
 * A função usa sequências de escape ANSI para subir o cursor `linhas` linhas // Explica o controlo do terminal. | Dicionário: sequências de escape ANSI = caracteres especiais de controlo.
 * (`\033[<N>A`) e apagar tudo a partir daí (`\033[J`), redesenhando assim o // Explica a reescrita in-place do texto. | Dicionário: \033[ = início do comando ANSI.
 * painel no mesmo lugar sem criar scroll. Para cada thread worker é apresentada // Explica a prevenção de scroll indesejado. | Dicionário: scroll = rolagem da janela.
 * uma barra de progresso com 20 caracteres e a percentagem calculada como // Detalha o cálculo individual. | Dicionário: N/A
 * `bytes_done / bytes_total * 100`. // Equação de percentagem de progresso. | Dicionário: N/A
 * // Linha de espaçamento. | Dicionário: N/A
 * Esta função é chamada exclusivamente pela thread monitor, não havendo // Justifica a ausência de proteção concorrente. | Dicionário: monitor = thread que atualiza painel.
 * necessidade de proteção por mutex pois: // Lista as justificações de segurança concorrente. | Dicionário: mutex = trinco exclusivo.
 *  - lê `g_bytes_done[]` e `g_bytes_total[]` — cada posição é escrita por uma // Primeira justificação: escritas isoladas no array. | Dicionário: N/A
 *    única thread worker (sem concorrência de escrita). // Detalha que não há race condition de escritas. | Dicionário: N/A
 *  - escreve apenas em `STDOUT` com `posix_writef`, chamada sequencial dentro // Segunda justificação: I/O isolado numa única thread. | Dicionário: posix_writef = escrita segura baseada em write().
 *    da thread monitor. // Local de execução da escrita. | Dicionário: N/A  **/
static void draw_dashboard(void) { // Declara função estática de desenho de ecrã para os workers multithread. | Dicionário: static = visibilidade estática; void = sem parâmetros de entrada/retorno.
    /* Número de linhas que o dashboard ocupa: uma por worker + 7 linhas de moldura */ // Comentário informativo interno sobre altura. | Dicionário: N/A
    int linhas = g_num_workers + 7; // Calcula o tamanho total de linhas ocupadas no terminal. | Dicionário: g_num_workers = quantidade de threads.
 
    /* Subir o cursor para o topo do dashboard e limpar a área */ // Preparação de escrita ANSI. | Dicionário: N/A
    posix_writef(STDOUT_FILENO, "\033[%dA", linhas); // Envia sequência ANSI para mover cursor para cima o número correto de linhas. | Dicionário: STDOUT_FILENO = descritor de stdout; \033[%dA = código de subir cursor.
    posix_writef(STDOUT_FILENO, "\033[J"); // Envia comando ANSI para apagar todo o texto por baixo do cursor. | Dicionário: \033[J = comando ANSI de limpar ecrã.
 
    /* Calcular tempo decorrido desde o início do processamento */ // Medição do relógio. | Dicionário: N/A
    time_t elapsed = time(NULL) - g_start_time; // Diferença em segundos entre a hora atual e a hora de arranque. | Dicionário: time = lê relógio Unix; elapsed = intervalo decorrido.
    int hh = elapsed / 3600; // Obtém as horas inteiras. | Dicionário: hh = horas.
    int mm = (elapsed % 3600) / 60; // Obtém os minutos. | Dicionário: mm = minutos; % = operador de resto.
    int ss = elapsed % 60; // Obtém os segundos. | Dicionário: ss = segundos.
 
    /* Acumular bytes globais para a barra total */ // Agregação de progresso. | Dicionário: N/A
    long total_done  = 0; // Inicializa acumulador para bytes concluídos totais. | Dicionário: total_done = contador acumulador local.
    long total_total = 0; // Inicializa acumulador para bytes da quota agregada. | Dicionário: total_total = contador total local.
    for (int i = 0; i < g_num_workers; i++) { // Itera sobre todas as threads operárias para somar os valores. | Dicionário: for = loop; g_num_workers = quantidade de threads.
        total_done  += g_bytes_done[i]; // Soma progresso do worker i. | Dicionário: g_bytes_done = array de progresso individual.
        total_total += g_bytes_total[i]; // Soma quota total do worker i. | Dicionário: g_bytes_total = array de quotas.
    } // Fim do ciclo de acumulação. | Dicionário: } = fecho de bloco.
    /* Calcular percentagem global; proteger contra divisão por zero */ // Cálculo estatístico final. | Dicionário: N/A
    int total_pct = (total_total > 0) ? (int)(total_done * 100 / total_total) : 0; // Determina a percentagem global de avanço. | Dicionário: ? : = operador condicional ternário; total_pct = percentagem total calculada.
    if (total_pct > 100) total_pct = 100; // Clampa a percentagem global se ultrapassar o teto matemático de bytes. | Dicionário: N/A
 
    /* Cabeçalho do dashboard */ // Desenho estético de moldura. | Dicionário: N/A
    posix_writef(STDOUT_FILENO, "╔══════════════════════════════════════════╗\n"); // Desenha bordo superior. | Dicionário: STDOUT_FILENO = descritor de stdout.
    posix_writef(STDOUT_FILENO, "║    LOG ANALYZER - THREADS MONITOR        ║\n"); // Desenha título da caixa do dashboard. | Dicionário: N/A
    posix_writef(STDOUT_FILENO, "╠══════════════════════════════════════════╣\n"); // Desenha linha intermédia superior. | Dicionário: N/A
 
    /* Uma linha por worker com a barra de progresso individual */ // Processamento visual iterativo dos workers. | Dicionário: N/A
    for (int i = 0; i < g_num_workers; i++) { // Percorre cada um dos workers lançados. | Dicionário: for = loop.
        /* Percentagem desta thread; clampar a [0, 100] */ // Cálculo individual de avanço. | Dicionário: N/A
        int pct = (g_bytes_total[i] > 0) ? (int)(g_bytes_done[i] * 100 / g_bytes_total[i]) : 0; // Realiza divisão de bytes feitos pelo total. | Dicionário: pct = percentagem individual calculada.
        if (pct > 100) pct = 100; // Clampa o limite individual se este exceder os 100%. | Dicionário: N/A
 
        /* Construir a barra: '#' para preenchido, '.' para vazio (20 colunas) */ // Formatação textual da barra. | Dicionário: N/A
        char bar[21]; // Reserva array de caracteres na stack local. | Dicionário: bar = string de progresso; 21 = tamanho.
        int filled = pct / 5;  /* cada '#' representa 5% */ // Define o número de blocos '#' preenchidos (escala de 5% cada). | Dicionário: filled = contagem de marcas cheias.
        for (int b = 0; b < 20; b++) bar[b] = (b < filled) ? '#' : '.'; // Insere '#' nas posições ativas e '.' nas restantes. | Dicionário: for = loop; b = índice; bar = string.
        bar[20] = '\0'; // Insere terminador de string C. | Dicionário: '\0' = terminador de string.
 
        posix_writef(STDOUT_FILENO, "║ Thread %-2d [%s] %3d%%           ║\n", i + 1, bar, pct); // Desenha a linha correspondente ao progresso do worker i. | Dicionário: %-2d = inteiro com 2 posições; %3d%% = percentagem formatada com 3 dígitos.
    } // Fim do ciclo de desenho dos workers. | Dicionário: } = fecho de bloco.
 
    posix_writef(STDOUT_FILENO, "╠══════════════════════════════════════════╣\n"); // Desenha a linha divisória inferior da caixa. | Dicionário: STDOUT_FILENO = descritor de stdout.
 
    /* Barra de progresso global (soma de todas as threads) */ // Desenho da barra acumulativa global. | Dicionário: N/A
    char tot_bar[21]; // Buffer local para a string da barra geral. | Dicionário: tot_bar = array temporário.
    int tot_filled = total_pct / 5; // Calcula a quantidade de '#' da barra acumulada. | Dicionário: tot_filled = marcas cheias do progresso total.
    for (int b = 0; b < 20; b++) tot_bar[b] = (b < tot_filled) ? '#' : '.'; // Popula a barra com '#' e '.'. | Dicionário: for = loop.
    tot_bar[20] = '\0'; // Termina a string C. | Dicionário: '\0' = terminador de string.
 
    posix_writef(STDOUT_FILENO, "║ Total     [%s] %3d%%           ║\n", tot_bar, total_pct); // Imprime a linha de progresso agregada de todas as threads. | Dicionário: total_pct = percentagem geral.
    posix_writef(STDOUT_FILENO, "║ Elapsed: %02d:%02d:%02d                      ║\n", hh, mm, ss); // Imprime o tempo decorrido total no formato relógio. | Dicionário: hh = horas; mm = minutos; ss = segundos.
    posix_writef(STDOUT_FILENO, "╚══════════════════════════════════════════╝\n"); // Imprime o bordo de fecho inferior do painel do dashboard. | Dicionário: STDOUT_FILENO = descritor de stdout.
} // Fim da função draw_dashboard. | Dicionário: } = fecho de escopo da função.
 
/* ─────────────────────────────────────────────────────────────────────────── */ // Linha estética de divisão de secções. | Dicionário: N/A
/*  Thread monitor                                                              */ // Comentário estético de início do módulo de monitor. | Dicionário: N/A
/* ─────────────────────────────────────────────────────────────────────────── */ // 

 * @brief Função de entrada da thread monitor do dashboard. // Resumo da função de ponto de entrada. | Dicionário: N/A
 * // Linha de espaçamento. | Dicionário: N/A
 * @param arg Não utilizado (NULL). Declarado como `(void)arg` para suprimir // Documenta o parâmetro. | Dicionário: @param = parâmetro.
 *            aviso do compilador. // Continuação da nota sobre aviso do compilador. | Dicionário: N/A
 * @return NULL — a thread termina com `pthread_exit(NULL)`, o que é equivalente // Documenta o retorno de thread. | Dicionário: @return = retorno; pthread_exit = fim controlado de thread POSIX.
 *         a retornar NULL mas torna explícita a terminação de uma thread POSIX. // Nota sobre a assinatura de retorno. | Dicionário: NULL = ponteiro nulo.
 * // Linha de espaçamento. | Dicionário: N/A
 * @details // Detalhes sobre concorrência e o padrão clássico do monitor. | Dicionário: @details = detalhes.
 * Esta thread corre em paralelo com as N threads worker. O seu único objetivo // Descreve a concorrência com workers. | Dicionário: paralelo = concorrência.
 * é redesenhar o dashboard a cada 100 ms (`usleep(100000)`) enquanto // Especifica o intervalo de sono e ciclo de atualização. | Dicionário: usleep = suspensão em microsegundos.
 * `g_all_done` for 0. Quando o `main()` define `g_all_done = 1` (após o // Lógica de encerramento do ciclo. | Dicionário: main = processo principal; g_all_done = flag de paragem.
 * `pthread_join` de todos os workers), a thread monitor sai do ciclo, faz um // Sincronização pós-término. | Dicionário: pthread_join = chamada POSIX de sincronização.
 * último redesenho para mostrar 100% e termina. // Explica o desenho final. | Dicionário: N/A
 * // Linha de espaçamento. | Dicionário: N/A
 * Padrão de thread monitor: // Diagrama textual simples do ciclo do monitor. | Dicionário: N/A
 * @code // Início do bloco de código. | Dicionário: @code = início de bloco de código Doxygen.
 *   while (!flag_de_termino) { // Ciclo de atualização gráfica periódica. | Dicionário: while = ciclo.
 *       atualizar_ui(); // Desenha no terminal. | Dicionário: UI = interface de utilizador.
 *       dormir_um_intervalo(); // Suspende temporariamente. | Dicionário: N/A
 *   } // Fim do ciclo. | Dicionário: N/A
 *   atualizar_ui();  // atualização final // Redesenho final de 100%. | Dicionário: UI = interface.
 *   pthread_exit(NULL); // Termina a thread. | Dicionário: pthread_exit = fecho de thread.
 * @endcode // Fim do código de documentação. | Dicionário: @endcode = fim de bloco.
 */ // Fim do comentário Doxygen. | Dicionário: */ = fim do comentário.
void *run_monitor_thread(void *arg) { // Ponto de entrada da thread monitora que devolve um ponteiro genérico void*. | Dicionário: void* = tipo de ponteiro genérico; run_monitor_thread = nome do ponto de entrada; arg = parâmetro.
    (void)arg;  /* parâmetro não utilizado nesta thread */ // Silencia avisos de falta de uso da variável arg no corpo do compilador. | Dicionário: (void) = conversão silenciosa de compilador; arg = parâmetro.
 
    /* Ciclo principal: redesenhar enquanto as workers ainda estiverem a correr */ // Comentário informativo do loop do monitor. | Dicionário: N/A
    while (!g_all_done) { // Executa o ciclo enquanto a flag global g_all_done for zero (falsa). | Dicionário: while = ciclo; ! = operador lógico NÃO; g_all_done = flag de término global.
        draw_dashboard(); // Invoca o desenho atualizado do dashboard na saída padrão. | Dicionário: draw_dashboard = função de desenho.
        usleep(100000);  /* esperar 100 ms antes de redesenhar (10 fps) */ // Adormece a thread por 100 milisegundos (100.000 microsegundos). | Dicionário: usleep = chamada de sistema para suspensão de thread; fps = frames (quadros) por segundo.
    } // Fim do loop while. | Dicionário: } = fecho de bloco.
 
    /* Redesenho final para garantir que o dashboard mostra 100% */ // Correção visual pós-processamento. | Dicionário: N/A
    draw_dashboard(); // Desenha uma última vez o dashboard antes de sair para garantir integridade visual. | Dicionário: draw_dashboard = desenha.
 
    /* // Início de bloco de comentário informativo sobre pthread_exit. | Dicionário: N/A
     * pthread_exit: termina a thread corrente de forma controlada. // Explica a utilidade da terminação explícita de threads. | Dicionário: N/A
     * Permite que pthread_join no main() desbloqueie após esta chamada. // Explica a sincronização com o main. | Dicionário: pthread_join = espera.
     * Diferença para return: pthread_exit chama os destruidores de chaves // Explica detalhes de libertação de chaves. | Dicionário: return = retorno simples; pthread_exit = chamada POSIX.
     * de thread-local storage antes de terminar. // Refere à libertação de armazenamento local da thread. | Dicionário: thread-local storage = dados privados de armazenamento da thread.
     */ // Fim do comentário. | Dicionário: N/A
    pthread_exit(NULL); // Termina e sai da thread monitora de forma limpa. | Dicionário: pthread_exit = fecho de thread; NULL = valor de retorno.
} // Fim da função run_monitor_thread. | Dicionário: } = fecho de escopo da função.
 
/* ─────────────────────────────────────────────────────────────────────────── */ // Linha estética de secção. | Dicionário: N/A
/*  Relatório final                                                             */ // Comentário estético de relatório. | Dicionário: N/A
/* ─────────────────────────────────────────────────────────────────────────── */ // Linha estética. | Dicionário: N/A
 
/** // Início de comentário Doxygen para gerar_relatorio_threads. | Dicionário: / * * = início do bloco.
 * @brief Gera o relatório final de análise e escreve-o no stdout ou num ficheiro. // Resumo da geração de relatório. | Dicionário: stdout = saída padrão.
 * // Linha de espaçamento. | Dicionário: N/A
 * @param total       Ponteiro para as métricas globais já fundidas por todas // Documenta o parâmetro total. | Dicionário: @param = parâmetro; Metrics = tipo de dados.
 *                    as threads worker. // Nota sobre a fusão prévia. | Dicionário: N/A
 * @param modo        String com o modo de análise: "security", "traffic", // Documenta o parâmetro modo. | Dicionário: N/A
 *                    "performance" ou "full". // Opções do modo de filtro. | Dicionário: N/A
 * @param output_file Caminho para o ficheiro de saída. Se NULL, o relatório é // Documenta o ficheiro de saída opcional. | Dicionário: NULL = ponteiro nulo.
 *                    escrito em `STDOUT`. // Comportamento fallback para o stdout. | Dicionário: STDOUT = saída padrão.
 * // Linha de espaçamento. | Dicionário: N/A
 * @details // Detalhes técnicos sobre buffer de escrita e chamadas ao sistema. | Dicionário: @details = detalhes.
 * A função constrói o relatório em memória (buffer de 4 KB) e escreve-o de // Explica a montagem do buffer na stack. | Dicionário: buffer = array local de escrita rápida; KB = kilobyte (1024 bytes).
 * uma vez com `write()`. Se `output_file` não for NULL e puder ser aberto, // Explica o encaminhamento condicional da escrita. | Dicionário: write = chamada de sistema POSIX para escrita de baixo nível.
 * o relatório é redireccionado para esse ficheiro; caso contrário, cai no // Explica a lógica de fallback. | Dicionário: N/A
 * stdout. As secções de alertas e tráfego só são incluídas se o `modo` // Detalhes sobre os filtros do modo. | Dicionário: N/A
 * corresponder. // Condição de filtro. | Dicionário: N/A
 */ // Fim do comentário Doxygen. | Dicionário: */ = fim do comentário.
void gerar_relatorio_threads(Metrics *total, char *modo, char *output_file) { // Declara a função pública de escrita de relatórios. | Dicionário: void = sem retorno; Metrics = estrutura de métricas; char = tipo de caractere.
    int fd_out = STDOUT_FILENO;  /* destino por omissão: stdout */ // Define a saída padrão por defeito como destino de escrita do relatório. | Dicionário: int = tipo inteiro; fd_out = descritor de escrita alvo; STDOUT_FILENO = descritor padrão para saída (1).
    int fd_file = -1; // Variável de descritor para ficheiro, inicializada a -1 (indicador de fechado/inválido). | Dicionário: fd_file = descritor de ficheiro.
 
    /* // Início de bloco de comentários sobre abertura opcional de ficheiro. | Dicionário: N/A
     * Se foi especificado um ficheiro de saída, tentar criá-lo/truncá-lo. // Explica a criação e escrita em ficheiro. | Dicionário: N/A
     * O_WRONLY: abrir apenas para escrita. // Explica o significado do modo de abertura. | Dicionário: O_WRONLY = constante de abertura para apenas escrita.
     * O_CREAT:  criar se não existir. // Significado de criação automática. | Dicionário: O_CREAT = constante que dita a criação do ficheiro se não existir.
     * O_TRUNC:  truncar (apagar conteúdo) se já existir. // Significado de limpeza de ficheiro existente. | Dicionário: O_TRUNC = constante que limpa o conteúdo anterior de ficheiro existente.
     * 0644:     permissões — dono lê+escreve, grupo e outros só lêem. // Significado dos bits de permissão octal. | Dicionário: 0644 = permissões de leitura/escrita Unix em formato octal.
     */ // Fim do comentário. | Dicionário: N/A
    if (output_file != NULL) { // Se um caminho de ficheiro foi especificado nos argumentos do utilizador. | Dicionário: if = condicional; NULL = ponteiro nulo.
        fd_file = open(output_file, O_WRONLY | O_CREAT | O_TRUNC, 0644); // Abre ou cria o ficheiro especificado com permissões de leitura e escrita. | Dicionário: open = chamada de sistema Unix para abrir descritores; O_WRONLY = apenas escrita; O_CREAT = cria; O_TRUNC = limpa conteúdo; | = operador binário OU lógico de bits.
        if (fd_file >= 0) { // Se o ficheiro foi aberto com sucesso (descritor válido maior ou igual a zero). | Dicionário: fd_file = descritor.
            fd_out = fd_file;  /* redirecionar saída para o ficheiro */ // Substitui o destino de escrita pelo descritor do ficheiro aberto. | Dicionário: fd_out = descritor alvo.
            posix_writef(STDOUT_FILENO, "\n[INFO] A gravar relatorio no ficheiro: %s\n", output_file); // Log informativo no terminal do utilizador. | Dicionário: STDOUT_FILENO = descritor de stdout; posix_writef = escrita segura.
        } // Fim do bloco if. | Dicionário: } = fecho de bloco.
        /* Se open falhar (disco cheio, permissões, …), fd_out mantém-se STDOUT */ // Nota informativa de tolerância a falhas. | Dicionário: N/A
    } // Fim do bloco if. | Dicionário: } = fecho de bloco.
 
    /* // Início do bloco de comentários sobre a montagem do buffer do relatório. | Dicionário: N/A
     * Construir o relatório em memória (buffer estático de 4 KiB) antes de // Justifica a construção inicial em memória para evitar syscalls fragmentadas. | Dicionário: syscalls = chamadas de sistema efetuadas ao kernel.
     * escrever. Esta abordagem evita múltiplas chamadas a write() e reduz a // Explica a melhoria de eficiência e coesão da saída. | Dicionário: write = chamada de sistema de escrita.
     * fragmentação da saída. snprintf com `buffer + len` vai preenchendo o // Explica a aritmética simples de escrita incremental. | Dicionário: snprintf = escrita formatada segura limitando número de bytes.
     * buffer de forma segura, nunca escrevendo além dos limites. // Explica a prevenção de overflow de buffer. | Dicionário: overflow = transbordo de memória.
     */ // Fim do comentário. | Dicionário: N/A
    char buffer[4096]; // Buffer de memória temporário de 4096 bytes na stack. | Dicionário: char = tipo de caractere; buffer = array de caracteres; 4096 = dimensão em bytes.
    int len = 0; // Controla o tamanho atual da string dentro do buffer à medida que escrevemos. | Dicionário: len = comprimento de preenchimento.
 
    /* Cabeçalho do relatório com o modo de análise usado */ // Fase 1: escrita de cabeçalho. | Dicionário: N/A
    len += snprintf(buffer + len, sizeof(buffer) - len, "\n=== RELATORIO FINAL THREADS (%s) ===\n", modo); // Escreve o título do relatório formatado para o buffer. | Dicionário: snprintf = escrita segura; buffer + len = endereço de escrita atualizado; sizeof = tamanho total em bytes; modo = filtro de análise.
    len += snprintf(buffer + len, sizeof(buffer) - len, "Total de linhas : %ld\n", total->total_lines); // Escreve a quantidade agregada de linhas processadas. | Dicionário: total_lines = campo totalizador de linhas; %ld = formato para inteiro longo.
 
    /* // Início de comentário interno explicativo da secção de segurança. | Dicionário: N/A
     * Secção de alertas de segurança. // Explica o conteúdo da secção. | Dicionário: N/A
     * Presente nos modos "security" e "full" — filtra contagens de severidade // Descreve as condições de ativação e filtros aplicados. | Dicionário: N/A
     * WARN, ERROR e CRITICAL encontradas nos logs. // Severidades mostradas. | Dicionário: N/A
     * `total` já contém a fusão de todas as threads (protegida pelo mutex // Relembra a fusão síncrona prévia dos dados. | Dicionário: N/A
     * durante a fase de fusão em worker_threads.c). // Ficheiro onde a fusão ocorreu. | Dicionário: N/A
     */ // Fim do comentário. | Dicionário: N/A
    if (strcmp(modo, "security") == 0 || strcmp(modo, "full") == 0) { // Se o filtro de modo incluir a secção de segurança. | Dicionário: strcmp = função para comparar strings; if = condicional.
        len += snprintf(buffer + len, sizeof(buffer) - len, "\n--- ALERTAS DE SEGURANCA ---\n"); // Escreve o subtítulo de segurança no buffer. | Dicionário: N/A
        len += snprintf(buffer + len, sizeof(buffer) - len, "WARNINGS        : %ld\n", total->count_warn); // Escreve a contagem agregada de logs WARNING. | Dicionário: count_warn = campo de aviso.
        len += snprintf(buffer + len, sizeof(buffer) - len, "ERRORS          : %ld\n", total->count_error); // Escreve a contagem agregada de logs ERROR. | Dicionário: count_error = campo de erro.
        len += snprintf(buffer + len, sizeof(buffer) - len, "CRITICAL        : %ld\n", total->count_critical); // Escreve a contagem agregada de logs CRITICAL. | Dicionário: count_critical = campo crítico.
    } // Fim do bloco if. | Dicionário: } = fecho de bloco.
 
    /* // Início de comentário interno da secção de tráfego. | Dicionário: N/A
     * Secção de estatísticas de tráfego HTTP. // Explica a finalidade do bloco. | Dicionário: N/A
     * Presente nos modos "traffic" e "full" — apresenta mensagens INFO e // Filtros aplicados no tráfego. | Dicionário: N/A
     * erros de cliente/servidor (HTTP 4xx + 5xx somados). // Soma de códigos de erros HTTP. | Dicionário: N/A
     */ // Fim do comentário. | Dicionário: N/A
    if (strcmp(modo, "traffic") == 0 || strcmp(modo, "full") == 0) { // Se o filtro selecionado pelo utilizador incluir o tráfego. | Dicionário: strcmp = comparação de strings.
        len += snprintf(buffer + len, sizeof(buffer) - len, "\n--- ESTATISTICAS DE TRAFEGO ---\n"); // Escreve o subtítulo de tráfego no buffer. | Dicionário: N/A
        len += snprintf(buffer + len, sizeof(buffer) - len, "INFO            : %ld\n", total->count_info); // Escreve a contagem agregada de logs INFO. | Dicionário: count_info = campo de contagem de logs informativos.
        len += snprintf(buffer + len, sizeof(buffer) - len, "HTTP 4xx/5xx    : %ld\n", total->count_4xx + total->count_5xx); // Escreve a soma acumulada de erros HTTP 4xx e 5xx. | Dicionário: count_4xx = erros do cliente; count_5xx = erros do servidor.
    } // Fim do bloco if. | Dicionário: } = fecho de bloco.
 
    len += snprintf(buffer + len, sizeof(buffer) - len, "=================================\n\n"); // Escreve a linha estética final no buffer do relatório. | Dicionário: snprintf = escrita formatada.
 
    /* // Início de comentário informativo de escrita em bloco. | Dicionário: N/A
     * Escrever o relatório completo de uma só vez com write() POSIX. // Explica a atomicidade e eficiência da chamada. | Dicionário: N/A
     * Uma única chamada a write() é preferível a várias printf() porque: // Justificação do método em relação ao printf. | Dicionário: printf = escrita padrão formatada em C.
     *  - é atómica para tamanhos inferiores a PIPE_BUF (4 KiB no Linux); // Primeira vantagem da chamada em sistemas Unix. | Dicionário: atómica = realizada como operação isolada sem interrupções intermédias; PIPE_BUF = limite de buffer para escritas atómicas.
     *  - evita intercalação com output de outras fontes (e.g., stderr). // Segunda vantagem: mantém integridade se houver erros de outras threads. | Dicionário: stderr = descritor padrão para fluxo de erro.
     */ // Fim do comentário. | Dicionário: N/A
    if (write(fd_out, buffer, len) < 0) perror("Erro ao escrever relatorio"); // Envia a totalidade do buffer construído para o destino (consola ou ficheiro). | Dicionário: write = chamada de sistema POSIX de escrita; perror = avisa erros.
 
    /* Fechar o descritor do ficheiro de saída se foi aberto nesta função */ // Libertação opcional de recursos abertos na rotina. | Dicionário: N/A
    if (fd_file >= 0) close(fd_file); // Fecha o descritor do ficheiro caso este tenha sido efetivamente aberto na função. | Dicionário: close = chamada de fecho de descritor.
} // Fim da função gerar_relatorio_threads. | Dicionário: } = fecho de escopo da função.
 
/* ─────────────────────────────────────────────────────────────────────────── */ // Linha divisória. | Dicionário: N/A
/*  main                                                                        */ // Comentário estético de arranque. | Dicionário: N/A
/* ─────────────────────────────────────────────────────────────────────────── */ // Linha divisória. | Dicionário: N/A
 
/** // Início de comentário Doxygen para o main. | Dicionário: / * * = início do bloco.
 * @brief Ponto de entrada do programa — orquestra todo o pipeline multithread. // Resumo da rotina principal. | Dicionário: N/A
 * // Linha de espaçamento. | Dicionário: N/A
 * @param argc Número de argumentos da linha de comandos. // Parâmetro argc. | Dicionário: @param = parâmetro; argc = quantidade de strings nos argumentos.
 * @param argv Vetor de argumentos: // Parâmetro argv. | Dicionário: argv = array contendo os argumentos de linha de comandos.
 *             - argv[1]: diretório com os ficheiros de log // Detalhe do argumento 1. | Dicionário: N/A
 *             - argv[2]: número de threads worker a criar // Detalhe do argumento 2. | Dicionário: N/A
 *             - argv[3]: modo de análise (security|performance|traffic|full) // Detalhe do argumento 3. | Dicionário: N/A
 *             - argv[4..]: flags opcionais --verbose e --output=<ficheiro> // Detalhe dos argumentos opcionais. | Dicionário: N/A
 * @return 0 em caso de sucesso; 1 em caso de erro. // Valor de retorno devolvido ao sistema operativo. | Dicionário: @return = valor de retorno.
 * // Linha de espaçamento. | Dicionário: N/A
 * @details // Detalhes dos passos lógicos aplicados no main(). | Dicionário: @details = detalhes.
 * Fluxo de execução: // Descrição ordenada. | Dicionário: N/A
 *  1. Validar argumentos e configurar modo do parser. // Passo 1. | Dicionário: N/A
 *  2. Varrer o diretório e recolher caminhos de ficheiros `.log`/`.json`. // Passo 2. | Dicionário: N/A
 *  3. Calcular total de bytes e determinar a fatia de cada thread. // Passo 3. | Dicionário: N/A
 *  4. Inicializar métricas globais e o mutex que as protege. // Passo 4. | Dicionário: N/A
 *  5. Criar a thread monitor (se em TTY) e as N threads worker. // Passo 5. | Dicionário: N/A
 *  6. Aguardar o fim das workers com `pthread_join`. // Passo 6. | Dicionário: pthread_join = chamada POSIX de barreira.
 *  7. Sinalizar a thread monitor, aguardá-la e destruir o mutex. // Passo 7. | Dicionário: mutex = trinco exclusivo.
 *  8. Imprimir o relatório e libertar memória. // Passo 8. | Dicionário: N/A
 */ // Fim do comentário Doxygen. | Dicionário: */ = fim do comentário.
int main(int argc, char *argv[]) { // Ponto de partida inicial do executável compilado. | Dicionário: int = tipo inteiro; main = ponto de entrada; argc = contagem de strings; argv = array de strings.
    /* Verificar se o stdout é um terminal (TTY) para ativar o dashboard ANSI */ // Configuração inicial baseada no ambiente de terminal. | Dicionário: N/A
    g_dashboard_enabled = isatty(STDOUT_FILENO); // Testa se a saída padrão é um terminal interativo (não redirecionado). | Dicionário: g_dashboard_enabled = flag de controlo de dashboard; isatty = função POSIX que detecta TTY; STDOUT_FILENO = descritor da consola (1).
 
    /* Validação mínima de argumentos obrigatórios */ // Verifica se foram passados os argumentos obrigatórios (mínimo de 4). | Dicionário: N/A
    if (argc < 4) { // Se a contagem de argumentos for inferior a 4. | Dicionário: if = condicional; < = menor que.
        posix_writef(STDOUT_FILENO, "Uso: %s <diretorio> <num_threads> <modo> [--verbose] [--output=ficheiro.txt]\n", argv[0]); // Mostra indicação do formato correto dos argumentos. | Dicionário: STDOUT_FILENO = consola; posix_writef = escrita segura; argv = array.
        exit(1); // Aborta a aplicação devolvendo o código de erro 1. | Dicionário: exit = termina processo.
    } // Fim do bloco if. | Dicionário: } = fecho de bloco.
 
    char *diretorio = argv[1]; // Carrega o endereço da string do caminho do diretório com os logs. | Dicionário: char = tipo caractere; diretorio = variável com string de pasta.
    int num_threads = atoi(argv[2]); // Converte a string contendo a contagem de threads para tipo inteiro. | Dicionário: int = tipo inteiro; num_threads = número de threads; atoi = converte string ASCII para inteiro.
    char *modo      = argv[3]; // Guarda o endereço do modo de processamento (filtros de métricas). | Dicionário: modo = string com modo de filtro.
    int verbose     = 0; // Flag boleana iniciada a zero (falsa) para o modo informativo detalhado. | Dicionário: verbose = flag de log verboso.
    char *output_file = NULL; // Ponteiro para string de caminho do ficheiro de saída, iniciado a nulo. | Dicionário: output_file = ponteiro de string de saída; NULL = ponteiro nulo.
 
    /* Processar flags opcionais: --verbose e --output=<caminho> */ // Leitura e processamento de parâmetros opcionais subsequentes. | Dicionário: N/A
    for (int i = 4; i < argc; i++) { // Itera sobre os argumentos fornecidos para além dos obrigatórios. | Dicionário: for = loop; i = índice; argc = contagem de argumentos.
        if (strcmp(argv[i], "--verbose") == 0) verbose = 1; // Se for encontrada a string "--verbose", ativa o modo verbose. | Dicionário: strcmp = compara strings; verbose = flag.
        else if (strncmp(argv[i], "--output=", 9) == 0) output_file = argv[i] + 9; // Se a string começar por "--output=", extrai o caminho do ficheiro (após o nono caractere). | Dicionário: strncmp = comparação de prefixo de string limitando o tamanho; output_file = ponteiro do ficheiro.
    } // Fim do loop de flags opcionais. | Dicionário: } = fecho de bloco.
 
    /* Configurar o parser com o modo escolhido; abortar se inválido */ // Validação do filtro de logs. | Dicionário: N/A
    if (parser_set_mode_from_string(modo) != 0) { // Configura o modo de parser e verifica se a string é inválida (retorno diferente de zero). | Dicionário: parser_set_mode_from_string = função de parser.h.
        posix_writef(STDERR_FILENO, "Modo invalido: %s (use security|performance|traffic|full)\n", modo); // Escreve o erro no descritor de erros padrão. | Dicionário: STDERR_FILENO = descritor de erros (2); posix_writef = escrita segura.
        exit(1); // Encerra a execução do analisador com código de erro 1. | Dicionário: exit = aborta.
    } // Fim do bloco if. | Dicionário: } = fecho de bloco.
 
    /* ── 1. Descobrir ficheiros ── */ // Primeira fase do main: travessia do diretório. | Dicionário: N/A
    int capacidade = 10, total_ficheiros = 0; // Inicia a capacidade lógica do vetor dinâmico de strings e o contador de ficheiros a zero. | Dicionário: capacidade = limite lógico; total_ficheiros = contagem de ficheiros.
    char **ficheiros = malloc(capacidade * sizeof(char *)); // Reserva espaço inicial na heap para 10 ponteiros de string. | Dicionário: char** = array de ponteiros para string; malloc = alocação dinâmica.
    DIR *dir = opendir(diretorio); // Abre o diretório dos logs para travessia das entradas. | Dicionário: DIR = tipo representativo de diretório; opendir = abre pasta.
    if (!dir) { perror("opendir"); exit(1); } // Se falhar a abertura do diretório, reporta erro e aborta. | Dicionário: ! = negação lógica; perror = avisa erro.
 
    /* Iterar as entradas do diretório e recolher apenas .log e .json */ // Comentário informativo do varrimento da pasta. | Dicionário: N/A
    struct dirent *entrada; // Ponteiro para guardar cada entrada lida da pasta. | Dicionário: struct dirent = tipo contendo nome e metadados de entrada da pasta.
    while ((entrada = readdir(dir)) != NULL) { // Varre as pastas até readdir retornar NULL (fim de diretório). | Dicionário: while = ciclo; readdir = lê entrada de diretório.
        int len = strlen(entrada->d_name); // Calcula o comprimento em caracteres do nome do ficheiro. | Dicionário: len = comprimento; strlen = tamanho de string; d_name = nome da entrada.
        if ((len > 4 && strcmp(entrada->d_name + len - 4, ".log")  == 0) || // Se a terminação do nome for ".log". | Dicionário: strcmp = comparação de strings.
            (len > 5 && strcmp(entrada->d_name + len - 5, ".json") == 0)) { // Ou se for ".json". | Dicionário: || = operador lógico OU.
            /* Crescer o array dinamicamente se necessário (duplicar capacidade) */ // Redimensionamento elástico de strings na heap. | Dicionário: N/A
            if (total_ficheiros == capacidade) { // Se atingir o limite lógico da capacidade do vetor dinâmico. | Dicionário: capacidade = limite atual.
                capacidade *= 2; // Duplica a capacidade limite. | Dicionário: N/A
                ficheiros = realloc(ficheiros, capacidade * sizeof(char *)); // Expande ou move a memória física do vetor na heap. | Dicionário: realloc = realocação dinâmica de memória.
            } // Fim do if de crescimento. | Dicionário: } = fecho de bloco.
            char caminho[512]; // Buffer temporário local para a string concatenada do caminho completo. | Dicionário: caminho = string; 512 = tamanho.
            snprintf(caminho, sizeof(caminho), "%s/%s", diretorio, entrada->d_name); // Une a pasta com o nome do ficheiro de forma segura. | Dicionário: snprintf = escrita limitada em buffer.
            ficheiros[total_ficheiros++] = strdup(caminho);  /* cópia própria do caminho */ // Cria a cópia física e regista a referência no array de strings. | Dicionário: strdup = cria duplicado na heap; total_ficheiros++ = pós-incremento.
        } // Fim do bloco if de validação de extensões. | Dicionário: } = fecho de bloco.
    } // Fim da travessia da pasta. | Dicionário: } = fecho de loop.
    closedir(dir); // Encerra os recursos de leitura de diretório no sistema operativo. | Dicionário: closedir = fecha pasta.
 
    if (total_ficheiros == 0) { // Se a pasta não contiver nenhum ficheiro com extensão suportada. | Dicionário: total_ficheiros = contagem.
        posix_writef(STDOUT_FILENO, "Nenhum ficheiro .log ou .json encontrado.\n"); // Informa na consola. | Dicionário: posix_writef = escrita segura.
        exit(0); // Termina a execução limpa do processo indicando sucesso ao OS. | Dicionário: exit = termina processo.
    } // Fim do bloco if. | Dicionário: } = fecho de bloco.
 
    /* Garantir que não excedemos o limite de threads */ // Limitação estática de segurança concorrente. | Dicionário: N/A
    if (num_threads > MAX_THREADS) num_threads = MAX_THREADS; // Clampa a contagem se exceder o tamanho do array global. | Dicionário: MAX_THREADS = limite estático (64).
 
    /* ── 2. Calcular total de bytes e dividir em fatias iguais ── */ // Segunda fase: cálculo de quotas. | Dicionário: N/A
    struct stat st; // Variável auxiliar para obter metadados de tamanho dos logs. | Dicionário: struct stat = tipo contendo tamanho em bytes.
    off_t total_bytes = 0; // Acumulador para o tamanho total de bytes agregados de todos os ficheiros. | Dicionário: total_bytes = tamanho total.
    for (int i = 0; i < total_ficheiros; i++) { // Itera sobre os caminhos de ficheiros registados no array. | Dicionário: for = loop.
        if (stat(ficheiros[i], &st) == 0) // Lê os metadados do ficheiro. Se obtiver sucesso (retorno 0). | Dicionário: stat = lê metadados.
            total_bytes += st.st_size; // Adiciona o tamanho físico de bytes deste ficheiro ao acumulador global. | Dicionário: st_size = tamanho do ficheiro na estrutura stat.
    } // Fim do loop. | Dicionário: } = fecho de bloco.
 
    /* Não faz sentido ter mais threads do que ficheiros */ // Comentário informativo de otimização concorrente. | Dicionário: N/A
    if (num_threads > total_ficheiros) num_threads = total_ficheiros; // Ajusta o número de threads para não exceder o total de ficheiros. | Dicionário: total_ficheiros = quantidade de ficheiros.
 
    /* // Início de comentário explicativo do fluxo virtual de fatias lineares. | Dicionário: N/A
     * Divisão do espaço de endereçamento de bytes em fatias consecutivas: // Explica a lógica de coordenadas. | Dicionário: N/A
     *   Thread 0  → [0,            bytes_por_thread) // Limites da fatia do worker 0. | Dicionário: N/A
     *   Thread 1  → [bytes_por_thread, 2*bytes_por_thread) // Limites da fatia do worker 1. | Dicionário: N/A
     *   ... // Restantes workers. | Dicionário: N/A
     *   Thread N-1→ [N-1)*bytes_por_thread, total_bytes)   ← última apanha o resto // Último worker cobrindo arredondamentos. | Dicionário: N/A
     * // Linha de espaçamento. | Dicionário: N/A
     * Este padrão garante que todos os bytes são processados sem sobreposição. // Conclusão sobre garantia de exatidão de dados. | Dicionário: N/A
     */ // Fim do comentário. | Dicionário: N/A
    off_t bytes_por_thread = total_bytes / num_threads; // Calcula o tamanho de bytes a processar por cada thread. | Dicionário: bytes_por_thread = quota por thread; off_t = tipo numérico de ficheiro.
 
    /* ── 3. Inicializar estruturas ── */ // Terceira fase: configuração de métricas e trincos. | Dicionário: N/A
    Metrics global_metrics; // Declara a estrutura global que armazenará as estatísticas somadas dos logs. | Dicionário: Metrics = tipo da estrutura de métricas.
    init_metrics(&global_metrics); // Limpa e zera os contadores das métricas conjuntas. | Dicionário: init_metrics = inicializa estatísticas.
 
    /* // Início de comentário interno informativo sobre a utilidade do mutex. | Dicionário: N/A
     * Mutex para proteger global_metrics durante a fase de fusão. // Explica a utilidade de isolar a escrita concorrente no final. | Dicionário: N/A
     * Sem mutex, duas threads a escrever simultaneamente em global_metrics // Descreve a ameaça concorrente. | Dicionário: N/A
     * causariam uma race condition: incrementos perdidos e dados corrompidos. // Descreve a race condition nos contadores. | Dicionário: race condition = condição de corrida (escritas sobrepostas conflituosas).
     * pthread_mutex_init com NULL usa atributos por omissão (mutex normal). // Detalha as configurações do mutex POSIX. | Dicionário: N/A
     */ // Fim do comentário. | Dicionário: N/A
    pthread_mutex_t metrics_mutex; // Declara a variável local do mutex de controlo exclusivo. | Dicionário: pthread_mutex_t = tipo de mutex; metrics_mutex = nome do mutex de métricas.
    pthread_mutex_init(&metrics_mutex, NULL); // Inicializa o mutex com as configurações de exclusão mútua padrão. | Dicionário: pthread_mutex_init = inicializa mutex; NULL = atributos por omissão.
 
    pthread_t  *threads = malloc(num_threads * sizeof(pthread_t)); // Aloca array na heap para os identificadores das threads worker. | Dicionário: threads = vetor dinâmico de identificadores.
    ThreadArgs *args    = malloc(num_threads * sizeof(ThreadArgs)); // Aloca array na heap para as estruturas de argumentos de cada worker. | Dicionário: ThreadArgs = estrutura de argumentos do worker; args = vetor dinâmico.
    pthread_t   monitor_thread; // Declara identificador de thread para alojar a execução do monitor do painel visual. | Dicionário: monitor_thread = identificador da thread do monitor.
 
    /* Inicializar variáveis globais de progresso antes de criar as threads */ // Configuração inicial de monitorização. | Dicionário: N/A
    g_num_workers = num_threads; // Atualiza contagem de workers do monitor com o número real de threads concorrentes. | Dicionário: g_num_workers = global de workers ativos.
    g_start_time  = time(NULL); // Regista o timestamp físico inicial do arranque de I/O concorrente. | Dicionário: time = relógio.
    memset(g_bytes_done,  0, sizeof(g_bytes_done)); // Preenche o progresso individual de bytes feitos com zeros. | Dicionário: memset = inicializa bloco de memória.
    memset(g_bytes_total, 0, sizeof(g_bytes_total)); // Preenche a quota total individual de bytes com zeros. | Dicionário: N/A
    g_all_done = 0; // Garante que a flag de paragem do dashboard arranca desativada. | Dicionário: g_all_done = flag de paragem do monitor.
 
    /* // Início de comentário informativo sobre preparação de consola ANSI. | Dicionário: N/A
     * Criar a thread monitor antes das workers para que o dashboard apareça // Explica a ordem temporal de arranque. | Dicionário: N/A
     * imediatamente. Imprime N+7 linhas em branco primeiro para reservar // Explica o truque de alinhamento estético de ecrã. | Dicionário: N/A
     * espaço no terminal que será reutilizado pelos redesenhos ANSI. // Explica como o desenho in-place é garantido no terminal. | Dicionário: ANSI = sequências especiais de consola.
     * // Linha de espaçamento. | Dicionário: N/A
     * pthread_create: cria uma nova thread de execução. // Descreve a assinatura da chamada de arranque. | Dicionário: N/A
     *   - arg1: identificador da thread (preenchido pela função) // Significado do parâmetro 1. | Dicionário: N/A
     *   - arg2: atributos (NULL = padrão) // Significado do parâmetro 2. | Dicionário: N/A
     *   - arg3: função de entrada da thread // Significado do parâmetro 3. | Dicionário: N/A
     *   - arg4: argumento passado à função de entrada // Significado do parâmetro 4. | Dicionário: N/A
     */ // Fim do comentário. | Dicionário: N/A
    if (g_dashboard_enabled) { // Se a saída padrão for um terminal TTY e suportar comandos ANSI. | Dicionário: g_dashboard_enabled = flag de TTY.
        for (int i = 0; i < g_num_workers + 7; i++) posix_writef(STDOUT_FILENO, "\n"); // Empurra o terminal imprimindo linhas vazias para reservar a área do dashboard. | Dicionário: STDOUT_FILENO = consola; posix_writef = escrita segura.
        pthread_create(&monitor_thread, NULL, run_monitor_thread, NULL); // Inicia fisicamente a thread de atualização de ecrã do monitor. | Dicionário: pthread_create = cria thread; monitor_thread = identificador; run_monitor_thread = função alvo; NULL = atributos por defeito/sem argumento.
    } // Fim do bloco if. | Dicionário: } = fecho de bloco.
 
    /* ── 4. Lançar threads com fatias de bytes ── */ // Quarta fase: arranque de workers multithread. | Dicionário: N/A
    for (int i = 0; i < num_threads; i++) { // Loop de criação e parametrização de cada thread worker. | Dicionário: num_threads = quantidade de workers.
        /* Preencher os argumentos específicos desta thread worker */ // Parametrização local da thread. | Dicionário: N/A
        args[i].ficheiros       = ficheiros;         /* lista partilhada de caminhos (só leitura) */ // Associa a referência da lista partilhada de caminhos de logs. | Dicionário: ficheiros = array de strings.
        args[i].total_ficheiros = total_ficheiros; // Passa o total de caminhos de logs na lista. | Dicionário: total_ficheiros = total de logs.
        args[i].byte_inicio     = (off_t)i * bytes_por_thread; // Configura o byte de início da quota correspondente à thread i. | Dicionário: byte_inicio = offset de arranque.
        /* A última thread vai até ao fim real para absorver o resto da divisão inteira */ // Explicação de tratamento do resto de bytes. | Dicionário: N/A
        args[i].byte_fim        = (i == num_threads - 1) ? total_bytes : (off_t)(i + 1) * bytes_por_thread; // Configura o byte de término estendendo-o até total_bytes se for a última thread. | Dicionário: total_bytes = tamanho total agregado.
        args[i].worker_index    = i; // Atribui o identificador numérico de ordenação à thread (0-based). | Dicionário: worker_index = identificador numérico.
        args[i].verbose         = verbose; // Associa a flag que dita se logs de depuração detalhados devem ser escritos. | Dicionário: verbose = modo detalhado.
        args[i].global_metrics  = &global_metrics;   /* partilhado — acesso via mutex */ // Passa o endereço das métricas conjuntas onde ocorrerá a fusão. | Dicionário: global_metrics = endereço de estatísticas.
        args[i].mutex           = &metrics_mutex;    /* mesmo mutex para todas as threads */ // Passa a referência do mutex que controla o acesso exclusivo no final. | Dicionário: metrics_mutex = endereço de mutex.
        args[i].bytes_done      = &g_bytes_done[i];  /* cada thread escreve na sua posição */ // Associa o slot de contagem de progresso de bytes no array global. | Dicionário: bytes_done = apontador de avanço.
        args[i].bytes_total     = &g_bytes_total[i]; // Associa o slot da quota total atribuída a esta thread. | Dicionário: bytes_total = apontador de total.
 
        /* // Início de comentário explicativo sobre pthread_create concorrente. | Dicionário: N/A
         * pthread_create: lança a thread i com a função run_worker_thread. // Explica a função alvo e a criação concorrente. | Dicionário: run_worker_thread = função operária.
         * Cada thread recebe um ponteiro para o seu próprio ThreadArgs. // Explica a passagem da estrutura de parâmetros isolada. | Dicionário: ThreadArgs = tipo de dados.
         * As threads correm em paralelo a partir deste ponto. // Concorrência física síncrona/assíncrona no OS. | Dicionário: paralelo = simultâneo.
         */ // Fim do comentário. | Dicionário: N/A
        if (pthread_create(&threads[i], NULL, run_worker_thread, &args[i]) != 0) { // Lança e inicia o worker i. Se falhar o arranque. | Dicionário: pthread_create = cria thread; run_worker_thread = função operária.
            perror("Erro ao criar thread"); // Reporta o erro ocorrido ao sistema operativo. | Dicionário: perror = erro de sistema.
            exit(1); // Aborta a aplicação devolvendo o código de erro 1. | Dicionário: exit = aborta.
        } // Fim do bloco if. | Dicionário: } = fecho de bloco.
    } // Fim do ciclo de criação das threads. | Dicionário: } = fecho de bloco.
 
    /* ── 5. Esperar pelas threads ── */ // Quinta fase: barreira de sincronização de fecho. | Dicionário: N/A
    /* // Início de comentário descritivo da barreira de join. | Dicionário: N/A
     * pthread_join: bloqueia o main() até que a thread indicada termine. // Explica o bloqueio do main até à conclusão do worker. | Dicionário: N/A
     * Garante que todas as métricas locais já foram fundidas nas globais // Justificação de segurança de consistência de dados. | Dicionário: N/A
     * antes de acedermos a global_metrics para o relatório. // Explica a necessidade lógica do join antes do print. | Dicionário: N/A
     * Sem pthread_join, o main() poderia ler global_metrics incompleta. // Descreve a race condition se o main prosseguir cedo demais. | Dicionário: N/A
     */ // Fim do comentário. | Dicionário: N/A
    for (int i = 0; i < num_threads; i++) // Ciclo de junção para todos os workers lançados. | Dicionário: num_threads = contagem de workers.
        pthread_join(threads[i], NULL); // Suspende a execução do main() até que a thread worker i termine por completo. | Dicionário: pthread_join = chamada POSIX de barreira; NULL = ignora retorno.
 
    /* Sinalizar a thread monitor que pode terminar e aguardar a sua saída */ // Protocolo de encerramento do monitor de painel. | Dicionário: N/A
    if (g_dashboard_enabled) { // Se a flag de dashboard interativo estiver ativa. | Dicionário: g_dashboard_enabled = flag de TTY.
        g_all_done = 1;                        /* escrever flag de fim */ // Ativa a flag que sinaliza à thread monitora que a sua atividade terminou. | Dicionário: g_all_done = flag de paragem.
        pthread_join(monitor_thread, NULL);    /* esperar redesenho final */ // Suspende o main() até que o monitor desenhe o estado final a 100% e saia. | Dicionário: pthread_join = espera.
    } // Fim do bloco if. | Dicionário: } = fecho de bloco.
 
    /* Libertar o mutex — já não há threads a aceder às métricas globais */ // Limpeza de primitivos concorrentes. | Dicionário: N/A
    pthread_mutex_destroy(&metrics_mutex); // Destrói o mutex das métricas desativando-o do sistema operativo. | Dicionário: pthread_mutex_destroy = destrói mutex; metrics_mutex = endereço de trinco.
 
    /* ── 6. Relatório e limpeza ── */ // Sexta fase: estatísticas e desalocação na heap. | Dicionário: N/A
    long elapsed = (long)(time(NULL) - g_start_time); // Calcula o tempo total de processamento em segundos. | Dicionário: elapsed = intervalo decorrido.
    gerar_relatorio_threads(&global_metrics, modo, output_file); // Gera e grava o relatório final de análise estatística. | Dicionário: gerar_relatorio_threads = cria relatório.
    posix_writef(STDOUT_FILENO, "Tempo de processamento: %ldmin %02lds\n", // Imprime o tempo total no formato minutos e segundos. | Dicionário: STDOUT_FILENO = consola; %ld = formato para inteiro longo; %02ld = formato com preenchimento a 2 casas.
                 elapsed / 60, elapsed % 60); // Argumentos da divisão e resto para obter minutos e segundos respetivamente. | Dicionário: N/A
 
    /* Libertar memória dinâmica alocada para caminhos e estruturas */ // Limpeza na heap. | Dicionário: N/A
    for (int i = 0; i < total_ficheiros; i++) free(ficheiros[i]); // Desaloca individualmente cada string contendo caminhos de ficheiros. | Dicionário: free = desalocação na heap.
    free(ficheiros); // Desaloca o vetor dinâmico de strings. | Dicionário: free = desalocação.
    free(threads); // Desaloca o array de identificadores de threads. | Dicionário: free = desalocação.
    free(args); // Desaloca o array de argumentos de workers. | Dicionário: free = desalocação.
 
    return 0; // Termina a execução do main() indicando sucesso (código zero) ao sistema operativo. | Dicionário: return = retorno; 0 = código de sucesso em C.
} // Fim da função main. | Dicionário: } = fecho de escopo da função.
