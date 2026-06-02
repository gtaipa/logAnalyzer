/** // Início do bloco de documentação do arquivo. | Dicionário: / * * = delimitador de início de comentários Doxygen.
 * @file worker_prodcons.c // Identifica o nome deste arquivo de código fonte. | Dicionário: @file = tag Doxygen para indicar o nome do ficheiro.
 * @brief Implementação do padrão Produtor-Consumidor com bounded buffer. // Descrição breve do objetivo do arquivo. | Dicionário: @brief = tag Doxygen para resumo curto; bounded buffer = buffer com capacidade limitada.
 * // Linha de espaçamento da documentação. | Dicionário: N/A
 * @details // Início da secção de detalhes adicionais. | Dicionário: @details = tag Doxygen para descrição detalhada.
 * Este ficheiro contém toda a mecânica de sincronização entre produtores // Explica que aqui reside a lógica de controlo de concorrência. | Dicionário: sincronização = coordenação de execução de threads.
 * e consumidores: // Continuação da descrição dos papéis do padrão. | Dicionário: N/A
 * // Linha de espaçamento da documentação. | Dicionário: N/A
 *  - **Buffer circular (bounded buffer):** estrutura `LogQueue` com capacidade // Explica a estrutura de dados circular partilhada. | Dicionário: Buffer circular = estrutura que reutiliza slots de memória ciclicamente; LogQueue = nome da estrutura de fila de logs.
 *    fixa `BUFFER_SIZE = 30`. Os índices `in` (próxima posição de escrita) e // Detalha o tamanho fixo do buffer e os apontadores. | Dicionário: BUFFER_SIZE = macro que define o tamanho máximo da fila; in = variável/campo para índice de inserção.
 *    out` (próxima posição de leitura) avançam em módulo `BUFFER_SIZE`, // Descreve a lógica do índice de saída e módulo circular. | Dicionário: out = campo para índice de remoção; módulo = operação matemática de resto da divisão (%).
 *    reutilizando ciclicamente as posições do array. O campo `count` mantém // Explica como o array é reutilizado e o contador de elementos. | Dicionário: array = vetor de elementos ordenados na memória; count = contador de elementos atualmente no buffer.
 *    o número de entradas actualmente no buffer. // Descreve a utilidade do contador. | Dicionário: buffer = área de armazenamento temporário.
 * // Linha de espaçamento da documentação. | Dicionário: N/A
 *  - **Semáforos POSIX:** // Introdução à secção sobre os semáforos. | Dicionário: Semáforos = primitivos de sincronização para controlar acesso a recursos partilhados; POSIX = padrão de sistema operativo.
 *      - `buffer.empty`: inicializado a `BUFFER_SIZE`; decrementado pelo // Descreve o semáforo que conta posições vazias. | Dicionário: buffer.empty = semáforo de posições vazias; inicializado = atribuído um valor de partida.
 *        produtor antes de escrever (bloqueia quando o buffer está cheio). // Explica o bloqueio do produtor quando o buffer enche. | Dicionário: decrementado = diminuído de um em um; bloqueia = suspende a execução da thread até o semáforo ser incrementado.
 *      - `buffer.full`: inicializado a 0; decrementado pelo consumidor antes // Descreve o semáforo que conta posições ocupadas. | Dicionário: buffer.full = semáforo de posições cheias/prontas.
 *        de ler (bloqueia quando o buffer está vazio). // Explica o bloqueio do consumidor quando o buffer esvazia. | Dicionário: consumidor = thread que retira dados do buffer.
 * // Linha de espaçamento da documentação. | Dicionário: N/A
 *  - **Mutex `buffer.mutex`:** garante exclusão mútua na secção crítica onde // Explica o mutex que protege os índices do buffer circular. | Dicionário: Mutex = trinco de exclusão mútua (mutual exclusion lock); exclusão mútua = apenas uma thread executa a secção de cada vez.
 *    os índices `in`/`out`/`count` são actualizados — evita corridas de dados // Explica o perigo de atualizações concorrentes. | Dicionário: secção crítica = parte do código que acede a recursos partilhados; corridas de dados = problemas onde o resultado depende da ordem de execução.
 *    entre múltiplos produtores e consumidores. // Continuação do detalhe sobre concorrência. | Dicionário: N/A
 * // Linha de espaçamento da documentação. | Dicionário: N/A
 *  - **`produtores_ativos`:** contador decrementado pelo último produtor a // Explica a variável que rastreia os produtores ativos. | Dicionário: produtores_ativos = variável que conta quantos produtores ainda estão a ler logs.
 *    terminar; quando chega a zero, esse produtor envia `buffer.count + 1` // Explica a lógica de acordar os consumidores no fim do processamento. | Dicionário: buffer.count = contador de itens; + 1 = valor extra de segurança.
 *    sinais em `buffer.full` para acordar todos os consumidores que possam // Detalha o mecanismo de acordar threads bloqueadas no semáforo. | Dicionário: sinais = posts/incrementos num semáforo; acordar = mudar estado da thread de bloqueada para pronta a correr.
 *    estar bloqueados em `sem_wait`, permitindo-lhes detectar o fim e sair. // Explica o objetivo de detetar o fim do trabalho. | Dicionário: sem_wait = função que decrementa/bloqueia num semáforo.
 * // Linha de espaçamento da documentação. | Dicionário: N/A
 *  - **`metrics_mutex`:** mutex independente que protege a actualização da // Explica o mutex separado para a estrutura de estatísticas globais. | Dicionário: metrics_mutex = trinco específico para a estrutura de métricas.
 *    estrutura `Metrics` global partilhada por todos os consumidores. // Descreve a partilha da estrutura. | Dicionário: Metrics = tipo de estrutura que armazena os contadores acumulados.
 *  */ // Fim do bloco de comentário geral do ficheiro. | Dicionário: */ = delimitador de fim de comentários de bloco.
 
#include "parser.h" // Inclui o ficheiro de cabeçalho do parser de logs. | Dicionário: #include = diretiva do pré-processador para incluir arquivos; parser.h = cabeçalho com funções de análise de texto.
#include "posix_io.h" // Inclui funções auxiliares de escrita no terminal. | Dicionário: posix_io.h = cabeçalho personalizado para funções POSIX de entrada/saída.
#include "worker_prodcons.h" // Inclui as definições e assinaturas deste módulo. | Dicionário: worker_prodcons.h = cabeçalho que declara as funções deste ficheiro.
 
#include <errno.h> // Inclui códigos de erro padrão do sistema (como EINTR). | Dicionário: errno.h = biblioteca padrão para códigos de erros numéricos das chamadas de sistema.
#include <fcntl.h> // Inclui definições de controlo de ficheiros (como O_RDONLY). | Dicionário: fcntl.h = biblioteca para operações de controle de descritores de ficheiros.
#include <stdio.h> // Inclui funções padrão de entrada/saída (como perror). | Dicionário: stdio.h = biblioteca de entrada/saída padrão da linguagem C.
#include <stdlib.h> // Inclui funções de utilidade geral (como exit e malloc). | Dicionário: stdlib.h = biblioteca padrão com funções de conversão, memória e controle de execução.
#include <string.h> // Inclui funções para manipulação de strings (como strlen e strcmp). | Dicionário: string.h = biblioteca de manipulação de vetores de caracteres.
#include <unistd.h> // Inclui chamadas de sistema POSIX básicas (como read e close). | Dicionário: unistd.h = biblioteca de chamadas de sistema do padrão POSIX.
#include <pthread.h> // Inclui a API de Threads POSIX para multi-threading. | Dicionário: pthread.h = biblioteca para criação e sincronização de threads.
#include <semaphore.h> // Inclui primitivos de semáforos POSIX para sincronização. | Dicionário: semaphore.h = biblioteca com funções e tipos para semáforos.
#include <sys/stat.h> // Inclui funções para obter metadados de ficheiros (como stat). | Dicionário: sys/stat.h = biblioteca com informações de estado de ficheiros.
 
/** @brief Tamanho do buffer de leitura I/O (em bytes) usado pelo produtor. */ // Documentação Doxygen da macro BUF_SIZE. | Dicionário: @brief = tag Doxygen de resumo curto.
#define BUF_SIZE       4096 // Define o tamanho padrão do bloco lido dos logs (4KB). | Dicionário: #define = diretiva do pré-processador para criar macros; BUF_SIZE = nome da macro de tamanho do buffer.
 
