/** // Início do bloco de documentação Doxygen do arquivo. | Dicionário: / * * = delimitador de início de comentários de bloco Doxygen.
 * @file worker_threads.c // Nome deste ficheiro de código fonte. | Dicionário: @file = tag Doxygen para especificar o nome do ficheiro.
 * @brief Implementação da thread worker do analisador de logs multithread. // Resumo breve do arquivo. | Dicionário: @brief = tag Doxygen para descrição curta.
 * // Linha de espaçamento da documentação. | Dicionário: N/A
 * @details // Detalhes da thread worker e concorrência baseada em data parallelism. | Dicionário: @details = tag Doxygen para descrição detalhada.
 * Cada thread worker recebe uma fatia de bytes dos ficheiros de log — definida // Explica a fatia atribuída a cada worker. | Dicionário: fatia = quota de bytes consecutivos.
 * pelos campos `byte_inicio` e `byte_fim` de `ThreadArgs` — e processa apenas // Refere os campos da estrutura. | Dicionário: ThreadArgs = estrutura com argumentos do worker; byte_inicio = offset inicial; byte_fim = offset final.
 * as linhas que caem nessa fatia. Este padrão de divisão de trabalho é chamado // Explica o papel de filtragem. | Dicionário: N/A
 * de **data parallelism**: o mesmo código corre em paralelo sobre partes // Explica o conceito de paralelismo de dados. | Dicionário: data parallelism = paralelismo de dados (executar o mesmo processamento em partes diferentes do dataset).
 * diferentes dos dados, sem necessidade de coordenação durante o processamento. // Explica a ausência de contenção na leitura. | Dicionário: N/A
 * // Linha de espaçamento da documentação. | Dicionário: N/A
 * Fluxo de execução de cada thread worker: // Introdução à listagem de passos. | Dicionário: N/A
 *  1. Inicializar métricas locais (privadas à thread, sem partilha). // Passo 1: inicialização isolada. | Dicionário: métricas locais = contadores privados alocados na stack da thread.
 *  2. Iterar os ficheiros de log e calcular qual a porção de cada ficheiro que // Passo 2: cálculo de porções lógicas de ficheiro. | Dicionário: N/A
 *     pertence à fatia desta thread. // Associação com a quota global. | Dicionário: N/A
 *  3. Posicionar o cursor de leitura (`lseek`) no início da fatia e avançar // Passo 3: deslocação com lseek e alinhamento. | Dicionário: lseek = chamada de sistema para mover cursor no ficheiro.
 *     até ao próximo `\n` para não começar a meio de uma linha alheia. // Detalha o alinhamento no caractere nova linha. | Dicionário: '\n' = caractere nova linha.
 *  4. Ler e analisar linha a linha, acumulando contagens nas métricas locais. // Passo 4: processamento dos logs. | Dicionário: N/A
 *  5. No final, fundir as métricas locais nas globais protegendo o acesso com // Passo 5: secção crítica protegida. | Dicionário: fusão = operação de soma dos resultados; global = partilhado por todos.
 *     `pthread_mutex_lock` / `pthread_mutex_unlock` para evitar race conditions. // Detalha as chamadas e o perigo de race conditions. | Dicionário: pthread_mutex_lock = bloqueia mutex; pthread_mutex_unlock = liberta mutex; race conditions = condições de corrida (escritas concorrentes destrutivas).
 * // Linha de espaçamento da documentação. | Dicionário: N/A
 * Porque se usam métricas **locais** durante o processamento: // Introdução à fundamentação de design. | Dicionário: N/A
 *  - Evita contenção no mutex: cada thread escreve nos seus próprios contadores // Vantagem 1: evita bloqueios sistemáticos no loop. | Dicionário: contenção = disputa concorrente por um trinco.
 *    durante toda a fase de leitura; o mutex só é adquirido uma vez, no final. // Especifica a obtenção única do mutex. | Dicionário: N/A
 *  - Melhor desempenho de cache: os contadores locais estão na stack da thread // Vantagem 2: localidade de referência e cache do CPU. | Dicionário: stack = pilha de memória privada da thread; cache L1 = memória ultrarrápida do CPU.
 *    (ou na sua cache L1), sem falhas de cache provocadas por outras threads. // Detalha a eliminação de invalidade de cache. | Dicionário: falhas de cache = ocorrências em que os dados necessários não estão na cache rápido do processador.
 * // Linha de espaçamento da documentação. | Dicionário: N/A
 * Race condition que o mutex previne: // Introdução à explicação sobre integridade concorrente. | Dicionário: N/A
 *  - Sem mutex, duas threads poderiam executar simultaneamente, por exemplo, // Caso de estudo sem proteção. | Dicionário: N/A
 *    `global->count_error += local->count_error`. Esta operação não é atómica // Explica a não-atomicidade de adições em C. | Dicionário: atómica = operação completa e indivisível.
 *    em C: envolve uma leitura, uma adição e uma escrita. Se ambas lerem o mesmo // Detalha os passos internos de CPU. | Dicionário: N/A
 *    valor antes de qualquer escrita, uma das adições é perdida (lost update). // Detalha a perda de informação (lost update). | Dicionário: lost update = atualização perdida por sobreposição.
 *  - O mutex garante exclusão mútua: apenas uma thread executa a secção crítica // Descreve a garantia oferecida pelo mutex. | Dicionário: exclusão mútua = apenas uma thread de cada vez; secção crítica = parte do código que acede a dados partilhados.
 *    de fusão de cada vez. // Continuação da descrição. | Dicionário: N/A
 */ // Fim do bloco de comentário Doxygen. | Dicionário: */ = fim do comentário.
 