/** @brief Comprimento máximo de uma linha de log suportada pelo produtor. */ // Documentação Doxygen da macro LINE_MAX_LOCAL. | Dicionário: N/A
#define LINE_MAX_LOCAL  512 // Define o limite de caracteres para cada linha de log (512). | Dicionário: LINE_MAX_LOCAL = tamanho máximo suportado por linha de texto.
 
/** // Início de comentário Doxygen para a variável buffer. | Dicionário: / * * = início do bloco de comentário.
 * @brief Buffer circular partilhado entre todos os produtores e consumidores. // Resumo sobre a fila global. | Dicionário: @brief = tag Doxygen de resumo.
 * // Linha de espaçamento do comentário. | Dicionário: N/A
 * @details // Secção de detalhes da fila global. | Dicionário: @details = tag Doxygen de detalhes.
 * Contém o array de `LogEntry`, os índices `in`/`out`/`count`, o mutex de // Detalha a composição e os mecanismos de sincronização associados. | Dicionário: LogEntry = estrutura que representa uma linha de log analisada.
 * acesso e os dois semáforos de controlo de fluxo (`empty` e `full`). // Descreve as variáveis internas de sincronização. | Dicionário: empty = semáforo de controle; full = semáforo de controle.
 * Definido como variável global para ser visível em main_prodcons.c via // Explica porque foi definida no âmbito global. | Dicionário: global = visível em todos os módulos vinculados.
 * `extern` declarado em worker_prodcons.h. // Explica o uso da declaração externa. | Dicionário: extern = palavra-chave do C que declara variável definida noutro ficheiro.
 */ // Fim do comentário Doxygen. | Dicionário: */ = fim do comentário.
LogQueue        buffer; // Instancia a estrutura global de fila circular partilhada. | Dicionário: LogQueue = tipo da estrutura de dados da fila circular; buffer = nome da variável global.
 
/** // Início de comentário Doxygen para produtores_ativos. | Dicionário: / * * = início de bloco.
 * @brief Número de threads produtoras ainda em execução. // Resumo sobre a variável de estado. | Dicionário: @brief = resumo.
 * // Linha de espaçamento. | Dicionário: N/A
 * @details // Detalhes da variável. | Dicionário: @details = detalhes.
 * Inicializado em main() com o número total de produtores lançados. // Explica a sua inicialização. | Dicionário: main() = ponto de entrada do executável.
 * Cada produtor decrementa esta variável (com o mutex do buffer adquirido) // Descreve a alteração segura da variável. | Dicionário: mutex = trinco exclusivo.
 * imediatamente antes de terminar. Quando chega a zero, o último produtor // Explica o encerramento seguro. | Dicionário: N/A
 * sabe que não chegará mais nenhuma entrada ao buffer e acorda os consumidores. // Explica o despertar dos consumidores no final. | Dicionário: N/A
 */ // Fim do comentário. | Dicionário: */ = fim de bloco.
int             produtores_ativos = 0; // Declara e zera a contagem de threads produtoras a correr. | Dicionário: int = tipo inteiro; produtores_ativos = nome da variável global.
 
/** // Início de comentário Doxygen para metrics_mutex. | Dicionário: / * * = início de bloco.
 * @brief Mutex que protege a actualização da estrutura de métricas global. // Resumo sobre o mutex. | Dicionário: @brief = resumo.
 * // Linha de espaçamento. | Dicionário: N/A
 * @details // Detalhes do mutex das métricas. | Dicionário: @details = detalhes.
 * Todos os consumidores partilham a mesma `Metrics`; este mutex garante que // Explica a partilha de recursos entre consumidores. | Dicionário: Metrics = estrutura de métricas globais.
 * apenas um consumidor de cada vez executa `update_metrics`, evitando // Explica a exclusão mútua ao atualizar métricas. | Dicionário: update_metrics = função que atualiza os contadores das métricas.
 * condições de corrida nos contadores acumulados. // Descreve a prevenção de conflitos de concorrência. | Dicionário: condições de corrida = acessos concorrentes destrutivos à memória.
 * Inicializado estaticamente com `PTHREAD_MUTEX_INITIALIZER`. // Indica o modo de inicialização estática. | Dicionário: PTHREAD_MUTEX_INITIALIZER = macro do POSIX para iniciar mutexes estáticos.
 */ // Fim do comentário. | Dicionário: */ = fim de bloco.
pthread_mutex_t metrics_mutex     = PTHREAD_MUTEX_INITIALIZER; // Declara e inicializa o mutex de controlo das métricas globais. | Dicionário: pthread_mutex_t = tipo de mutex POSIX; metrics_mutex = nome da variável; PTHREAD_MUTEX_INITIALIZER = inicializador de mutex padrão.
 
/** // Início de comentário Doxygen para a função esperar. | Dicionário: / * * = início de bloco.
 * @brief Wrapper seguro em torno de `sem_wait` que reomeça se interrompido. // Resumo do wrapper do semáforo. | Dicionário: Wrapper = função que encapsula outra para simplificar ou proteger; sem_wait = decremento de semáforo.
 * // Linha de espaçamento. | Dicionário: N/A
 * @details // Detalhes sobre interrupções por sinais. | Dicionário: @details = detalhes.
 * `sem_wait` pode retornar -1 com `errno == EINTR` quando o processo recebe // Explica a falha por sinal no meio da espera. | Dicionário: errno = variável que regista códigos de erro; EINTR = constante para chamada interrompida por sinal.
 * um sinal durante a espera. O loop garante que a espera recomeça nesse caso, // Explica o tratamento com loop de reinício. | Dicionário: loop = ciclo de repetição.
 * implementando uma espera bloqueante verdadeiramente fiável. // Justifica a criação da função para robustez. | Dicionário: bloqueante = que suspende a execução até ser notificada.
 * // Linha de espaçamento. | Dicionário: N/A
 * @param s  Apontador para o semáforo POSIX a decrementar (bloquear se zero). // Documenta o parâmetro s. | Dicionário: @param = tag Doxygen para parâmetros; sem_t = tipo de semáforo POSIX.
 */ // Fim do comentário. | Dicionário: */ = fim de bloco.
static void esperar(sem_t *s) { // Declara função interna estática para esperar num semáforo de forma robusta. | Dicionário: static = limita a visibilidade da função ao próprio ficheiro; void = indica que não retorna valor; sem_t = tipo de semáforo.
    /* Repetir se a chamada foi interrompida por um sinal (EINTR) */ // Comentário informativo interno sobre o loop de tratamento de sinais. | Dicionário: EINTR = erro de chamada interrompida por sinal.
    while (sem_wait(s) == -1 && errno == EINTR) // Ciclo que tenta decrementar o semáforo e reinicia se a falha for por interrupção de sinal. | Dicionário: while = estrutura de repetição; sem_wait = decrementa o semáforo; s = ponteiro para semáforo; errno = erro global.
        ; // Instrução vazia do ciclo. | Dicionário: ; = terminador de instrução vazio em C.
} // Fim da função esperar. | Dicionário: } = fecha o escopo da função.
 
/** // Início de comentário Doxygen para a função assinalar. | Dicionário: / * * = início de bloco.
 * @brief Wrapper em torno de `sem_post` para incrementar (sinalizar) um semáforo. // Resumo da função de notificação. | Dicionário: sem_post = função POSIX para incrementar semáforo.
 * // Linha de espaçamento. | Dicionário: N/A
 * @details // Detalhes sobre o sem post. | Dicionário: @details = detalhes.
 * Incrementa o semáforo, acordando uma thread que esteja bloqueada em // Explica o efeito de acordar waiters. | Dicionário: waiters = threads bloqueadas à espera de recursos.
 * `esperar()` (i.e., em `sem_wait`). Usado pelo produtor para sinalizar // Descreve os cenários de uso do produtor. | Dicionário: N/A
 * `buffer.full` após inserir uma entrada, e pelo consumidor para sinalizar // Descreve os cenários de uso do consumidor. | Dicionário: N/A
 * `buffer.empty` após retirar uma entrada. // Continuação da explicação de uso. | Dicionário: N/A
 * // Linha de espaçamento. | Dicionário: N/A
 * @param s  Apontador para o semáforo POSIX a incrementar (sinalizar). // Documenta o parâmetro. | Dicionário: @param = parâmetro.
 */ // Fim do comentário. | Dicionário: */ = fim de bloco.
static void assinalar(sem_t *s) { // Declara função estática para incrementar e sinalizar um semáforo. | Dicionário: static = visibilidade interna; void = sem retorno; sem_t = tipo de semáforo.
    sem_post(s);  /* Incrementa o semáforo e acorda um waiter, se existir */ // Efetua o incremento do semáforo POSIX apontado por s. | Dicionário: sem_post = chamada de sistema que incrementa o semáforo.
} // Fim da função assinalar. | Dicionário: } = fecha o escopo da função.
 
/** // Início de comentário Doxygen de init_bounded_buffer. | Dicionário: / * * = início de bloco.
 * @brief Inicializa o bounded buffer e os seus primitivos de sincronização. // Resumo da função de inicialização. | Dicionário: N/A
 * // Linha de espaçamento. | Dicionário: N/A
 * @details // Detalhes da rotina de arranque. | Dicionário: @details = detalhes.
 * Deve ser chamada uma única vez em main() antes de lançar qualquer thread. // Explica quando deve ser executada. | Dicionário: thread = linha de execução leve e concorrente.
 * Configura: // Lista as variáveis inicializadas. | Dicionário: N/A
 *  - `buffer.in = buffer.out = buffer.count = 0` — buffer vazio. // Mostra a zeragem dos índices. | Dicionário: N/A
 *  - `pthread_mutex_init(&buffer.mutex)` — mutex para acesso exclusivo ao buffer. // Mostra a inicialização do mutex. | Dicionário: pthread_mutex_init = cria/inicia um mutex de exclusão mútua.
 *  - `sem_init(&buffer.empty, 0, BUFFER_SIZE)` — há `BUFFER_SIZE` espaços livres. // Mostra a inicialização do semáforo empty. | Dicionário: sem_init = inicializa semáforo POSIX; BUFFER_SIZE = tamanho limite do buffer.
 *  - `sem_init(&buffer.full,  0, 0)` — zero entradas disponíveis para consumir. // Mostra a inicialização do semáforo full. | Dicionário: N/A
 */ // Fim do comentário. | Dicionário: */ = fim de bloco.
void init_bounded_buffer(void) { // Declara a função pública de inicialização do buffer partilhado. | Dicionário: void = indica que não recebe parâmetros e não retorna valores.
    buffer.in    = 0;   /* Índice de escrita: próxima posição livre no array */ // Define o índice de escrita inicial como zero. | Dicionário: buffer = fila partilhada; in = campo do índice de entrada.
    buffer.out   = 0;   /* Índice de leitura: próxima posição a consumir */ // Define o índice de leitura inicial como zero. | Dicionário: out = campo do índice de saída.
    buffer.count = 0;   /* Entradas actualmente no buffer */ // Define a contagem inicial de entradas como zero. | Dicionário: count = campo que regista o número de elementos presentes.
 
    /* Mutex que garante exclusão mútua no acesso aos índices do buffer */ // Comentário informativo interno sobre inicialização de mutex. | Dicionário: Mutex = trinco de controle.
    pthread_mutex_init(&buffer.mutex, NULL); // Inicializa o mutex interno da estrutura de buffer. | Dicionário: pthread_mutex_init = inicializa mutex de threads; buffer.mutex = endereço do mutex a inicializar; NULL = atributos por omissão.
 
    /* // Início de bloco de comentário informativo sobre o semáforo empty. | Dicionário: N/A
     * Semáforo "empty": conta o número de posições livres no buffer circular. // Explica a utilidade do semáforo empty. | Dicionário: N/A
     * Começa em BUFFER_SIZE (buffer completamente vazio). // Indica o valor de arranque. | Dicionário: BUFFER_SIZE = macro para capacidade.
     * O produtor faz sem_wait(empty) antes de escrever — bloqueia se cheio. // Descreve a ação do produtor. | Dicionário: sem_wait = decrementar semáforo.
     * O consumidor faz sem_post(empty) depois de ler — liberta uma posição. // Descreve a ação do consumidor. | Dicionário: sem_post = incrementar semáforo.
     */ // Fim do comentário interno. | Dicionário: N/A
    sem_init(&buffer.empty, 0, BUFFER_SIZE); // Inicializa o semáforo empty com a capacidade total do buffer. | Dicionário: sem_init = inicialização de semáforo; buffer.empty = endereço do semáforo; 0 = partilhado entre threads do mesmo processo.
 
    /* // Início de bloco de comentário informativo sobre o semáforo full. | Dicionário: N/A
     * Semáforo "full": conta o número de entradas disponíveis para consumir. // Explica a utilidade do semáforo full. | Dicionário: N/A
     * Começa em 0 (buffer vazio, nada para consumir). // Indica o valor inicial. | Dicionário: N/A
     * O consumidor faz sem_wait(full) antes de ler — bloqueia se vazio. // Descreve a ação do consumidor. | Dicionário: sem_wait = decrementar.
     * O produtor faz sem_post(full) depois de escrever — sinaliza nova entrada. // Descreve a ação do produtor. | Dicionário: sem_post = incrementar.
     */ // Fim do comentário interno. | Dicionário: N/A
    sem_init(&buffer.full,  0, 0); // Inicializa o semáforo full a zero, representando nenhum elemento inicial. | Dicionário: sem_init = inicializa semáforo; buffer.full = endereço do semáforo.
} // Fim da função init_bounded_buffer. | Dicionário: } = fecha o escopo da função.
 
/** // Início de comentário Doxygen para destroy_bounded_buffer. | Dicionário: / * * = início de bloco.
 * @brief Destroi o mutex e os semáforos do bounded buffer. // Resumo da função de destruição de recursos. | Dicionário: N/A
 * // Linha de espaçamento. | Dicionário: N/A
 * @details // Detalhes da rotina de limpeza de recursos. | Dicionário: @details = detalhes.
 * Deve ser chamada em main() depois de todos os threads terem terminado // Explica a altura adequada para libertar recursos. | Dicionário: N/A
 * (após os `pthread_join`), para libertar os recursos do sistema operativo // Explica a necessidade de esperar pelas threads antes. | Dicionário: pthread_join = bloqueia até a thread alvo terminar.
 * associados ao mutex e aos semáforos POSIX. // Refere os recursos a libertar. | Dicionário: POSIX = interface padronizada de sistemas Unix.
 */ // Fim do comentário. | Dicionário: */ = fim de bloco.
void destroy_bounded_buffer(void) { // Declara a função pública de libertação dos primitivos do buffer. | Dicionário: void = sem parâmetros e sem retorno.
    pthread_mutex_destroy(&buffer.mutex);  /* Libertar recursos do mutex */ // Desativa e desaloca o mutex do buffer. | Dicionário: pthread_mutex_destroy = elimina o mutex; buffer.mutex = trinco do buffer.
    sem_destroy(&buffer.empty);            /* Libertar semáforo de espaços livres */ // Elimina o semáforo empty libertando recursos do sistema. | Dicionário: sem_destroy = elimina o semáforo.
    sem_destroy(&buffer.full);             /* Libertar semáforo de entradas prontas */ // Elimina o semáforo full libertando recursos do sistema. | Dicionário: sem_destroy = destrói semáforo.
} // Fim da função destroy_bounded_buffer. | Dicionário: } = fecha o escopo.
 
/** // Início de comentário Doxygen para inserir_no_buffer. | Dicionário: / * * = início de bloco.
 * @brief Insere uma `LogEntry` no buffer circular (secção do produtor). // Resumo da inserção de dados. | Dicionário: LogEntry = entrada individual de log analisada.
 * // Linha de espaçamento. | Dicionário: N/A
 * @details // Detalhes do algoritmo clássico do lado do produtor. | Dicionário: @details = detalhes.
 * Implementa a metade "produtor" do padrão clássico Produtor-Consumidor: // Explica a lógica de inserção concorrente. | Dicionário: Produtor-Consumidor = padrão estrutural para cooperar processos.
 * // Linha de espaçamento. | Dicionário: N/A
 *  1. `esperar(&buffer.empty)` — decrementa o semáforo de espaços livres; // Passo 1: verificar vaga. | Dicionário: empty = posições livres.
 *     bloqueia se o buffer estiver cheio (count == BUFFER_SIZE). // Bloqueio em caso de buffer cheio. | Dicionário: BUFFER_SIZE = capacidade.
 *  2. `pthread_mutex_lock(&buffer.mutex)` — adquire acesso exclusivo ao buffer // Passo 2: obter exclusão mútua. | Dicionário: pthread_mutex_lock = bloqueia mutex.
 *     para que dois produtores não escrevam na mesma posição. // Previne interferências cruzadas. | Dicionário: N/A
 *  3. Escreve a entrada em `buffer.queue[buffer.in]` e avança `in` em módulo // Passo 3: escrever na fila circular. | Dicionário: queue = vetor interno de elementos; in = índice de escrita.
 *     `BUFFER_SIZE` (comportamento circular). // Detalha aritmética modular. | Dicionário: módulo = resto da divisão.
 *  4. Incrementa `buffer.count`. // Passo 4: adicionar contagem. | Dicionário: count = contador.
 *  5. `pthread_mutex_unlock` — liberta o acesso exclusivo. // Passo 5: desbloquear exclusão mútua. | Dicionário: pthread_mutex_unlock = desbloqueia mutex.
 *  6. `assinalar(&buffer.full)` — incrementa o semáforo de entradas disponíveis, // Passo 6: anunciar a presença de item. | Dicionário: full = posições preenchidas.
 *     acordando um consumidor bloqueado se existir. // Efeito sobre os consumidores em espera. | Dicionário: N/A
 * // Linha de espaçamento. | Dicionário: N/A
 * @param entry  A entrada de log a inserir no buffer. // Parâmetro recebido. | Dicionário: @param = parâmetro; entry = dado a introduzir.
 */ // Fim do comentário. | Dicionário: */ = fim de bloco.