#include "worker_threads.h" // Inclui as definições das estruturas de argumentos de workers. | Dicionário: #include = inclui biblioteca; worker_threads.h = cabeçalho declarativo das threads worker.
#include "parser.h" // Inclui funções de parser de formato e análise sintática de logs. | Dicionário: parser.h = cabeçalho do analisador de texto de logs.
#include "posix_io.h" // Inclui wrappers síncronos de escrita para consola. | Dicionário: posix_io.h = cabeçalho personalizado de entrada/saída.
#include <stdio.h> // Inclui biblioteca padrão para impressão e controlo de erros. | Dicionário: stdio.h = cabeçalho padrão de entrada/saída.
#include <stdlib.h> // Inclui biblioteca padrão para gestão de processos e memória. | Dicionário: stdlib.h = cabeçalho de funções de sistema.
#include <string.h> // Inclui biblioteca padrão para comparação e manipulação de strings. | Dicionário: string.h = cabeçalho para vetores de caracteres.
#include <unistd.h> // Inclui biblioteca padrão POSIX com chamadas de leitura e fecho de descritores. | Dicionário: unistd.h = cabeçalho POSIX de chamadas de sistema básicas.
#include <fcntl.h> // Inclui definições padrão para controle de descritores de ficheiro (como O_RDONLY). | Dicionário: fcntl.h = cabeçalho de controlo de ficheiros.
#include <pthread.h> // Inclui a API de Threads POSIX para criação e controlo concorrente. | Dicionário: pthread.h = cabeçalho de multi-threading.
#include <sys/stat.h> // Inclui biblioteca para metadados de ficheiros (tamanho, acessos, etc.). | Dicionário: sys/stat.h = metadados de estado de ficheiros.
 
/** Tamanho do buffer de leitura em bytes (4 KiB — alinhado com páginas de memória). */ // Documentação Doxygen. | Dicionário: N/A
#define BUF_SIZE 4096 // Define a macro de tamanho do bloco lido dos logs como 4096. | Dicionário: #define = define macro; BUF_SIZE = macro de tamanho de leitura.
 
/** Comprimento máximo de uma linha de log aceite pelo parser. */ // Documentação Doxygen. | Dicionário: N/A
#define LINE_MAX 512 // Define a macro com o limite de caracteres de uma linha de log como 512. | Dicionário: LINE_MAX = tamanho máximo por linha.
 
/** // Início de comentário Doxygen para a função run_worker_thread. | Dicionário: / * * = início do bloco.
 * @brief Função de entrada de cada thread worker — processa uma fatia de bytes // Resumo da rotina operária. | Dicionário: N/A
 *        dos ficheiros de log e funde as métricas locais nas globais. // Detalhe da fusão final. | Dicionário: N/A
 * // Linha de espaçamento. | Dicionário: N/A
 * @param arg Ponteiro para a estrutura `ThreadArgs` preenchida pelo `main()`. // Documenta o parâmetro arg. | Dicionário: @param = parâmetro; ThreadArgs = estrutura de argumentos.
 *            Contém a fatia de bytes a processar, os descritores dos ficheiros, // Detalha campos de limites e ficheiros. | Dicionário: N/A
 *            o ponteiro para as métricas globais, o mutex de proteção e os // Detalha campos de sincronização e métricas. | Dicionário: N/A
 *            ponteiros de progresso (`bytes_done` / `bytes_total`). // Detalha campos de monitorização. | Dicionário: N/A
 * @return NULL — a thread termina com `pthread_exit(NULL)`. O valor de retorno // Documenta o retorno. | Dicionário: @return = retorno; pthread_exit = fim de thread.
 *         não é utilizado pelo `main()` (segundo argumento de `pthread_join` é // Explica que o join ignora o retorno. | Dicionário: NULL = ponteiro nulo.
 *         NULL), mas a assinatura `void *` é obrigatória pela API POSIX Threads. // Justificação da tipagem de retorno. | Dicionário: void* = tipo de ponteiro genérico.
 * // Linha de espaçamento. | Dicionário: N/A
 * @details // Detalhes do fluxo estruturado em pseudocódigo. | Dicionário: @details = detalhes.
 * Padrão de thread worker (data parallelism por fatia de bytes): // Explica o conceito arquitetural. | Dicionário: N/A
 * @code // Início do bloco de código. | Dicionário: @code = início de bloco de código Doxygen.
 *   inicializar métricas locais // Passo inicial na stack. | Dicionário: N/A
 *   para cada ficheiro: // Ciclo de ficheiros. | Dicionário: N/A
 *       calcular [local_start, local_end) dentro deste ficheiro // Mapeamento de coordenadas. | Dicionário: N/A
 *       lseek para local_start // Deslocação do cursor. | Dicionário: lseek = reposiciona cursor.
 *       se local_start > 0: avançar até ao próximo '\n'   ← alinhamento de linha // Sincronização e alinhamento de linha. | Dicionário: '\n' = nova linha.
 *       ler blocos de BUF_SIZE bytes e analisar linha a linha // Parser em blocos. | Dicionário: N/A
 *       acumular resultados em local_metrics               ← sem contenção // Agregação privada de dados. | Dicionário: local_metrics = métricas privadas.
 *   pthread_mutex_lock(mutex)                              ← secção crítica // Entrada em zona restrita concorrente. | Dicionário: mutex = trinco.
 *       fundir local_metrics em global_metrics // Soma final de contadores. | Dicionário: N/A
 *   pthread_mutex_unlock(mutex)                            ← fim da secção crítica // Saída da zona concorrente. | Dicionário: N/A
 *   pthread_exit(NULL) // Terminação de thread. | Dicionário: pthread_exit = fecho.
 * @endcode // Fim do bloco de código. | Dicionário: @endcode = fim de bloco.
 * // Linha de espaçamento. | Dicionário: N/A
 * A thread não necessita de mutex durante a fase de leitura porque `local_metrics` // Fundamentação sobre exclusão mútua. | Dicionário: N/A
 * é uma variável local na stack — invisível a todas as outras threads. // Explica a natureza isolada da stack de execução. | Dicionário: stack = memória privada.
 */ // Fim do comentário Doxygen. | Dicionário: */ = fim do comentário.