static void inserir_no_buffer(LogEntry entry) { // Declara função interna estática para colocar logs no buffer circular. | Dicionário: static = visibilidade local; void = sem retorno; LogEntry = estrutura de entrada de log.
    /* // Início de comentário interno do Passo 1. | Dicionário: N/A
     * Passo 1 — Esperar por um espaço livre no buffer. // Passo 1: verificar e aguardar vaga. | Dicionário: N/A
     * Se buffer.count == BUFFER_SIZE, sem_wait bloqueia aqui até que um // Explica o bloqueio do produtor. | Dicionário: sem_wait = espera no semáforo.
     * consumidor liberte uma posição com sem_post(&buffer.empty). // Descreve a libertação pelo consumidor. | Dicionário: sem_post = sinaliza semáforo.
     */ // Fim do comentário interno. | Dicionário: N/A
    esperar(&buffer.empty); // Aguarda (e decrementa) uma posição livre no buffer. | Dicionário: esperar = função interna segura de sem_wait; empty = semáforo de vagas.
 
    /* // Início de comentário interno do Passo 2. | Dicionário: N/A
     * Passo 2 — Acesso exclusivo: apenas um produtor de cada vez pode // Explica a necessidade do trinco. | Dicionário: N/A
     * escrever, para evitar corridas de dados no índice `in` e em `count`. // Descreve os campos críticos protegidos. | Dicionário: in = índice; count = contagem.
     */ // Fim do comentário interno. | Dicionário: N/A
    pthread_mutex_lock(&buffer.mutex); // Adquire o mutex de acesso exclusivo ao buffer. | Dicionário: pthread_mutex_lock = trinco de bloqueio; buffer.mutex = endereço do mutex.
 
    /* Passo 3 — Escrita no buffer circular na posição `in` */ // Efetua a cópia física da entrada na vaga atual. | Dicionário: in = índice.
    buffer.queue[buffer.in] = entry; // Copia a estrutura de log recebida para o array do buffer no índice in. | Dicionário: queue = vetor de LogEntry; buffer.in = índice de inserção.
 
    /* Avançar o índice de escrita em módulo BUFFER_SIZE (buffer circular) */ // Incremento circular do ponteiro. | Dicionário: N/A
    buffer.in = (buffer.in + 1) % BUFFER_SIZE; // Atualiza o índice in utilizando o operador de resto para fechar o círculo. | Dicionário: % = operador módulo; BUFFER_SIZE = capacidade da fila.
 
    /* Passo 4 — Registar que há mais uma entrada no buffer */ // Incrementa o número total de elementos válidos no buffer. | Dicionário: N/A
    buffer.count++; // Soma 1 ao contador count de itens na fila circular. | Dicionário: count = número de itens ativos; ++ = incremento de unidade.
 
    /* Passo 5 — Libertar o mutex para que outros produtores possam escrever */ // Fim do período exclusivo de escrita. | Dicionário: N/A
    pthread_mutex_unlock(&buffer.mutex); // Desbloqueia o mutex permitindo que outras threads acedam ao buffer. | Dicionário: pthread_mutex_unlock = chamada POSIX de desbloqueio.
 
    /* // Início de comentário interno do Passo 6. | Dicionário: N/A
     * Passo 6 — Sinalizar que há uma nova entrada disponível para consumir. // Explica a sinalização ao consumidor. | Dicionário: N/A
     * Se um consumidor estiver bloqueado em esperar(&buffer.full), acorda agora. // Explica o despertar de consumidores. | Dicionário: esperar = espera segura; full = semáforo de dados prontos.
     */ // Fim do comentário. | Dicionário: N/A
    assinalar(&buffer.full); // Incrementa o semáforo full e notifica eventuais consumidores suspensos. | Dicionário: assinalar = função de post; buffer.full = semáforo de dados ocupados.
} // Fim da função inserir_no_buffer. | Dicionário: } = fecho de bloco.
 
/** // Início de comentário Doxygen para run_producer. | Dicionário: / * * = início de bloco.
 * @brief Função de thread do produtor: lê ficheiros de log e alimenta o buffer. // Resumo do papel do produtor. | Dicionário: N/A
 * // Linha de espaçamento. | Dicionário: N/A
 * @details // Detalhes da partição lógica de ficheiros por quotas de bytes. | Dicionário: @details = detalhes.
 * Cada produtor recebe uma fatia [byte_inicio, byte_fim[ do espaço virtual // Explica a partilha por fatias lógicas. | Dicionário: fatia = subconjunto consecutivo de bytes.
 * resultante da concatenação lógica de todos os ficheiros de log. O algoritmo: // Explica a concatenação virtual na divisão. | Dicionário: concatenação = união sequencial de ficheiros.
 * // Linha de espaçamento. | Dicionário: N/A
 *  1. Percorre os ficheiros em ordem, calculando os offsets locais da fatia. // Passo 1: mapeamento local de ficheiro. | Dicionário: offsets = deslocamentos em bytes.
 *  2. Salta linhas parciais no início da fatia (para não processar linhas que // Passo 2: sincronização nas bordas. | Dicionário: N/A
 *     pertencem ao produtor anterior — sincronização ao nível de linha). // Evita processar a mesma linha em duplicado. | Dicionário: sincronização = alinhamento.
 *  3. Lê o ficheiro em blocos de `BUF_SIZE` bytes, acumula caracteres em // Passo 3: acumulação e parser de dados. | Dicionário: BUF_SIZE = tamanho do bloco de leitura.
 *     `linha[]` até encontrar `'\n'`, detecta o formato e chama `parse_line`. // Explica a deteção automática do tipo de log. | Dicionário: parse_line = função que descodifica a linha de texto; '\n' = caractere de fim de linha.
 *  4. Cada linha válida é inserida no buffer partilhado via `inserir_no_buffer`. // Passo 4: entrega de resultados. | Dicionário: inserir_no_buffer = envia para a fila.
 *  5. Para ao atingir o fim da sua fatia (`file_pos >= local_end`). // Passo 5: condição de paragem do loop. | Dicionário: file_pos = posição de leitura atual; local_end = limite final da fatia.
 * // Linha de espaçamento. | Dicionário: N/A
 * Ao terminar, decrementa `produtores_ativos` com o mutex adquirido. // Registo da sua saída da execução concorrente. | Dicionário: produtores_ativos = contador de produtores; mutex = trinco exclusivo.
 * Se for o último produtor (`produtores_ativos == 0`), envia sinais extra em // Protocolo do último a sair. | Dicionário: N/A
 * `buffer.full` para acordar todos os consumidores bloqueados. // Acorda consumidores para detetarem fim e saírem. | Dicionário: buffer.full = semáforo de dados ativos.
 * // Linha de espaçamento. | Dicionário: N/A
 * @param arg  Apontador para `ProducerArgs` com a fatia de bytes e metadados. // Documentação do argumento recebido. | Dicionário: @param = parâmetro; ProducerArgs = tipo da estrutura de argumentos do produtor.
 * @return     NULL (convenção pthread). // Tipo de retorno da thread. | Dicionário: @return = retorno; NULL = ponteiro nulo.
 */ // Fim do comentário. | Dicionário: */ = fim de bloco.
void *run_producer(void *arg) { // Ponto de entrada da thread produtora que retorna ponteiro genérico. | Dicionário: void* = tipo de dados de ponteiro genérico; run_producer = nome da função; arg = argumento recebido.
    ProducerArgs *a = (ProducerArgs *)arg; // Converte o ponteiro genérico void* para a estrutura com argumentos do produtor. | Dicionário: ProducerArgs = estrutura com configurações do produtor; a = ponteiro local para os argumentos.
 
    /* Limites da fatia de bytes atribuída a este produtor */ // Variáveis locais de limites da quota de leitura. | Dicionário: N/A
    off_t byte_inicio = a->byte_inicio; // Extrai o ponto inicial da fatia em bytes globais. | Dicionário: off_t = tipo padrão POSIX para offsets de ficheiros; byte_inicio = início da fatia.
    off_t byte_fim    = a->byte_fim; // Extrai o ponto final da fatia em bytes globais. | Dicionário: byte_fim = fim da fatia.
    off_t quota       = byte_fim - byte_inicio;  /* Total de bytes a processar */ // Calcula o tamanho da fatia atribuída a esta thread. | Dicionário: quota = diferença de bytes calculada.
 
    /* Registar o total de bytes para o dashboard de progresso */ // Comunica ao dashboard do monitor a informação do tamanho da quota. | Dicionário: dashboard = painel visual de monitorização.
    if (a->bytes_total) *(a->bytes_total) = (long)quota; // Guarda na variável do dashboard o total de bytes a analisar se o ponteiro for válido. | Dicionário: bytes_total = ponteiro para o total global de bytes do worker; long = inteiro longo.
    if (a->bytes_done)  *(a->bytes_done)  = 0; // Inicializa a variável do dashboard do progresso com zero. | Dicionário: bytes_done = ponteiro para bytes concluídos pelo worker.
 
    /* // Início de bloco explicativo sobre global_offset. | Dicionário: N/A
     * global_offset: posição acumulada no espaço virtual de bytes concatenados. // Explica a lógica de coordenadas globais. | Dicionário: N/A
     * À medida que iteramos os ficheiros, global_offset avança com o tamanho // Explica o avanço acumulado. | Dicionário: N/A
     * de cada um, permitindo mapear byte_inicio/byte_fim para offsets locais. // Explica a utilidade do mapeamento local. | Dicionário: N/A
     */ // Fim do comentário. | Dicionário: N/A
    off_t global_offset = 0; // Variável acumuladora do deslocamento global de ficheiros visitados. | Dicionário: off_t = tipo inteiro para ficheiros; global_offset = offset acumulado.
    char  buf[BUF_SIZE];        /* Buffer de leitura I/O para read() */ // Reserva buffer local para carregar blocos de ficheiros em memória. | Dicionário: char = tipo de caractere de 1 byte; buf = vetor temporário de caracteres; BUF_SIZE = tamanho do bloco de leitura.
    char  linha[LINE_MAX_LOCAL]; /* Buffer para acumular uma linha de log */ // Reserva memória para guardar e montar as linhas de log sequencialmente. | Dicionário: linha = vetor temporário para montar a linha; LINE_MAX_LOCAL = tamanho máximo da linha de log.
 
    for (int i = 0; i < a->total_ficheiros; i++) { // Loop sobre todos os caminhos de ficheiros recebidos nos argumentos. | Dicionário: for = estrutura de repetição; i = índice de iteração; total_ficheiros = quantidade total de caminhos.
        struct stat st; // Cria uma estrutura para obter metadados (como o tamanho) do ficheiro atual. | Dicionário: struct = palavra-chave que declara estrutura de dados; stat = tipo de metadados de ficheiro.
        if (stat(a->ficheiros[i], &st) != 0) continue;  /* Ignorar se inacessível */ // Tenta ler informações do ficheiro. Se falhar, salta para o próximo. | Dicionário: stat = função do sistema que lê metadados; ficheiros = lista de caminhos; continue = instrução que avança para a próxima iteração.
        off_t fsize = st.st_size; // Obtém o tamanho em bytes do ficheiro atual a partir da estrutura stat. | Dicionário: fsize = tamanho do ficheiro; st_size = campo da estrutura contendo o tamanho do ficheiro.
 
        /* Ficheiro completamente antes da nossa fatia → ignorar */ // Comentário informativo de otimização de varrimento de ficheiros. | Dicionário: N/A
        if (global_offset + fsize <= byte_inicio) { // Se a soma do offset e tamanho não atingir o início da nossa fatia, salta o ficheiro. | Dicionário: global_offset = posição no fluxo lógico global; byte_inicio = byte de arranque da thread.
            global_offset += fsize; // Adiciona o tamanho do ficheiro saltado ao deslocamento global. | Dicionário: += = operador de atribuição aditiva.
            continue; // Salta para a próxima iteração do ciclo de ficheiros. | Dicionário: continue = reinicia o loop.
        } // Fim do bloco if. | Dicionário: } = fecho de escopo.
        /* Ficheiro completamente depois da nossa fatia → parar */ // Comentário explicativo de otimização por excesso de avanço. | Dicionário: N/A
        if (global_offset >= byte_fim) break; // Se o offset global já atingiu o fim da nossa fatia, termina o processamento dos ficheiros. | Dicionário: byte_fim = limite de fim da fatia; break = interrompe e sai do ciclo de repetição.
 
        /* // Início de comentário sobre cálculo de coordenadas locais. | Dicionário: N/A
         * Calcular os offsets locais (dentro deste ficheiro) correspondentes // Explica a tradução de global para local. | Dicionário: N/A
         * ao início e fim da nossa fatia global. // Explica as variáveis calculadas. | Dicionário: N/A
         */ // Fim do comentário. | Dicionário: N/A
        off_t local_start = (byte_inicio > global_offset) ? byte_inicio - global_offset : 0; // Calcula a posição inicial de leitura dentro do ficheiro atual. | Dicionário: ? : = operador ternário condicional.
        off_t local_end   = (byte_fim < global_offset + fsize) ? byte_fim - global_offset : fsize; // Calcula a posição final de leitura dentro do ficheiro atual. | Dicionário: local_end = limite de fim no ficheiro local.
 
        /* Abrir o ficheiro em modo só-leitura com a API POSIX */ // Comentário informativo sobre abertura de descritores de ficheiro. | Dicionário: N/A
        int fd = open(a->ficheiros[i], O_RDONLY); // Abre o ficheiro atual apenas para leitura. | Dicionário: open = chamada de sistema para abrir ficheiro; O_RDONLY = flag de apenas leitura; fd = descritor de ficheiro.
        if (fd < 0) { global_offset += fsize; continue; } // Se a abertura falhar, avança o offset e passa ao ficheiro seguinte. | Dicionário: < 0 = indica erro no retorno de chamadas Unix.
 
        /* Modo verbose: imprimir o intervalo de bytes a processar neste ficheiro */ // Informação de log se a flag verbose estiver ativa. | Dicionário: N/A
        if (a->verbose) // Verifica se a thread deve produzir logs textuais detalhados na consola. | Dicionário: verbose = campo bool de mensagens.
            posix_writef(STDOUT_FILENO, // Escreve informação no terminal utilizando a função robusta personalizada. | Dicionário: STDOUT_FILENO = descritor para saída padrão; posix_writef = função personalizada para escrita segura baseada em write().
                         "[Produtor %d] %s bytes [%lld-%lld]\n", // String de formato do print do intervalo. | Dicionário: N/A
                         a->worker_index, a->ficheiros[i], // Argumentos de índice e nome de ficheiro. | Dicionário: worker_index = identificador da thread.
                         (long long)local_start, (long long)local_end); // Argumentos de limites de bytes convertidos para long long. | Dicionário: long long = tipo inteiro de 64 bits em C.
 
        /* Posicionar o descritor de ficheiro no início da nossa fatia */ // Deslocação do descritor de ficheiro. | Dicionário: N/A
        if (lseek(fd, local_start, SEEK_SET) < 0) { // Move a posição do cursor do ficheiro para local_start. | Dicionário: lseek = chamada de sistema para mover posição do cursor no ficheiro; SEEK_SET = modo absoluto a partir do início.
            perror("lseek"); // Imprime o erro correspondente no descritor de erro padrão se lseek falhar. | Dicionário: perror = imprime a mensagem de erro do sistema com base na variável errno.
            close(fd); // Fecha o descritor de ficheiro aberto para libertar recursos. | Dicionário: close = chamada de sistema para fechar descritor.
            global_offset += fsize; // Atualiza o offset global acumulado com o tamanho deste ficheiro. | Dicionário: global_offset = offset global acumulado.
            continue; // Avança para o próximo ficheiro da lista. | Dicionário: continue = reinicia o loop.
        } // Fim do bloco if. | Dicionário: } = fecho de bloco.
 
        off_t file_pos = local_start;  /* Posição actual de leitura no ficheiro */ // Define a variável que controla a posição de leitura física no ficheiro. | Dicionário: file_pos = offset de leitura físico atual.
 
        /* // Início de comentário informativo sobre skip de primeira linha incompleta. | Dicionário: N/A
         * Se não estamos no início do ficheiro, avançar até ao '\n' // Explica a lógica de alinhamento de linha. | Dicionário: N/A
         * para não processar uma linha que pertence ao produtor anterior. // Explica que a linha cortada é tratada pelo vizinho anterior. | Dicionário: N/A
         * Isto garante que cada linha é processada por exactamente um produtor. // Consequência da política de alinhamento de linha. | Dicionário: N/A
         */ // Fim do comentário. | Dicionário: N/A
        if (local_start > 0) { // Se a fatia começar no meio do ficheiro, alinha com a primeira linha inteira. | Dicionário: local_start = offset inicial.
            char c; // Variável temporária para carregar o caractere lido. | Dicionário: char = tipo de caractere.
            ssize_t r; // Variável para capturar o retorno da função read. | Dicionário: ssize_t = tipo inteiro assinado para retorno de I/O.
            while ((r = read(fd, &c, 1)) == 1) { // Lê 1 byte de cada vez e continua enquanto ler com sucesso. | Dicionário: read = chamada de sistema para ler bytes de descritor; &c = endereço do destino; 1 = quantidade de bytes.
                file_pos++; // Incrementa a nossa posição física de leitura. | Dicionário: file_pos = posição local.
                if (c == '\n') break;  /* Encontrou o fim da linha parcial */ // Se encontrar o fim da linha cortada, interrompe a pesquisa de alinhamento. | Dicionário: '\n' = caractere nova linha; break = quebra de loop.
            } // Fim do ciclo. | Dicionário: } = fecho de loop.
            if (r <= 0) { // Se ocorreu fim de ficheiro ou erro de leitura antes de encontrar '\n'. | Dicionário: r = resultado da leitura.
                /* EOF ou erro antes de encontrar '\n' — nada a processar */ // Nada a fazer neste ficheiro. | Dicionário: EOF = fim de ficheiro.
                close(fd); // Fecha o descritor do ficheiro. | Dicionário: close = fecha descritor.
                global_offset += fsize; // Incrementa o offset virtual global com o tamanho deste ficheiro. | Dicionário: global_offset = offset global.
                continue; // Salta para a leitura do próximo ficheiro da lista. | Dicionário: continue = reinicia o loop.
            } // Fim do if. | Dicionário: } = fecho de bloco.
        } // Fim do bloco de alinhamento de linha. | Dicionário: } = fecho de bloco.
 
        int len  = 0;              /* Número de caracteres acumulados em `linha` */ // Inicializa o acumulador do comprimento da linha. | Dicionário: len = tamanho acumulado localmente.
        int done = 0;              /* Flag: 1 quando a fatia foi completamente lida */ // Flag de controlo para saber se a quota de leitura já esgotou. | Dicionário: done = indicador boleano de conclusão.
        LogFormat fmt = FORMAT_UNKNOWN;  /* Formato do ficheiro (detectado na 1ª linha) */ // Inicia o formato de logs como desconhecido. | Dicionário: LogFormat = tipo enumerado de formato de log; FORMAT_UNKNOWN = constante de formato indefinido.
 
        /* Loop principal de leitura: lê o ficheiro em blocos de BUF_SIZE bytes */ // Processamento interno. | Dicionário: N/A
        while (!done) { // Continua a leitura do bloco enquanto a fatia de bytes não estiver concluída. | Dicionário: while = ciclo de repetição; done = flag lógica.
            ssize_t n = read(fd, buf, BUF_SIZE); // Lê um bloco do ficheiro até BUF_SIZE bytes para o buffer buf. | Dicionário: read = leitura física; n = número real de bytes lidos.
            if (n <= 0) break;  /* EOF ou erro de I/O */ // Se ler zero bytes (fim de ficheiro) ou negativo (erro), sai do loop. | Dicionário: break = quebra de ciclo.
 
            /* Actualizar o progresso em bytes para o dashboard do monitor */ // Comunicação do avanço atualizado. | Dicionário: N/A
            off_t pos_na_quota = (global_offset + file_pos) - byte_inicio; // Calcula a quantidade de bytes lidos no total da fatia. | Dicionário: pos_na_quota = progresso acumulado.
            if (pos_na_quota < 0)     pos_na_quota = 0; // Clampa o progresso para não ser negativo (por exemplo, devido ao alinhamento de início). | Dicionário: pos_na_quota = progresso da quota.
            if (pos_na_quota > quota) pos_na_quota = quota; // Clampa o progresso de modo a não ultrapassar a quota atribuída à thread. | Dicionário: quota = total atribuído.
            if (a->bytes_done) *(a->bytes_done) = (long)pos_na_quota; // Escreve o valor calculado na variável monitorizada se o ponteiro existir. | Dicionário: bytes_done = endereço de escrita de progresso; long = tipo de dados inteiro longo.
 
            /* Processar byte a byte o bloco lido */ // Ciclo de parser do bloco em memória. | Dicionário: N/A
            for (ssize_t b = 0; b < n && !done; b++) { // Itera sobre cada byte lido na chamada de leitura física. | Dicionário: for = loop de iteração; b = índice do buffer; n = total de bytes lidos.
                char c = buf[b]; // Obtém o caractere no índice b do buffer. | Dicionário: buf = buffer de leitura; c = caractere.
                file_pos++; // Incrementa a posição de leitura física no ficheiro. | Dicionário: file_pos = ponteiro lógico de leitura.
 
                if (c == '\n') { // Se encontrar o caractere indicador de nova linha. | Dicionário: '\n' = caractere nova linha.
                    /* Fim de linha: processar a linha acumulada em `linha[]` */ // Comentário explicativo interno do parse da linha. | Dicionário: N/A
                    if (len > 0) { // Se a linha acumulada tiver caracteres válidos. | Dicionário: len = comprimento.
                        linha[len] = '\0';  /* Terminar a string C */ // Adiciona o terminador nulo para formar uma string C válida. | Dicionário: '\0' = terminador de string.
 
                        /* Detectar o formato do ficheiro (Apache, JSON, etc.) na primeira linha válida */ // Comentário informativo sobre deteção inicial. | Dicionário: N/A
                        if (fmt == FORMAT_UNKNOWN) // Se o formato ainda não foi determinado para este ficheiro. | Dicionário: fmt = formato de log.
                            fmt = detect_format(linha); // Invoca a função que deduz o formato a partir do texto. | Dicionário: detect_format = função que lê linha e retorna formato de log.
 
                        /* Fazer parse da linha para uma LogEntry estruturada */ // Conversão para campos estruturados. | Dicionário: N/A
                        LogEntry entry; // Variável local que guardará os campos da linha descodificada. | Dicionário: LogEntry = estrutura de campos de log (IP, data, nível, etc.).
                        if (parse_line(linha, fmt, &entry) == 0) // Executa o parser e, se tiver sucesso (retorna 0), prossegue. | Dicionário: parse_line = função que faz split do texto e valida dados; entry = resultado.
                            inserir_no_buffer(entry);  /* Produzir: inserir no buffer partilhado */ // Coloca a entrada de log estruturada na fila circular concorrente. | Dicionário: inserir_no_buffer = envia à fila circular.
 
                        len = 0;  /* Reiniciar o acumulador de linha */ // Zera o tamanho acumulado para começar a acumular a linha seguinte. | Dicionário: len = comprimento acumulado.
                    } // Fim do if len > 0. | Dicionário: } = fecho de bloco.
                    /* Verificar se já atingimos o fim da nossa fatia de bytes */ // Verifica o limite de avanço de leitura física. | Dicionário: N/A
                    if (file_pos >= local_end) done = 1; // Se a posição atingir o final da quota do ficheiro, marca done para parar. | Dicionário: local_end = limite superior; done = flag boleana de saída.
                } else if (c != '\r') { // Se não for mudança de linha mas for caractere comum, ignorando retornos de carro Windows. | Dicionário: '\r' = retorno de carro em ficheiros Windows.
                    /* Acumular o caractere na linha (ignorar '\r' de fins de linha Windows) */ // Acumulação. | Dicionário: N/A
                    if (len < LINE_MAX_LOCAL - 1) // Se ainda houver espaço no buffer para acumular sem transbordar. | Dicionário: LINE_MAX_LOCAL = tamanho limite.
                        linha[len++] = c; // Guarda o caractere no buffer e incrementa o tamanho acumulado. | Dicionário: len++ = pós-incremento de len.
                } // Fim do if-else de leitura de caractere. | Dicionário: } = fecho de bloco.
            } // Fim do ciclo de iteração do buffer. | Dicionário: } = fecho de bloco.
        } // Fim do ciclo de leitura física de blocos. | Dicionário: } = fecho de bloco.
 
        /* Última linha sem '\n' no final do ficheiro — processar igualmente */ // Processa restos do buffer caso o ficheiro não termine com newline. | Dicionário: N/A
        if (len > 0) { // Se restou texto no acumulador de linha após o fim do ficheiro. | Dicionário: len = comprimento.
            linha[len] = '\0'; // Insere o terminador nulo para formar a string C final. | Dicionário: '\0' = terminador de string.
            if (fmt == FORMAT_UNKNOWN) fmt = detect_format(linha); // Garante a deteção de formato se a linha isolada for a primeira do ficheiro. | Dicionário: detect_format = deteta formato de logs.
            LogEntry entry; // Cria variável para guardar a estrutura processada do log final. | Dicionário: LogEntry = estrutura do log.
            if (parse_line(linha, fmt, &entry) == 0) // Efetua o processamento sintático da última linha. | Dicionário: parse_line = analisa a linha.
                inserir_no_buffer(entry);  /* Inserir a última entrada no buffer */ // Envia para o buffer partilhado circular de forma síncrona. | Dicionário: inserir_no_buffer = adiciona na fila circular.
        } // Fim do bloco if. | Dicionário: } = fecho de bloco.
 
        close(fd);             /* Fechar o descritor de ficheiro POSIX */ // Encerra o descritor e liberta recursos associados. | Dicionário: close = chamada de fecho de descritor.
        global_offset += fsize; /* Avançar o offset virtual para o próximo ficheiro */ // Soma o tamanho completo do ficheiro analisado ao deslocamento acumulado global. | Dicionário: global_offset = deslocamento global.
    } // Fim do ciclo de ficheiros. | Dicionário: } = fecho de bloco.
 
    /* Forçar progresso a 100% no dashboard ao terminar */ // Ajuste final do dashboard visual. | Dicionário: N/A
    if (a->bytes_done) *(a->bytes_done) = (long)quota; // Atualiza o progresso para o valor total da quota para ficar visualmente completo. | Dicionário: bytes_done = ponteiro de progresso do monitor.
 
    /* // Início do bloco de comentários sobre a sinalização de fim das threads. | Dicionário: N/A
     * Sinalização de fim ao último produtor: // Lógica aplicada apenas quando a última thread conclui. | Dicionário: N/A
     * // Linha de espaçamento. | Dicionário: N/A
     * Adquirir o mutex do buffer para decrementar `produtores_ativos` de forma // Explica a atonicidade necessária no contador. | Dicionário: N/A
     * atómica relativamente aos consumidores (que também lêem esta variável // Prevenção de conflito de escrita/leitura concorrente. | Dicionário: N/A
     * com o mutex adquirido para decidir se devem terminar). // Justificação do bloqueio exclusivo. | Dicionário: N/A
     * // Linha de espaçamento. | Dicionário: N/A
     * Se este for o ÚLTIMO produtor (produtores_ativos chega a 0): // Cenário final. | Dicionário: N/A
     *   - Calcular quantos consumidores podem estar bloqueados em sem_wait(full). // Contagem de despertares necessários. | Dicionário: N/A
     *     No pior caso são todos os NUM_CONSUMERS consumidores, mais os que // Detalhes da estimativa de envio de sinais. | Dicionário: N/A
     *     já retiraram mas voltaram a bloquear. Enviar buffer.count + 1 sinais // Envio de sinais extra. | Dicionário: N/A
     *     extra garante que todos os consumidores acordam e verificam a condição // Garantia de paragem e escape dos ciclos de consumo. | Dicionário: N/A
     *     de paragem (count == 0 && produtores_ativos == 0). // Condição lógica que dita o fim dos consumidores. | Dicionário: N/A
     */ // Fim do comentário. | Dicionário: N/A
    pthread_mutex_lock(&buffer.mutex); // Adquire o trinco exclusivo do buffer circular. | Dicionário: pthread_mutex_lock = trinco exclusivo.
    produtores_ativos--;  /* Registar que este produtor terminou */ // Decrementa a contagem partilhada de produtores em execução. | Dicionário: produtores_ativos = contador de threads produtoras ativas.
    int consumidores_a_acordar = (produtores_ativos == 0) ? buffer.count + 1 : 0; // Calcula os sinais a enviar se formos o último produtor. | Dicionário: consumidores_a_acordar = variável temporária de contagem de acordares; buffer.count = elementos no buffer.
    pthread_mutex_unlock(&buffer.mutex); // Desbloqueia o trinco exclusivo do buffer circular. | Dicionário: pthread_mutex_unlock = chamada POSIX de desbloqueio.
 
    /* Acordar consumidores bloqueados em sem_wait(&buffer.full) */ // Envio das notificações aos consumidores. | Dicionário: N/A
    for (int k = 0; k < consumidores_a_acordar; k++) // Ciclo de envio de posts dependendo do valor calculado. | Dicionário: for = ciclo; k = contador; consumidores_a_acordar = total de posts.
        assinalar(&buffer.full);  /* sem_post: acordar um consumidor por chamada */ // Notifica/incrementa o semáforo full para reativar um consumidor suspenso. | Dicionário: assinalar = incrementa semáforo; buffer.full = semáforo de posições ocupadas.
 
    return NULL; // Retorna ponteiro nulo conforme especificado pela assinatura da thread. | Dicionário: return = instrução de devolução de valor; NULL = macro para ponteiro nulo.
} // Fim da função run_producer. | Dicionário: } = fecho de escopo da função.
 