void *run_worker_thread(void *arg) { // Ponto de entrada de thread que aceita e retorna ponteiros genéricos. | Dicionário: void* = tipo de ponteiro genérico; run_worker_thread = nome da função.
    /* Fazer cast do argumento genérico (void*) para o tipo concreto ThreadArgs* */ // Comentário informativo sobre conversão de ponteiros. | Dicionário: N/A
    ThreadArgs *t = (ThreadArgs *)arg; // Converte o ponteiro genérico void* para a estrutura com metadados do worker. | Dicionário: ThreadArgs = tipo de argumentos; t = ponteiro local com argumentos.
 
    /* Extrair os limites da fatia atribuída a esta thread */ // Variáveis de coordenadas locais da fatia. | Dicionário: N/A
    off_t byte_inicio = t->byte_inicio;  /* primeiro byte global a processar (inclusivo) */ // Armazena o offset inicial global da quota de leitura. | Dicionário: off_t = tipo numérico de ficheiro POSIX; byte_inicio = início da fatia.
    off_t byte_fim    = t->byte_fim;     /* último byte global a processar (exclusivo) */ // Armazena o offset final global da quota de leitura. | Dicionário: byte_fim = fim da fatia.
    off_t quota       = byte_fim - byte_inicio;  /* total de bytes desta fatia */ // Calcula a dimensão total de bytes a processar pela thread. | Dicionário: quota = diferença calculada de bytes.
 
    /* Comunicar à thread monitor a quota e o progresso inicial (0 bytes feitos) */ // Inicialização de monitorização de progresso. | Dicionário: N/A
    *(t->bytes_total) = (long)quota; // Guarda no dashboard do monitor a quota de bytes total de processamento. | Dicionário: bytes_total = ponteiro do dashboard de total; long = tipo de dados inteiro longo.
    *(t->bytes_done)  = 0; // Configura o progresso inicial de bytes concluídos como zero no dashboard. | Dicionário: bytes_done = ponteiro de progresso feito.
 
    /* // Início de comentário interno explicativo sobre as métricas locais. | Dicionário: N/A
     * Métricas locais: privadas a esta thread durante todo o processamento. // Explica a privacidade das variáveis locais da stack. | Dicionário: N/A
     * Evitam concorrência no mutex porque não são partilhadas; apenas no final, // Explica o ganho de eficiência no loop. | Dicionário: N/A
     * na fase de fusão, é que os seus valores são somados às métricas globais. // Explica o acoplamento final. | Dicionário: N/A
     */ // Fim do comentário. | Dicionário: N/A
    Metrics local_metrics; // Declara a estrutura local na stack para guardar os contadores desta thread. | Dicionário: Metrics = tipo de dados; local_metrics = variável local na stack.
    init_metrics(&local_metrics); // Inicia e zera todos os contadores da estrutura local de métricas. | Dicionário: init_metrics = inicialização de estatísticas.
 
    /* // Início de comentário explicativo sobre global_offset. | Dicionário: N/A
     * global_offset: acumulador do deslocamento em bytes considerando todos os // Explica o papel de acumulador. | Dicionário: N/A
     * ficheiros já visitados. Serve para mapear offsets locais de cada ficheiro // Explica a utilidade do mapeamento local. | Dicionário: N/A
     * para o espaço de endereçamento global (contínuo) de bytes. // Conclusão sobre as coordenadas lineares. | Dicionário: N/A
     */ // Fim do comentário. | Dicionário: N/A
    off_t global_offset = 0; // Registador do deslocamento lógico global de ficheiros visitados. | Dicionário: global_offset = offset acumulado de bytes.
    char  buf[BUF_SIZE];   /* buffer de leitura reutilizável para cada bloco */ // Buffer local para carregar os pedaços físicos do ficheiro de logs. | Dicionário: char = tipo de caractere; buf = array local de caracteres; BUF_SIZE = tamanho do bloco de leitura.
    char  linha[LINE_MAX]; /* linha atual em construção, caracter a caracter */ // Buffer local para guardar a linha de log à medida que a montamos. | Dicionário: linha = array local de caractere; LINE_MAX = comprimento limite.
 
    /* ── Iterar todos os ficheiros para encontrar os que pertencem à nossa fatia ── */ // Primeira etapa: pesquisa. | Dicionário: N/A
    for (int i = 0; i < t->total_ficheiros; i++) { // Percorre a lista de ficheiros registados. | Dicionário: for = loop de repetição; total_ficheiros = contagem de caminhos.
        struct stat st; // Cria estrutura para obter metadados (como tamanho em bytes) do ficheiro. | Dicionário: struct stat = metadados de estado do ficheiro.
        if (stat(t->ficheiros[i], &st) != 0) continue; /* ficheiro inacessível */ // Lê metadados. Se falhar (retorno diferente de zero), ignora o ficheiro. | Dicionário: stat = lê metadados; ficheiros = lista de caminhos; continue = salta para a próxima iteração.
        off_t fsize = st.st_size; // Obtém o tamanho físico em bytes do ficheiro a partir da estrutura stat. | Dicionário: fsize = tamanho do ficheiro; st_size = campo que guarda o tamanho em bytes.
 
        /* Ficheiro completamente antes da nossa fatia → ignorar e avançar o offset */ // Comentário explicativo sobre saltar ficheiros prévios. | Dicionário: N/A
        if (global_offset + fsize <= byte_inicio) { // Se a nossa fatia começar mais à frente do que este ficheiro termina. | Dicionário: N/A
            global_offset += fsize; // Soma o tamanho do ficheiro saltado ao acumulador de deslocamento global. | Dicionário: += = operador de atribuição aditiva.
            continue; // Avança para o ficheiro seguinte da lista. | Dicionário: continue = reinicia loop.
        } // Fim do bloco if. | Dicionário: } = fecho de bloco.
        /* Ficheiro completamente depois da nossa fatia → parar a iteração */ // Comentário sobre otimização por excesso de avanço. | Dicionário: N/A
        if (global_offset >= byte_fim) break; // Se a nossa fatia terminar antes de atingir este ficheiro, encerra o loop de processamento. | Dicionário: break = interrompe o ciclo.
 
        /* // Início de comentário explicativo sobre cálculo de limites locais. | Dicionário: N/A
         * Calcular os limites dentro deste ficheiro em coordenadas locais: // Mapeia coordenadas globais para coordenadas locais do ficheiro. | Dicionário: N/A
         *   local_start: byte do ficheiro a partir do qual esta thread deve ler. // Início da leitura no ficheiro atual. | Dicionário: N/A
         *   local_end:   byte do ficheiro até ao qual esta thread deve ler. // Fim da leitura no ficheiro atual. | Dicionário: N/A
         * Se a fatia global começa antes deste ficheiro, local_start = 0. // Caso particular de arranque no ficheiro. | Dicionário: N/A
         * Se a fatia global termina depois deste ficheiro, local_end = fsize. // Caso particular de fecho no ficheiro. | Dicionário: N/A
         */ // Fim do comentário. | Dicionário: N/A
        off_t local_start = (byte_inicio > global_offset) ? byte_inicio - global_offset : 0; // Calcula o deslocamento local de início de leitura no ficheiro. | Dicionário: ? : = operador condicional ternário.
        off_t local_end   = (byte_fim < global_offset + fsize) ? byte_fim - global_offset : fsize; // Calcula o deslocamento local de fim de leitura. | Dicionário: local_end = limite superior local.
 
        /* Abrir o ficheiro em modo só-leitura (sem criação, sem escrita) */ // Abertura física do descritor de ficheiro. | Dicionário: N/A
        int fd = open(t->ficheiros[i], O_RDONLY); // Abre o ficheiro atual apenas para leitura. | Dicionário: open = chamada de sistema para abrir ficheiro; O_RDONLY = flag de apenas leitura; fd = descritor de ficheiro.
        if (fd < 0) { global_offset += fsize; continue; } /* falha de abertura → próximo */ // Se falhar a abertura, avança o offset e passa ao ficheiro seguinte. | Dicionário: < 0 = indica erro de chamada Unix.
 
        /* Modo verbose: registar no stderr qual o intervalo processado */ // Escrita de log verbose. | Dicionário: N/A
        if (t->verbose) // Se o modo detalhado tiver sido ativado pelo utilizador. | Dicionário: verbose = flag de log verboso.
            posix_writef(STDERR_FILENO, "[Thread %d] %s bytes [%lld-%lld]\n", // Escreve o intervalo no descritor de erros padrão. | Dicionário: STDERR_FILENO = descritor de erros (2); posix_writef = escrita síncrona.
                         t->worker_index, t->ficheiros[i], // Argumentos de índice da thread e caminho do ficheiro. | Dicionário: worker_index = identificador numérico de thread.
                         (long long)local_start, (long long)local_end); // Parâmetros de limites convertidos para tipo long long. | Dicionário: long long = inteiro de 64 bits.
 
        /* // Início de comentário sobre deslocação de lseek. | Dicionário: N/A
         * lseek: reposicionar o cursor de leitura do descritor `fd` para // Explica a chamada e o descritor a mover. | Dicionário: fd = descritor de ficheiro.
         * `local_start` bytes a partir do início do ficheiro (SEEK_SET). // Explica o ponto de destino local_start. | Dicionário: SEEK_SET = modo absoluto a partir do início do ficheiro.
         * Sem esta chamada, a leitura começaria sempre no byte 0 do ficheiro, // Explica o perigo de repetição de dados se não movermos o cursor. | Dicionário: N/A
         * obrigando a descartar dados que pertencem a outras threads. // Explica o desperdício de tempo e processamento redundante. | Dicionário: N/A
         */ // Fim do comentário. | Dicionário: N/A
        if (lseek(fd, local_start, SEEK_SET) < 0) { // Reposiciona o cursor de leitura para local_start. | Dicionário: lseek = move cursor de leitura do ficheiro.
            perror("lseek"); // Reporta o erro ocorrido no lseek no fluxo de erro. | Dicionário: perror = erro de sistema.
            close(fd); // Fecha o descritor do ficheiro aberto. | Dicionário: close = fecha descritor.
            global_offset += fsize; // Soma o tamanho do ficheiro ao deslocamento global acumulado. | Dicionário: global_offset = offset global.
            continue; // Avança para o ficheiro seguinte da lista. | Dicionário: continue = reinicia o loop.
        } // Fim do bloco if. | Dicionário: } = fecho de bloco.
 
        off_t file_pos = local_start; /* posição atual dentro do ficheiro */ // Variável local que guarda a posição física corrente de leitura no ficheiro. | Dicionário: file_pos = offset lógico local.
 
        /* // Início de comentário explicativo sobre o alinhamento de linha. | Dicionário: N/A
         * Alinhamento de linha: se `local_start > 0`, o cursor pode estar // Explica a hipótese de estar a meio de uma linha de outra thread. | Dicionário: N/A
         * a meio de uma linha que pertence à thread anterior (que processa // Explica que a linha mutilada pertence ao worker anterior. | Dicionário: N/A
         * o byte imediatamente antes de `local_start`). Para evitar parsear // Explica o porquê de saltarmos a linha incompleta. | Dicionário: N/A
         * uma linha incompleta, avançamos caracter a caracter até encontrar // Explica o varrimento por alinhamento de newline. | Dicionário: N/A
         * um '\n' — o início do nosso território é sempre o começo de uma // Garante o ponto de partida na nova linha. | Dicionário: '\n' = caractere nova linha.
         * linha nova. // Conclusão. | Dicionário: N/A
         */ // Fim do comentário. | Dicionário: N/A
        if (local_start > 0) { // Se a nossa quota começar no meio do ficheiro, avança até à primeira linha inteira. | Dicionário: local_start = offset inicial.
            char c; // Variável para armazenar o caractere lido na pesquisa. | Dicionário: char = tipo de caractere.
            ssize_t r; // Variável para reter o retorno do read. | Dicionário: ssize_t = tipo inteiro assinado para retornos de I/O.
            /* Ler byte a byte até ao newline que delimita o fim da linha anterior */ // Loop de pesquisa. | Dicionário: N/A
            while ((r = read(fd, &c, 1)) == 1) { // Lê 1 caractere de cada vez. | Dicionário: read = leitura física; 1 = quantidade de bytes.
                file_pos++; // Incrementa a posição lógica local do ficheiro. | Dicionário: file_pos = posição de leitura.
                if (c == '\n') break; /* encontrámos o início da próxima linha */ // Se encontrar o caractere de nova linha, interrompe o alinhamento. | Dicionário: break = quebra de loop.
            } // Fim do ciclo while. | Dicionário: } = fecho de loop.
            if (r <= 0) { // Se ler zero bytes (fim de ficheiro) ou ocorrer erro de I/O. | Dicionário: r = retorno de leitura.
                /* EOF ou erro antes de encontrar '\n' — não há linhas a processar */ // Nada a ler no ficheiro atual. | Dicionário: EOF = fim de ficheiro.
                close(fd); // Fecha o descritor de ficheiro. | Dicionário: close = fecha descritor.
                global_offset += fsize; // Avança o deslocamento acumulado global com o tamanho deste ficheiro. | Dicionário: global_offset = offset global.
                continue; // Salta para o ficheiro seguinte. | Dicionário: continue = reinicia o loop.
            } // Fim do if. | Dicionário: } = fecho de bloco.
        } // Fim do bloco de alinhamento de linha. | Dicionário: } = fecho de bloco.
 
        int len  = 0;             /* número de caracteres acumulados em `linha` */ // Inicializa a contagem de caracteres acumulados na linha de log. | Dicionário: len = comprimento.
        int done = 0;             /* flag: 1 quando ultrapassámos local_end */ // Flag iniciada a zero que controla se a quota do ficheiro terminou. | Dicionário: done = indicador boleano de fecho.
        LogFormat fmt = FORMAT_UNKNOWN; /* formato do log (detetado na 1.ª linha) */ // Inicia o formato de logs como desconhecido. | Dicionário: LogFormat = tipo enumerado de formato de log; FORMAT_UNKNOWN = constante de formato indefinido.
 
        /* ── Loop principal de leitura em blocos ── */ // Processamento em blocos. | Dicionário: N/A
        while (!done) { // Ciclo de leitura enquanto a fatia de bytes não estiver esgotada. | Dicionário: while = ciclo; done = flag.
            /* // Início de comentário informativo sobre read em blocos. | Dicionário: N/A
             * read: lê até BUF_SIZE bytes do ficheiro para `buf`. // Explica o carregamento em memória do buffer. | Dicionário: BUF_SIZE = macro de tamanho de bloco.
             * Retorna o número real de bytes lidos (pode ser < BUF_SIZE no // Explica o retorno em caso de fim de ficheiro. | Dicionário: N/A
             * final do ficheiro) ou 0 (EOF) ou -1 (erro). // Diferentes estados de retorno. | Dicionário: N/A
             */ // Fim do comentário. | Dicionário: N/A
            ssize_t n = read(fd, buf, BUF_SIZE); // Lê bloco de bytes para buf. | Dicionário: read = leitura física; n = total de bytes lidos.
            if (n <= 0) break; /* EOF ou erro → parar processamento deste ficheiro */ // Abandona o ciclo de blocos se ler zero ou der erro. | Dicionário: break = quebra de ciclo.
 
            /* // Início de comentário informativo sobre atualização de progresso. | Dicionário: N/A
             * Atualizar o progresso desta thread em bytes processados. // Explica a comunicação de progresso para o dashboard. | Dicionário: N/A
             * `pos_na_quota` é o deslocamento dentro da nossa fatia global // Explica o cálculo da percentagem relativa. | Dicionário: N/A
             * (começa em 0, termina em `quota`). A thread monitor lê este // Destinatário da leitura do valor. | Dicionário: N/A
             * valor para calcular a percentagem do dashboard. // Objetivo final da comunicação de progresso. | Dicionário: N/A
             * Não há race condition aqui: apenas esta thread escreve nesta // Explica a segurança concorrente devido ao isolamento do slot. | Dicionário: N/A
             * posição do array g_bytes_done[worker_index]. // Vetor de progresso global. | Dicionário: N/A
             */ // Fim do comentário. | Dicionário: N/A
            off_t pos_na_quota = (global_offset + file_pos) - byte_inicio; // Calcula os bytes feitos até ao momento na fatia. | Dicionário: pos_na_quota = progresso acumulado local.
            if (pos_na_quota < 0)     pos_na_quota = 0; // Ajusta para não ser negativo caso o alinhamento inicial mova a leitura. | Dicionário: N/A
            if (pos_na_quota > quota) pos_na_quota = quota; // Clampa o limite para não ultrapassar a quota da fatia atribuída. | Dicionário: quota = bytes totais da fatia.
            *(t->bytes_done) = (long)pos_na_quota; // Escreve o progresso no endereço da variável partilhada com o monitor. | Dicionário: bytes_done = ponteiro de progresso do monitor.
 
            /* Processar o bloco lido caracter a caracter para montar linhas */ // Analisador sintático iterativo de bloco. | Dicionário: N/A
            for (int b = 0; b < (int)n && !done; b++) { // Itera sobre cada um dos bytes lidos no buffer. | Dicionário: for = loop; b = índice do buffer; n = bytes lidos.
                char c = buf[b]; // Carrega o caractere b do buffer. | Dicionário: buf = buffer de leitura; c = caractere.
                file_pos++; /* avançar a posição local no ficheiro */ // Incrementa a nossa posição física de leitura no ficheiro. | Dicionário: file_pos = offset lógico de leitura.
 
                if (c == '\n') { // Se o caractere atual for mudança de linha. | Dicionário: '\n' = caractere nova linha.
                    /* Fim de linha: analisar a linha acumulada em `linha` */ // Comentário informativo de processamento de linha. | Dicionário: N/A
                    if (len > 0) { // Se a linha acumulada tiver dados válidos. | Dicionário: len = comprimento.
                        linha[len] = '\0'; /* terminar a string C */ // Insere o terminador nulo para formar a string C. | Dicionário: '\0' = terminador de string.
 
                        /* // Início de comentário sobre deteção estática de formato. | Dicionário: N/A
                         * Detetar o formato do log (Apache, Nginx, syslog, JSON…) // Explica a deteção inicial. | Dicionário: N/A
                         * uma única vez por ficheiro: a partir da 1.ª linha // Explica que o formato permanece idêntico em todo o ficheiro. | Dicionário: N/A
                         * válida, o formato mantém-se constante. // Explica a constância do tipo de logs. | Dicionário: N/A
                         */ // Fim do comentário. | Dicionário: N/A
                        if (fmt == FORMAT_UNKNOWN) fmt = detect_format(linha); // Invoca deteção de formato se for a primeira linha do ficheiro. | Dicionário: detect_format = função que deduz o formato de logs; fmt = formato de log.
 
                        /* Parsear a linha e atualizar as métricas locais */ // Análise de campos e incremento de métricas privadas. | Dicionário: N/A
                        LogEntry entry; // Variável que receberá a estrutura do log analisado. | Dicionário: LogEntry = estrutura do log com IP, data, etc.
                        if (parse_line(linha, fmt, &entry) == 0) // Descodifica a linha. Se tiver sucesso (retorno 0). | Dicionário: parse_line = analisa texto; entry = destino.
                            update_metrics(&local_metrics, &entry); // Atualiza os contadores locais na stack com os dados obtidos. | Dicionário: update_metrics = soma métricas; local_metrics = métricas da stack.
                        len = 0; /* reiniciar o buffer de linha */ // Zera o tamanho acumulado para ler a linha seguinte. | Dicionário: len = comprimento.
                    } // Fim do bloco if. | Dicionário: } = fecho de bloco.
                    /* // Início de comentário explicativo do limite de paragem de leitura. | Dicionário: N/A
                     * Verificar se já ultrapassámos o limite da nossa fatia. // Explica a condição de paragem no ficheiro. | Dicionário: N/A
                     * A condição é testada depois do '\n' para garantir que // Justifica a avaliação da condição pós-newline. | Dicionário: N/A
                     * a linha completa é processada antes de parar. // Garante que não mutilamos linhas válidas no corte. | Dicionário: N/A
                     */ // Fim do comentário. | Dicionário: N/A
                    if (file_pos >= local_end) done = 1; // Se a posição física de leitura atingir o final, ativa done para sair do loop. | Dicionário: local_end = limite local.
                } else if (c != '\r') { // Se for caractere comum, ignorando retornos de carro Windows. | Dicionário: '\r' = retorno de carro em ficheiros Windows.
                    /* Acumular o caracter na linha atual (ignorar '\r' de CRLF) */ // Acumulação. | Dicionário: CRLF = quebra de linha com retorno de carro (Windows).
                    if (len < LINE_MAX - 1) linha[len++] = c; // Guarda o caractere e atualiza o tamanho se houver espaço livre no buffer. | Dicionário: LINE_MAX = limite; len++ = pós-incremento.
                } // Fim do bloco de caracteres. | Dicionário: } = fecho de bloco.
            } // Fim do ciclo de caracteres do bloco. | Dicionário: } = fecho de bloco.
        } // Fim do ciclo de blocos do ficheiro. | Dicionário: } = fecho de bloco.
 
        /* // Início de comentário descritivo da última linha órfã. | Dicionário: N/A
         * Última linha sem '\n': pode acontecer no final de um ficheiro que // Explica a possibilidade de falta de newline final. | Dicionário: N/A
         * não termina com newline, ou quando `done` foi ativado no meio de // Explica a interrupção a meio do ficheiro. | Dicionário: N/A
         * uma linha. Processar o que ficou no buffer `linha`. // Garante o processamento dos dados órfãos. | Dicionário: N/A
         */ // Fim do comentário. | Dicionário: N/A
        if (len > 0) { // Se restou texto no acumulador de linha após o fecho de blocos. | Dicionário: len = comprimento.
            linha[len] = '\0'; // Insere o terminador nulo para formar a string C final. | Dicionário: '\0' = terminador de string.
            if (fmt == FORMAT_UNKNOWN) fmt = detect_format(linha); // Garante a deteção de formato caso tenha sido a única linha válida. | Dicionário: detect_format = deduz formato.
            LogEntry entry; // Variável local estruturada para capturar campos do log. | Dicionário: LogEntry = estrutura do log.
            if (parse_line(linha, fmt, &entry) == 0) // Descodifica a linha final. | Dicionário: parse_line = analisa a linha.
                update_metrics(&local_metrics, &entry); // Atualiza os contadores na stack local com a última entrada. | Dicionário: update_metrics = soma métricas.
        } // Fim do bloco if. | Dicionário: } = fecho de bloco.
 
        close(fd);             /* libertar o descritor de ficheiro */ // Fecha o descritor libertando recursos do sistema operativo. | Dicionário: close = fecha descritor.
        global_offset += fsize; /* avançar o offset global para o próximo ficheiro */ // Soma o tamanho completo do ficheiro analisado ao deslocamento acumulado global. | Dicionário: global_offset = offset global.
    } // Fim do ciclo de ficheiros. | Dicionário: } = fecho de bloco.
 
    /* Forçar progresso a 100% para que o dashboard mostre a thread como concluída */ // Ajuste visual de fecho de thread no dashboard. | Dicionário: N/A
    *(t->bytes_done) = (long)quota; // Escreve o total de quota no slot de progresso para preencher a barra gráfica a 100%. | Dicionário: bytes_done = apontador de progresso feito.
 
    /* // Início de bloco de comentários sobre a fusão exclusiva final. | Dicionário: N/A
     * ── Secção crítica: fusão das métricas locais nas globais ── // Zona de exclusão concorrente regulada. | Dicionário: N/A
     * // Linha de espaçamento. | Dicionário: N/A
     * pthread_mutex_lock: adquire o mutex exclusivo. Se outra thread worker // Explica o bloqueio se outra thread estiver a aceder às métricas. | Dicionário: pthread_mutex_lock = trinco de bloqueio.
     * já estiver a fundir as suas métricas, esta thread bloqueia aqui até // Explica a sincronização de concorrência. | Dicionário: N/A
     * que o mutex seja libertado. Isto garante que apenas uma thread de cada // Explica a garantia de consistência temporal. | Dicionário: N/A
     * vez escreve em `global_metrics`, eliminando a race condition. // Explica a prevenção de race condition. | Dicionário: global_metrics = métricas conjuntas.
     * // Linha de espaçamento. | Dicionário: N/A
     * Sem este mutex, dois incrementos simultâneos (e.g., count_error += N) // Detalha o comportamento incorreto concorrente se não usar trinco. | Dicionário: N/A
     * poderiam sobrepor-se: ambas as threads lêem o mesmo valor antigo, // Detalha a leitura do valor original por duas threads. | Dicionário: N/A
     * somam as suas contribuições separadamente e escrevem resultados // Detalha as somas isoladas baseadas em dados desatualizados. | Dicionário: N/A
     * inconsistentes — um dos incrementos seria silenciosamente perdido. // Conclusão de falha de cálculo (lost update). | Dicionário: N/A
     */ // Fim do comentário. | Dicionário: N/A
    pthread_mutex_lock(t->mutex); // Adquire o mutex partilhado para aceder e atualizar as métricas globais em segurança. | Dicionário: pthread_mutex_lock = bloqueia mutex; mutex = ponteiro para mutex.
 
    /* Somar os contadores simples: operação segura dentro da secção crítica */ // Fusão física dos dados locais na estrutura global. | Dicionário: N/A
    t->global_metrics->total_lines    += local_metrics.total_lines; // Soma as linhas acumuladas localmente às linhas globais. | Dicionário: total_lines = campo totalizador de linhas.
    t->global_metrics->count_debug    += local_metrics.count_debug; // Soma a contagem local de logs DEBUG à contagem global. | Dicionário: count_debug = campo.
    t->global_metrics->count_info     += local_metrics.count_info; // Soma a contagem local de logs INFO à contagem global. | Dicionário: count_info = campo.
    t->global_metrics->count_warn     += local_metrics.count_warn; // Soma a contagem local de logs WARNING à contagem global. | Dicionário: count_warn = campo.
    t->global_metrics->count_error    += local_metrics.count_error; // Soma a contagem local de logs ERROR à contagem global. | Dicionário: count_error = campo.
    t->global_metrics->count_critical += local_metrics.count_critical; // Soma a contagem local de logs CRITICAL à contagem global. | Dicionário: count_critical = campo.
    t->global_metrics->count_4xx      += local_metrics.count_4xx; // Soma a contagem local de erros HTTP 4xx à contagem global. | Dicionário: count_4xx = erros do cliente.
    t->global_metrics->count_5xx      += local_metrics.count_5xx; // Soma a contagem local de erros HTTP 5xx à contagem global. | Dicionário: count_5xx = erros do servidor.
 
    /* // Início de comentário explicativo da fusão síncrona da tabela de IPs. | Dicionário: N/A
     * Fusão da tabela de IPs: para cada IP encontrado localmente, verificar // Lógica de merge linear de IPs únicos. | Dicionário: N/A
     * se já existe na tabela global (merge) ou inserir como nova entrada. // Lógica de pesquisa condicional. | Dicionário: N/A
     * Esta lógica de pesquisa linear é executada dentro do mutex para que // Justifica a inclusão da pesquisa no mutex para evitar escritas cruzadas. | Dicionário: N/A
     * não haja dois workers a modificar ip_list[] / ip_count[] em simultâneo. // Previne quebras na ordenação e consistência da lista. | Dicionário: ip_list = array de IPs; ip_count = array de contadores.
     */ // Fim do comentário. | Dicionário: N/A
    for (int i = 0; i < local_metrics.ip_num; i++) { // Itera sobre cada IP único encontrado pela thread localmente. | Dicionário: for = loop; ip_num = contagem de IPs únicos locais.
        int found = -1; // Variável que guardará o índice do IP se este já existir na tabela global (inicia a -1). | Dicionário: found = índice do IP encontrado.
        /* Procurar se este IP já está registado nas métricas globais */ // Pesquisa linear exclusiva. | Dicionário: N/A
        for (int j = 0; j < t->global_metrics->ip_num; j++) { // Itera sobre a lista de IPs já registados na tabela global. | Dicionário: ip_num = IPs globais.
            if (strcmp(t->global_metrics->ip_list[j], local_metrics.ip_list[i]) == 0) { // Se as duas strings do IP coincidirem. | Dicionário: strcmp = compara strings; ip_list = array bidimensional de IPs.
                found = j; /* IP já existe — guardar o índice para atualizar a contagem */ // Regista o índice onde o IP correspondente foi encontrado na lista global. | Dicionário: found = índice.
                break; // Interrompe a pesquisa linear dado que o IP já foi localizado. | Dicionário: break = quebra loop.
            } // Fim do bloco if. | Dicionário: } = fecho de bloco.
        } // Fim da pesquisa. | Dicionário: } = fecho de loop.
        if (found >= 0) { // Se o IP já estava registado na lista global (índice maior ou igual a zero). | Dicionário: found = índice do IP.
            /* IP duplicado: somar a contagem local à entrada global já existente */ // Soma dos contadores do IP específico. | Dicionário: N/A
            t->global_metrics->ip_count[found] += local_metrics.ip_count[i]; // Soma as ocorrências locais do IP na contagem da lista global correspondente. | Dicionário: ip_count = array de contadores de IP.
        } else if (t->global_metrics->ip_num < MAX_IPS) { // Caso seja IP novo e ainda haja vagas na tabela global (abaixo de MAX_IPS). | Dicionário: MAX_IPS = constante com capacidade máxima da lista de IPs.
            /* IP novo: inserir na próxima posição livre da tabela global */ // Inserção de nova string e valor. | Dicionário: N/A
            strncpy(t->global_metrics->ip_list[t->global_metrics->ip_num], // Copia a string do IP local para a primeira vaga livre da lista global. | Dicionário: strncpy = cópia segura limitando caracteres; ip_num = total de IPs globais registados.
                    local_metrics.ip_list[i], IP_LEN - 1); // Passa o IP local e define o limite do tamanho para evitar transbordo. | Dicionário: IP_LEN = tamanho reservado por IP.
            t->global_metrics->ip_list[t->global_metrics->ip_num][IP_LEN - 1] = '\0'; // Adiciona o terminador nulo físico no fim do buffer do IP copiado. | Dicionário: '\0' = terminador de string.
            t->global_metrics->ip_count[t->global_metrics->ip_num] = local_metrics.ip_count[i]; // Define o valor da contagem inicial de pedidos deste IP na tabela global. | Dicionário: ip_count = contadores.
            t->global_metrics->ip_num++; // Incrementa a contagem total de IPs únicos registados na tabela global. | Dicionário: ip_num = total de IPs.
        } // Fim do bloco de merge do IP. | Dicionário: } = fecho de bloco.
        /* Se MAX_IPS foi atingido, o IP extra é descartado silenciosamente */ // Nota sobre o limite superior do array estático. | Dicionário: N/A
    } // Fim do loop de merge de IPs. | Dicionário: } = fecho de bloco.
 
    /* // Início de comentário explicativo do merge de alertas síncrono. | Dicionário: N/A
     * Fusão dos alertas: copiar os alertas locais para a lista global, // Lógica de merge de alertas textuais. | Dicionário: N/A
     * respeitando o limite MAX_ALERTS. Também dentro do mutex para evitar // Explica o respeito ao limite máximo e proteção concorrente. | Dicionário: MAX_ALERTS = limite de alertas da tabela global.
     * escritas concorrentes no array global de alertas. // Previne race conditions de escrita em alertas. | Dicionário: N/A
     */ // Fim do comentário. | Dicionário: N/A
    for (int i = 0; i < local_metrics.num_alerts && t->global_metrics->num_alerts < MAX_ALERTS; i++) { // Percorre alertas locais e continua se houver vagas no vetor global. | Dicionário: for = loop; num_alerts = contagem de alertas.
        strncpy(t->global_metrics->alerts[t->global_metrics->num_alerts], // Copia o texto do alerta local para a vaga livre no vetor de alertas global. | Dicionário: strncpy = cópia segura; alerts = array de strings de alertas.
                local_metrics.alerts[i], ALERT_LEN - 1); // Passa a mensagem de alerta e limita o tamanho físico do texto para ALERT_LEN-1. | Dicionário: ALERT_LEN = tamanho máximo reservado por alerta.
        t->global_metrics->alerts[t->global_metrics->num_alerts][ALERT_LEN - 1] = '\0'; // Garante o caractere nulo terminal físico no buffer de alertas copiado. | Dicionário: '\0' = terminador de string.
        t->global_metrics->num_alerts++; // Incrementa o contador global de alertas registados. | Dicionário: num_alerts = alertas registados.
    } // Fim do loop de merge de alertas. | Dicionário: } = fecho de bloco.
 
    /* // Início do bloco de comentários informativos sobre pthread_mutex_unlock. | Dicionário: N/A
     * pthread_mutex_unlock: libertar o mutex para que a próxima thread worker // Explica a libertação da exclusão mútua concorrente. | Dicionário: pthread_mutex_unlock = chamada POSIX para desbloqueio de mutex.
     * em espera possa entrar na secção crítica e fundir as suas métricas. // Explica a reativação de workers suspensos no trinco. | Dicionário: N/A
     * É fundamental não esquecer esta chamada — um mutex nunca libertado // Explica o perigo de esquecimento do desbloqueio (deadlock). | Dicionário: N/A
     * bloquearia o programa indefinidamente (deadlock). // Detalha a suspensão eterna resultante do bloqueio. | Dicionário: deadlock = impasse de suspensão mútua (bloqueio perpétuo de recursos).
     */ // Fim do comentário. | Dicionário: N/A
    pthread_mutex_unlock(t->mutex); // Liberta o mutex síncrono partilhado de escrita exclusiva das métricas. | Dicionário: pthread_mutex_unlock = desbloqueia trinco; mutex = ponteiro para o mutex.
 
    /* // Início do bloco de comentários explicativos sobre a chamada pthread_exit. | Dicionário: N/A
     * pthread_exit: terminar esta thread de forma controlada. // Explica a terminação limpa e controlada da thread worker. | Dicionário: N/A
     * O valor NULL é o "valor de retorno" da thread; como o main() chama // Explica o destino do valor de retorno passado. | Dicionário: main = processo principal; NULL = ponteiro nulo.
     * pthread_join(threads[i], NULL), esse valor é ignorado. // Explica que o main descarta a leitura do retorno no join. | Dicionário: pthread_join = espera.
     * Diferença em relação a `return NULL`: pthread_exit executa os // Explica a vantagem de limpeza profunda de recursos localizados. | Dicionário: return = retorno simples.
     * destruidores de thread-local storage antes de terminar, garantindo // Refere a libertação de destruidores TLS associados à thread. | Dicionário: thread-local storage = dados privados de armazenamento local da thread.
     * limpeza correta de recursos associados à thread. // Explica a robustez proporcionada pela chamada. | Dicionário: N/A
     * Após esta chamada, o pthread_join correspondente no main() desbloqueia. // Explica a consequência de desbloqueio síncrono no main. | Dicionário: N/A
     */ // Fim do comentário. | Dicionário: N/A
    pthread_exit(NULL); // Executa a terminação física limpa da thread operária POSIX. | Dicionário: pthread_exit = fecho de thread; NULL = ponteiro nulo.
} // Fim da função run_worker_thread. | Dicionário: } = fecho de escopo da função.