/** // Início de comentário Doxygen para run_consumer. | Dicionário: / * * = início de bloco.
 * @brief Função de thread do consumidor: retira entradas do buffer e actualiza métricas. // Resumo da rotina do consumidor. | Dicionário: N/A
 * // Linha de espaçamento. | Dicionário: N/A
 * @details // Detalhes da rotina concorrente e condição de paragem do consumidor. | Dicionário: @details = detalhes.
 * Implementa a metade "consumidor" do padrão Produtor-Consumidor: // Descrição do comportamento clássico de consumo. | Dicionário: N/A
 * // Linha de espaçamento. | Dicionário: N/A
 *  1. `esperar(&buffer.full)` — bloqueia enquanto o buffer estiver vazio. // Passo 1: aguardar elementos. | Dicionário: N/A
 *  2. Adquire o mutex e verifica as condições de paragem e de buffer vazio. // Passo 2: verificação de segurança exclusiva. | Dicionário: N/A
 *  3. Retira a entrada em `buffer.queue[buffer.out]`, avança `out` em módulo // Passo 3: leitura circular dos dados. | Dicionário: out = índice de leitura.
 *     `BUFFER_SIZE` e decrementa `count`. // Detalhe da contagem e índices. | Dicionário: count = elementos; BUFFER_SIZE = tamanho do buffer.
 *  4. Liberta o mutex e sinaliza `buffer.empty` (espaço livre para produtores). // Passo 4: libertar e notificar espaço vago. | Dicionário: empty = semáforo de espaços vazios.
 *  5. Actualiza as métricas globais com o mutex `metrics_mutex`. // Passo 5: fusão final de dados. | Dicionário: metrics_mutex = trinco de estatísticas.
 * // Linha de espaçamento. | Dicionário: N/A
 * **Condição de paragem:** `buffer.count == 0 && produtores_ativos == 0`. // Especificação matemática da paragem do processamento global. | Dicionário: N/A
 * Quando um consumidor detecta esta condição, reenvia o sinal em `buffer.full` // Protocolo de cascata para encerramento de threads. | Dicionário: N/A
 * (passa o testemunho) para acordar os outros consumidores ainda bloqueados, // Acorda o vizinho concorrente. | Dicionário: N/A
 * e termina com `break`. // Abandona o ciclo infinito e encerra a thread. | Dicionário: N/A
 * // Linha de espaçamento. | Dicionário: N/A
 * @param arg  Apontador para `ConsumerArgs` com `global_metrics` e `metrics_mutex`. // Documentação do parâmetro. | Dicionário: @param = parâmetro; ConsumerArgs = tipo dos argumentos do consumidor.
 * @return     NULL (convenção pthread). // Documentação de retorno. | Dicionário: @return = retorno.
 */ // Fim do comentário. | Dicionário: */ = fim de bloco.
void *run_consumer(void *arg) { // Ponto de entrada da thread consumidora que retorna ponteiro genérico. | Dicionário: void* = tipo de ponteiro genérico; run_consumer = nome da função.
    ConsumerArgs *a = (ConsumerArgs *)arg; // Converte o argumento void* para a estrutura com metadados do consumidor. | Dicionário: ConsumerArgs = estrutura de argumentos do consumidor; a = ponteiro para os argumentos.
 
    while (1) { // Loop infinito de consumo, que se mantém ativo até ser explicitamente forçada a saída. | Dicionário: while = ciclo de repetição; 1 = condição constante para infinito.
        /* // Início de comentário informativo do Passo 1. | Dicionário: N/A
         * Passo 1 — Aguardar uma entrada disponível no buffer. // Passo 1: suspensão em semáforo se vazio. | Dicionário: N/A
         * sem_wait decrementa buffer.full; bloqueia se for zero (buffer vazio). // Mecanismo de bloqueio síncrono. | Dicionário: sem_wait = suspensão síncrona.
         * Pode ser acordado de dois modos: // Lista as origens de despertamento do consumidor. | Dicionário: N/A
         *   a) Por um produtor após inserir uma entrada (caso normal). // Despertar normal por produção. | Dicionário: N/A
         *   b) Pelo último produtor que envia sinais extra para acordar todos // Despertar no fim do processamento global. | Dicionário: N/A
         *      os consumidores quando produtores_ativos chega a zero. // Condição de saída para fecho de threads. | Dicionário: N/A
         */ // Fim do comentário. | Dicionário: N/A
        esperar(&buffer.full); // Bloqueia e aguarda até que o semáforo full seja assinalado (maior que zero). | Dicionário: esperar = função interna segura; buffer.full = semáforo de itens no buffer.
 
        /* Passo 2 — Acesso exclusivo para verificar estado e retirar entrada */ // Início do processamento exclusivo. | Dicionário: N/A
        pthread_mutex_lock(&buffer.mutex); // Adquire acesso exclusivo ao buffer circular partilhado. | Dicionário: pthread_mutex_lock = trinco de bloqueio.
 
        /* // Início do bloco de comentários sobre a condição de paragem no consumidor. | Dicionário: N/A
         * Condição de paragem: buffer vazio E nenhum produtor activo. // Condição limite de encerramento do analisador. | Dicionário: N/A
         * Não chegará mais nenhuma entrada — é seguro terminar. // Confirmação de segurança. | Dicionário: N/A
         * Reenviar o sinal em buffer.full para passar o testemunho aos // Lógica de propagação de despertamentos (cascade wake-up). | Dicionário: N/A
         * outros consumidores ainda bloqueados (padrão "cascade wake-up"). // Evita bloqueio indefinido dos consumidores remanescentes. | Dicionário: N/A
         */ // Fim do comentário. | Dicionário: N/A
        if (buffer.count == 0 && produtores_ativos == 0) { // Se não houver dados e mais nenhum produtor estiver ativo. | Dicionário: if = estrutura condicional; buffer.count = itens na fila; produtores_ativos = produtores pendentes.
            pthread_mutex_unlock(&buffer.mutex); // Liberta o mutex do buffer para outros consumidores antes de sair. | Dicionário: pthread_mutex_unlock = desbloqueia mutex.
            assinalar(&buffer.full);  /* Acordar o próximo consumidor bloqueado */ // Envia sinal de propagação em cascata no semáforo full. | Dicionário: assinalar = incrementa semáforo; buffer.full = semáforo de dados.
            break;                    /* Terminar o loop de consumo */ // Sai do ciclo infinito de consumo. | Dicionário: break = quebra de ciclo.
        } // Fim do bloco if. | Dicionário: } = fecho de bloco.
 
        /* // Início de comentário interno para evitar acordares espúrios no buffer. | Dicionário: N/A
         * Protecção contra acordar espúrio (spurious wakeup) ou sinal extra // Tratamento contra despertares indesejados. | Dicionário: spurious wakeup = despertar involuntário de thread sem sinal.
         * do produtor quando o buffer já foi esvaziado por outro consumidor. // Caso particular de sinais extra de fim. | Dicionário: N/A
         */ // Fim do comentário. | Dicionário: N/A
        if (buffer.count == 0) { // Se acordámos mas a contagem está vazia (por exemplo, devido a um sinal extra de paragem concorrente). | Dicionário: buffer.count = contador de itens.
            pthread_mutex_unlock(&buffer.mutex); // Liberta o mutex temporariamente para não bloquear o sistema. | Dicionário: pthread_mutex_unlock = liberta trinco.
            continue;  /* Voltar a esperar sem consumir */ // Volta para o início do ciclo, reentrando na espera de semáforo. | Dicionário: continue = reinicia o loop.
        } // Fim do bloco if. | Dicionário: } = fecho de bloco.
 
        /* Passo 3 — Retirar a entrada mais antiga do buffer circular */ // Efetua a extração física do log. | Dicionário: N/A
        LogEntry entry = buffer.queue[buffer.out]; // Lê o elemento do array do buffer apontado pelo índice de leitura out. | Dicionário: LogEntry = estrutura; queue = array de logs; buffer.out = índice de saída.
 
        /* Avançar o índice de leitura em módulo BUFFER_SIZE (buffer circular) */ // Incremento circular do ponteiro de leitura. | Dicionário: N/A
        buffer.out = (buffer.out + 1) % BUFFER_SIZE; // Atualiza o índice de leitura com incremento modular. | Dicionário: % = operador módulo; BUFFER_SIZE = tamanho da fila circular.
 
        /* Registar que há menos uma entrada no buffer */ // Atualização da contagem de dados. | Dicionário: N/A
        buffer.count--; // Decrementa a variável de contagem de elementos na fila. | Dicionário: buffer.count = contador; -- = decremento unitário.
 
        /* Passo 4 — Libertar o mutex e sinalizar que há uma posição livre */ // Conclusão do acesso exclusivo ao buffer circular. | Dicionário: N/A
        pthread_mutex_unlock(&buffer.mutex); // Liberta o trinco do buffer para outros produtores ou consumidores poderem aceder. | Dicionário: pthread_mutex_unlock = desbloqueia mutex.
 
        /* // Início de comentário interno. | Dicionário: N/A
         * Sinalizar ao produtor que há um espaço livre no buffer. // Explica a sinalização ao produtor. | Dicionário: N/A
         * Se um produtor estiver bloqueado em esperar(&buffer.empty), acorda agora. // Explica que se o produtor estivesse suspenso, retoma. | Dicionário: empty = semáforo de espaços vagos.
         */ // Fim do comentário. | Dicionário: N/A
        assinalar(&buffer.empty); // Sinaliza e incrementa o semáforo empty, notificando eventuais produtores em espera por vaga. | Dicionário: assinalar = incrementa semáforo; buffer.empty = semáforo de posições vazias.
 
        /* // Início de comentário sobre atualização síncrona de métricas. | Dicionário: N/A
         * Passo 5 — Actualizar as métricas globais com exclusão mútua. // Passo 5: registo exclusivo dos contadores de estatísticas. | Dicionário: N/A
         * metrics_mutex é diferente de buffer.mutex: protege apenas a estrutura // Descreve a vantagem de separar os mutexes de controlo. | Dicionário: N/A
         * Metrics, permitindo que a inserção/remoção do buffer continue em // Explica o aumento do paralelismo de inserção. | Dicionário: N/A
         * paralelo com a actualização das métricas. // Explica que as métricas não atrasam o fluxo circular. | Dicionário: N/A
         */ // Fim do comentário. | Dicionário: N/A
        pthread_mutex_lock(a->metrics_mutex); // Bloqueia o mutex das métricas partilhado para garantir escrita exclusiva. | Dicionário: pthread_mutex_lock = trinco exclusivo; metrics_mutex = ponteiro para mutex das métricas.
        update_metrics(a->global_metrics, &entry);  /* Acumular contadores da entrada */ // Funde os dados da entrada retirada nos contadores acumulados globais. | Dicionário: update_metrics = função de soma de métricas; global_metrics = ponteiro para estrutura global.
        pthread_mutex_unlock(a->metrics_mutex); // Liberta o mutex das métricas permitindo acesso a outras threads consumidoras. | Dicionário: pthread_mutex_unlock = desbloqueio de mutex.
    } // Fim do ciclo while. | Dicionário: } = fecho de loop.
 
    return NULL; // Retorna ponteiro nulo em conformidade com as exigências POSIX. | Dicionário: return = instrução de devolução de valor; NULL = macro para ponteiro nulo.
} // Fim da função run_consumer. | Dicionário: } = fecho de escopo da função.