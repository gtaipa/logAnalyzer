/**
 * @file main_sockets.c
 * @brief Processo PAI — servidor Unix Domain Socket e orquestrador de workers.
 *
 * @details
 * Este ficheiro implementa o ponto de entrada do programa e toda a lógica
 * do processo pai.  O seu funcionamento divide-se nas seguintes etapas:
 *
 *  1. **Descoberta de ficheiros** — lê o directório passado como argumento e
 *     recolhe os caminhos de todos os ficheiros `.log` e `.json` encontrados.
 *
 *  2. **Criação do socket servidor** — cria um socket do domínio Unix
 *     (`AF_UNIX, SOCK_STREAM`) e associa-o a um ficheiro especial em
 *     `/tmp` (via `bind()`).  Fica à escuta de ligações entrantes com
 *     `listen()`.  Este socket serve de canal IPC (Inter-Process
 *     Communication) entre o pai e os N filhos.
 *
 *  3. **Criação dos processos filho** — o pai chama `fork()` N vezes,
 *     produzindo N cópias de si mesmo.  Cada filho fecha o file descriptor
 *     do socket servidor (não precisa de o aceitar) e chama `run_worker()`.
 *
 *  4. **Aceitação das ligações (`accept`)** — o pai aceita uma ligação por
 *     cada filho (`accept()`) e envia imediatamente uma mensagem `MSG_CONFIG`
 *     com a fatia de bytes que aquele worker deve processar.  O `select()`
 *     garante que o pai não bloqueia indefinidamente à espera de um filho
 *     que possa nunca ligar.
 *
 *  5. **Loop de recepção com `select()`** — o pai aguarda actividade em
 *     qualquer dos sockets dos filhos sem bloquear num único:
 *       - `MSG_PROGRESSO` → actualiza a barra de progresso no ecrã.
 *       - `MSG_RESULTADO` → acumula as métricas no total global.
 *
 *  6. **Recolha de zombies** — após todos os resultados estarem reunidos,
 *     o pai chama `waitpid()` para cada filho, evitando que os processos
 *     terminados fiquem em estado zombie.
 *
 *  7. **Relatório final** — imprime as métricas acumuladas e o tempo total.
 *
 * **Protocolo de mensagens (pai ↔ worker):**
 * ```
 *   Pai  → Worker :  [int MSG_CONFIG]     + [WorkerConfig]
 *   Worker → Pai  :  [int MSG_PROGRESSO]  + [ProgressUpdate]   (repetido)
 *   Worker → Pai  :  [int MSG_RESULTADO]  + [WorkerResult]      (único, final)
 * ```
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "ipc.h"
#include "parser.h"
#include "worker.h"

/*
 * Os valores abaixo redefinidos localmente coincidem com os de ipc.h.
 * São mantidos aqui para deixar claro, no próprio ficheiro do pai, quais
 * os tipos de mensagem que ele conhece e usa.
 */
#define MSG_PROGRESSO 1  /**< @brief Tipo de mensagem: atualização de progresso (worker → pai). */
#define MSG_RESULTADO 2  /**< @brief Tipo de mensagem: resultado final          (worker → pai). */

/* ═════════════════════════════════════════════════════════════════════════════
 * DASHBOARD
 *
 * O dashboard funciona com um truque de terminal:
 *   - Imprimimos uma linha por worker no início (para "reservar" espaço).
 *   - Quando queremos actualizar, subimos o cursor N linhas com \033[NA,
 *     apagamos tudo abaixo com \033[J, e voltamos a imprimir as barras.
 *   - O resultado visual é uma animação in-place sem scroll.
 * ════════════════════════════════════════════════════════════════════════════= */

/** @brief Largura (em caracteres '#'/'.')  da barra de progresso no terminal. */
#define LARGURA_BARRA 20

/**
 * @brief Redesenha o dashboard de progresso de todos os workers no terminal.
 *
 * @details
 * Usa sequências de escape ANSI para mover o cursor para cima e apagar as
 * linhas antigas antes de as redesenhar com os valores actualizados.
 * O truque evita scroll desnecessário e dá uma animação in-place:
 *   - `\033[NA`  — sobe o cursor N linhas.
 *   - `\033[J`   — apaga tudo do cursor até ao fim do ecrã.
 *
 * @param progressos   Array com o último estado de progresso de cada worker.
 * @param num_workers  Número de workers (e de linhas do dashboard).
 */
static void desenhar_dashboard(ProgressUpdate *progressos, int num_workers) {
    /* Subir o cursor tantas linhas quantos workers existem */
    printf("\033[%dA", num_workers);
    /* Apagar tudo abaixo do cursor (as linhas antigas) */
    printf("\033[J");

    for (int i = 0; i < num_workers; i++) {
        long feitas = progressos[i].bytes_done;
        long total  = progressos[i].bytes_total;

        /* Calcular percentagem; proteger divisão por zero */
        int pct = (total > 0) ? (int)(feitas * 100 / total) : 0;
        if (pct > 100) pct = 100;

        /* Construir a barra de progresso com '#' e '.' */
        char barra[LARGURA_BARRA + 1];
        int cheio = pct * LARGURA_BARRA / 100;
        for (int b = 0; b < LARGURA_BARRA; b++)
            barra[b] = (b < cheio) ? '#' : '.';
        barra[LARGURA_BARRA] = '\0';

        printf("Worker %2d [%s] %3d%% (%ld/%ld bytes)\n",
               i, barra, pct, feitas, total);
    }

    fflush(stdout); /* forçar o flush — o terminal pode ter buffering */
}

/* ═════════════════════════════════════════════════════════════════════════════
 * UTILITÁRIOS DO PAI
 * ════════════════════════════════════════════════════════════════════════════= */

/**
 * @brief Liberta toda a memória alocada para o array de caminhos de ficheiros.
 *
 * @param ficheiros  Array de strings alocadas com strdup().
 * @param total      Número de entradas no array.
 */
static void libertar_ficheiros(char **ficheiros, int total) {
    /* Libertar cada string individualmente (foram alocadas com strdup) */
    for (int i = 0; i < total; i++) free(ficheiros[i]);
    /* Libertar o array de ponteiros */
    free(ficheiros);
}

/**
 * @brief Calcula o número total de bytes de todos os ficheiros via `stat()`.
 *
 * @details
 * Usa a chamada ao sistema `stat()` em vez de abrir os ficheiros, o que é
 * muito mais eficiente: apenas lê os metadados do inode, sem transferir
 * conteúdo.  O resultado é usado pelo pai para dividir o trabalho
 * proporcionalmente entre os N workers.
 *
 * @param ficheiros        Array de caminhos dos ficheiros.
 * @param total_ficheiros  Número de ficheiros no array.
 * @return Soma dos tamanhos (em bytes) de todos os ficheiros.
 */
static off_t obter_bytes_totais(char **ficheiros, int total_ficheiros) {
    off_t total = 0;
    struct stat st;
    for (int i = 0; i < total_ficheiros; i++) {
        /* stat() preenche st.st_size com o tamanho do ficheiro em bytes
         * sem o precisar de abrir — apenas consulta o inode no VFS */
        if (stat(ficheiros[i], &st) == 0)
            total += st.st_size;
    }
    return total;
}

/**
 * @brief Lê um directório e devolve um array de caminhos dos ficheiros `.log` e `.json`.
 *
 * @details
 * Usa `opendir()` / `readdir()` / `closedir()` para iterar as entradas do
 * directório sem recorrer a `glob` ou `find`.  O array cresce dinamicamente
 * com `realloc()` quando necessário.
 *
 * @param dir       Caminho do directório a ler.
 * @param total_out Ponteiro onde é escrito o número de ficheiros encontrados.
 * @return Array alocado dinamicamente com os caminhos completos; o chamador
 *         deve libertá-lo com libertar_ficheiros().
 */
static char **ler_directorio(const char *dir, int *total_out) {
    int capacidade = 10;   /* capacidade inicial do array dinâmico */
    int total = 0;
    char **ficheiros = malloc(capacidade * sizeof(char *));
    if (!ficheiros) { perror("malloc"); exit(1); }

    /* opendir() devolve um handle para o directório — equivale a abrir um
     * "stream" de entradas de directório, sem carregar tudo para memória */
    DIR *d = opendir(dir);
    if (!d) { perror("opendir"); exit(1); }

    struct dirent *entrada;
    /* readdir() avança uma entrada de cada vez; devolve NULL no fim */
    while ((entrada = readdir(d)) != NULL) {
        char *nome = entrada->d_name;
        int len = strlen(nome);

        /* Só nos interessam ficheiros .log e .json */
        int e_log  = (len > 4 && strcmp(nome + len - 4, ".log")  == 0);
        int e_json = (len > 5 && strcmp(nome + len - 5, ".json") == 0);
        if (!e_log && !e_json) continue;

        /* Crescer o array se necessário — realloc preserva o conteúdo existente */
        if (total == capacidade) {
            capacidade *= 2;
            ficheiros = realloc(ficheiros, capacidade * sizeof(char *));
            if (!ficheiros) { perror("realloc"); exit(1); }
        }

        /* Guardar o caminho completo: "directorio/ficheiro.log" */
        char caminho[512];
        snprintf(caminho, sizeof(caminho), "%s/%s", dir, nome);
        /* strdup() aloca memória nova e copia a string — chamador responsável
         * por libertar com free() */
        ficheiros[total++] = strdup(caminho);
    }

    /* closedir() liberta o handle do directório e fecha o file descriptor
     * associado ao directório aberto pelo kernel */
    closedir(d);
    *total_out = total;
    return ficheiros;
}

/**
 * @brief Acumula as métricas de um worker no total global, incluindo os Top 10 IPs.
 *
 * @details
 * Soma contadores simples directamente.  Para os IPs, mantém uma lista
 * global de até 256 entradas e usa uma ordenação por bolha para manter
 * apenas os 10 mais frequentes em `total->top_ips`.
 *
 * @param total             Estrutura de resultados acumulados (alterada in-place).
 * @param r                 Resultado do worker a integrar.
 * @param ip_list_global    Lista global de IPs únicos vistos até agora.
 * @param ip_count_global   Contagem correspondente a cada IP em ip_list_global.
 * @param ip_num_global     Número de IPs distintos registados até agora.
 */
static void acumular(WorkerResult *total, WorkerResult *r,
                     char ip_list_global[256][IP_LEN], long ip_count_global[256], int *ip_num_global) {
    /* Somar contadores básicos ao total acumulado */
    total->total_lines    += r->total_lines;
    total->count_debug    += r->count_debug;
    total->count_info     += r->count_info;
    total->count_warn     += r->count_warn;
    total->count_error    += r->count_error;
    total->count_critical += r->count_critical;
    total->count_4xx      += r->count_4xx;
    total->count_5xx      += r->count_5xx;

    /* Atualizar Top 10 IPs globalmente a partir do Top 10 de cada worker.
     * Cada worker envia apenas os seus 10 IPs mais frequentes; aqui
     * fundimos essa informação numa lista global única. */
    for (int k = 0; k < 10; k++) {
        /* Ignorar entradas vazias ou com contagem nula */
        if (r->top_ips[k][0] == '\0' || r->top_ips_counts[k] <= 0) continue;

        /* Verificar se este IP já existe na lista global */
        int found = -1;
        for (int i = 0; i < *ip_num_global; i++) {
            if (strcmp(ip_list_global[i], r->top_ips[k]) == 0) {
                found = i;
                break;
            }
        }

        if (found == -1 && *ip_num_global < 256) {
            /* IP novo: adicionar ao fim da lista global */
            strncpy(ip_list_global[*ip_num_global], r->top_ips[k], IP_LEN - 1);
            ip_list_global[*ip_num_global][IP_LEN - 1] = '\0';
            ip_count_global[*ip_num_global] = r->top_ips_counts[k];
            (*ip_num_global)++;
        } else if (found >= 0) {
            /* IP já conhecido: somar contagens */
            ip_count_global[found] += r->top_ips_counts[k];
        }
    }

    /* Copiar alertas críticos do worker para o total, respeitando o limite MAX_ALERTS */
    for (int i = 0; i < r->num_alerts && total->num_alerts < MAX_ALERTS; i++) {
        strncpy(total->alerts[total->num_alerts], r->alerts[i], ALERT_LEN - 1);
        total->alerts[total->num_alerts][ALERT_LEN - 1] = '\0';
        total->num_alerts++;
    }

    /* Ordenar a lista global de IPs por contagem decrescente (bubble sort)
     * para que possamos depois copiar os primeiros 10 facilmente */
    for (int i = 0; i < *ip_num_global - 1; i++) {
        for (int j = 0; j < *ip_num_global - i - 1; j++) {
            if (ip_count_global[j] < ip_count_global[j + 1]) {
                /* Trocar contagens */
                long tmp_count = ip_count_global[j];
                ip_count_global[j] = ip_count_global[j + 1];
                ip_count_global[j + 1] = tmp_count;

                /* Trocar IPs correspondentes */
                char tmp_ip[IP_LEN];
                strncpy(tmp_ip, ip_list_global[j], IP_LEN);
                strncpy(ip_list_global[j], ip_list_global[j + 1], IP_LEN);
                strncpy(ip_list_global[j + 1], tmp_ip, IP_LEN);
            }
        }
    }

    /* Actualizar os top 10 no struct total com os primeiros 10 da lista ordenada */
    memset(total->top_ips, 0, sizeof(total->top_ips));
    memset(total->top_ips_counts, 0, sizeof(total->top_ips_counts));
    int limite = *ip_num_global < 10 ? *ip_num_global : 10;
    for (int i = 0; i < limite; i++) {
        strncpy(total->top_ips[i], ip_list_global[i], IP_LEN - 1);
        total->top_ips[i][IP_LEN - 1] = '\0';
        total->top_ips_counts[i] = ip_count_global[i];
    }
}

/**
 * @brief Imprime o relatório final com as métricas agregadas de todos os workers.
 *
 * @param total  Estrutura com os totais acumulados de todos os workers.
 * @param modo   String com o modo de análise utilizado (ex.: "security", "full").
 */
static void imprimir_relatorio(WorkerResult *total, char *modo) {
    printf("\n=== RELATORIO FINAL (%s) ===\n", modo);
    printf("Total de linhas  : %ld\n", total->total_lines);
    printf("DEBUG            : %ld\n", total->count_debug);
    printf("INFO             : %ld\n", total->count_info);
    printf("WARNINGS         : %ld\n", total->count_warn);
    printf("ERRORS           : %ld\n", total->count_error);
    printf("CRITICAL         : %ld\n", total->count_critical);
    printf("HTTP 4xx         : %ld\n", total->count_4xx);
    printf("HTTP 5xx         : %ld\n", total->count_5xx);
    printf("\n--- TOP 10 IPs ---\n");
    for (int i = 0; i < 10; i++) {
        if (total->top_ips[i][0] == '\0' || total->top_ips_counts[i] <= 0) break;
        printf("%2d. %s (%ld pedidos)\n", i + 1, total->top_ips[i], total->top_ips_counts[i]);
    }

    printf("\n--- ALERTAS CRITICOS ---\n");
    if (total->num_alerts == 0) {
        printf("Sem alertas criticos.\n");
    } else {
        for (int i = 0; i < total->num_alerts; i++) {
            printf("%2d. %s\n", i + 1, total->alerts[i]);
        }
    }
    printf("=================================\n");
}

/* ═════════════════════════════════════════════════════════════════════════════
 * MAIN
 * ════════════════════════════════════════════════════════════════════════════= */

/**
 * @brief Ponto de entrada do programa — processo pai/servidor.
 *
 * @details
 * Orquestra todo o ciclo de vida: descoberta de ficheiros, criação do socket
 * servidor, criação dos processos filho, distribuição de configuração, recepção
 * de resultados e impressão do relatório final.
 *
 * @param argc  Número de argumentos da linha de comandos.
 * @param argv  Array de strings dos argumentos:
 *              `argv[1]` = directório com logs,
 *              `argv[2]` = número de processos worker,
 *              `argv[3]` = modo de análise,
 *              `argv[4]` = (opcional) `--verbose`.
 * @return 0 em sucesso; 1 em caso de erro.
 */
int main(int argc, char *argv[]) {

    if (argc < 4) {
        printf("Uso: %s <directorio> <num_processos> <modo> [--verbose]\n", argv[0]);
        exit(1);
    }

    char *dir          = argv[1];
    int   num_procs    = atoi(argv[2]);
    char *modo         = argv[3];
    int   verbose      = (argc > 4 && strcmp(argv[4], "--verbose") == 0);

    /* Configurar o modo do parser (security / traffic / full / ...) */
    if (parser_set_mode_from_string(modo) != 0) {
        fprintf(stderr, "Modo invalido: %s\n", modo);
        exit(1);
    }

    /* ── 1. Descobrir os ficheiros de log ── */
    int total_ficheiros = 0;
    /* ler_directorio() usa opendir/readdir/closedir para listar o directório
     * e devolve um array dinâmico com os caminhos completos */
    char **ficheiros = ler_directorio(dir, &total_ficheiros);

    if (total_ficheiros == 0) {
        printf("Nenhum ficheiro .log ou .json encontrado em: %s\n", dir);
        free(ficheiros);
        exit(0);
    }

    /* Não faz sentido ter mais processos do que ficheiros */
    if (num_procs > total_ficheiros)
        num_procs = total_ficheiros;

    printf("Ficheiros encontrados: %d | Workers: %d | Modo: %s\n\n",
           total_ficheiros, num_procs, modo);

    /* ── 1.5 Obter dimensão total via stat() — sem ler os ficheiros ── */
    printf("A calcular dimensao total...\n");
    /* stat() consulta apenas os metadados do inode — não lê conteúdo,
     * pelo que é instantâneo independentemente do tamanho dos ficheiros */
    off_t total_bytes = obter_bytes_totais(ficheiros, total_ficheiros);
    printf("Total de bytes encontrados: %lld\n\n", (long long)total_bytes);

    /* Calcular configuração de cada worker antes do fork.
     * Cada worker recebe um intervalo [byte_inicio, byte_fim) proporcional. */
    WorkerConfig *configs = malloc(num_procs * sizeof(WorkerConfig));
    if (!configs) { perror("malloc"); exit(1); }

    off_t bytes_por_worker = total_bytes / num_procs;
    for (int i = 0; i < num_procs; i++) {
        configs[i].worker_index        = i;
        configs[i].byte_inicio         = (off_t)i * bytes_por_worker;
        /* O último worker fica com o resto para não perder bytes por arredondamento */
        configs[i].byte_fim            = (i == num_procs - 1) ? total_bytes
                                         : configs[i].byte_inicio + bytes_por_worker;
        configs[i].total_bytes_globais = total_bytes;
    }

    /* ── 2. Criar o socket servidor (antes do fork, para os filhos herdarem o path) ── */

    /* Remover socket antigo se existir — unlink() apaga o ficheiro especial
     * de socket criado por um bind() anterior; necessário para evitar EADDRINUSE */
    unlink(SOCKET_PATH);

    /* socket() cria um endpoint de comunicação e devolve um file descriptor.
     *   AF_UNIX      → domínio Unix (comunicação local, sem rede)
     *   SOCK_STREAM  → fiável, orientado à ligação (como TCP, mas local)
     *   0            → protocolo por defeito para este tipo de socket
     * AF_UNIX é preferível a AF_INET para IPC local porque é mais rápido
     * (sem overhead TCP/IP) e não expõe o canal à rede. */
    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); exit(1); }

    /* Preencher a estrutura de endereço Unix Domain Socket.
     * sun_family identifica a família de endereços; sun_path é o caminho
     * do ficheiro especial que representará o socket no sistema de ficheiros. */
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    /* bind() associa o socket ao endereço (cria o ficheiro especial SOCKET_PATH).
     * É o equivalente a "reservar" um endereço de escuta para o servidor. */
    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        exit(1);
    }

    /* listen() marca o socket como passivo (pronto a aceitar ligações) e define
     * o tamanho da fila de ligações pendentes (backlog = 64).
     * Sem listen(), o socket não aceita ligações entrantes. */
    if (listen(server_fd, 64) < 0) {
        perror("listen");
        exit(1);
    }

    pid_t *pids = malloc(num_procs * sizeof(pid_t));

    /* fflush(NULL) limpa TODOS os buffers de stdio antes do fork().
     * Sem isto, o conteúdo em buffer poderia ser duplicado: tanto o
     * processo pai como o filho teriam a mesma cópia no buffer e
     * escreveriam ao mesmo tempo para stdout. */
    fflush(NULL);

    /* Registar o instante de início para calcular o tempo total no final */
    time_t t_inicio = time(NULL);

    /* ── 3. Criar os filhos com fork() ── */
    for (int i = 0; i < num_procs; i++) {
        /* fork() cria uma cópia exacta do processo corrente.
         * Retorna 0 no processo filho e o PID do filho no processo pai.
         * Ambos continuam a executar a partir da instrução seguinte. */
        pid_t pid = fork();
        if (pid < 0) { perror("fork"); exit(1); }

        if (pid == 0) {
            /* --- Código que corre APENAS no processo FILHO --- */

            /* O filho não precisa de aceitar ligações, por isso fecha
             * o server_fd que herdou do pai via fork().
             * Não fechar file descriptors desnecessários desperdiça
             * entradas na tabela de FDs do processo. */
            if (close(server_fd) == -1) {
                perror("close");
                exit(1);
            }

            // O PAI AGORA PASSA TODOS OS FICHEIROS PARA TODOS OS FILHOS!
            // A divisão vai ser feita por linhas lá dentro.
            /* run_worker() liga-se ao socket do pai via connect(),
             * recebe a sua WorkerConfig e processa a fatia de bytes atribuída */
            run_worker(ficheiros, total_ficheiros, num_procs, i, verbose);
            exit(0);
        }

        /* --- Código que corre APENAS no processo PAI --- */
        /* Guardar o PID do filho para poder fazer waitpid() mais tarde */
        pids[i] = pid;
    }

    /* ── 4. Aceitar as ligações dos filhos (uma por filho, usando select para não bloquear) ── */
    int *client_fds = malloc(num_procs * sizeof(int));
    /* Inicializar todos os slots com -1 (indica "slot vazio") */
    for (int i = 0; i < num_procs; i++) client_fds[i] = -1;

    int num_conectados = 0;

    /* Aguardar até que todos os N filhos se tenham ligado ao servidor */
    while (num_conectados < num_procs) {
        /* Preparar o conjunto de FDs para select() — apenas server_fd nesta fase */
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(server_fd, &readfds);

        /* Timeout de 5 segundos: se nenhum filho ligar, imprime aviso e
         * volta ao início do while em vez de bloquear para sempre */
        struct timeval tv;
        tv.tv_sec = 5;
        tv.tv_usec = 0;

        /* select() bloqueia até haver actividade em qualquer FD do conjunto
         * ou até expirar o timeout.  Devolve o número de FDs prontos, 0 se
         * expirou, ou -1 em erro.
         * Usar select() aqui em vez de accept() a seco evita bloqueio
         * permanente caso um filho falhe antes de se ligar. */
        int activity = select(server_fd + 1, &readfds, NULL, NULL, &tv);
        if (activity < 0) { perror("select"); exit(1); }

        if (activity == 0) {
            /* Timeout expirado — nenhum filho ligou neste intervalo */
            fprintf(stderr, "Timeout: aguardando conexão de worker\n");
            continue;
        }

        if (FD_ISSET(server_fd, &readfds)) {
            /* Há uma ligação pendente — accept() cria um novo socket dedicado
             * à comunicação com este cliente (filho).  O server_fd continua
             * disponível para aceitar mais ligações. */
            int client_fd = accept(server_fd, NULL, NULL);
            if (client_fd < 0) { perror("accept"); exit(1); }

            /* Encontrar um slot vazio para este worker.
             * Nota: não é por ordem, pode ser qualquer um.
             * Os filhos podem ligar-se ao pai em qualquer sequência,
             * dependendo do escalonamento do kernel. */
            int idx = -1;
            for (int i = 0; i < num_procs; i++) {
                if (client_fds[i] == -1) {
                    idx = i;
                    break;
                }
            }

            if (idx >= 0) {
                client_fds[idx] = client_fd;

                /* Enviar configuração ao worker via socket.
                 * Protocolo: primeiro um int com o tipo MSG_CONFIG,
                 * depois a estrutura WorkerConfig com o intervalo de bytes.
                 * O worker do outro lado faz read() exactamente nestas
                 * mesmas quantidades e nesta ordem. */
                int tipo = MSG_CONFIG;
                /* write() escreve o tipo da mensagem (int) — o worker usa
                 * o tipo para saber o que se segue na stream */
                write(client_fds[idx], &tipo, sizeof(tipo));
                /* write() envia a configuração completa (byte_inicio, byte_fim, etc.) */
                write(client_fds[idx], &configs[idx], sizeof(WorkerConfig));

                num_conectados++;
                if (verbose)
                    printf("Worker %d conectado (total: %d/%d)\n", idx, num_conectados, num_procs);
            }
        }
    }

    /* ── 5. Reservar espaço no terminal para o dashboard ── */
    /* calloc() inicializa a memória a zero — garante que bytes_done e
     * bytes_total começam a 0, evitando lixo na primeira renderização */
    ProgressUpdate *progressos = calloc(num_procs, sizeof(ProgressUpdate));
    for (int i = 0; i < num_procs; i++) {
        progressos[i].worker_index = i;
        /* Imprimir uma linha "placeholder" por worker para reservar espaço
         * no terminal — o dashboard irá sobrescrever estas linhas */
        printf("Worker %2d [....................] -- Aguardar...\n", i);
    }
    fflush(stdout);

    /* ── 6. Loop principal: receber mensagens dos filhos (com select, sem busy-waiting) ── */
    char ip_list_global[256][IP_LEN];
    long ip_count_global[256] = {0};
    int ip_num_global = 0;

    WorkerResult total = {0};
    int resultados = 0;  /* número de workers que já enviaram o resultado final */

    /* Continuar até receber MSG_RESULTADO de todos os workers */
    while (resultados < num_procs) {
        fd_set readfds;
        FD_ZERO(&readfds);

        /* Adicionar todos os client_fds activos ao conjunto de leitura.
         * select() irá notificar-nos quando qualquer um deles tiver dados. */
        int max_fd = -1;
        for (int i = 0; i < num_procs; i++) {
            if (client_fds[i] != -1) {
                FD_SET(client_fds[i], &readfds);
                /* select() precisa do valor max_fd+1 como primeiro argumento;
                 * é o índice do FD mais alto + 1 */
                if (client_fds[i] > max_fd) max_fd = client_fds[i];
            }
        }

        /* Esperar por actividade com timeout de 1 segundo.
         * O timeout evita bloqueio eterno caso um worker tenha falhado
         * silenciosamente; o loop volta ao início e reavalia o estado. */
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        /* select() monitoriza simultaneamente todos os sockets dos workers.
         * É a alternativa eficiente a fazer read() bloqueante num único socket
         * — permite que o pai reaja à mensagem que chegar primeiro, seja
         * de que worker for. */
        int activity = select(max_fd + 1, &readfds, NULL, NULL, &tv);
        if (activity < 0) { perror("select"); exit(1); }

        if (activity == 0) continue; /* Timeout, tenta novamente */

        /* Verificar qual(ais) socket(s) têm dados disponíveis */
        for (int i = 0; i < num_procs; i++) {
            /* Ignorar workers já terminados (fd = -1) ou sem dados prontos */
            if (client_fds[i] == -1 || !FD_ISSET(client_fds[i], &readfds))
                continue;

            /* Ler o tipo da mensagem — um único int que identifica o que se segue.
             * Este padrão (type-length-value ou apenas type-value) é um protocolo
             * binário simples que evita parsear texto e minimiza o número de
             * syscalls read(). */
            int tipo;
            ssize_t lidos = read(client_fds[i], &tipo, sizeof(tipo));
            if (lidos <= 0) {
                /* Filho fechou o socket inesperadamente (lidos == 0 → EOF,
                 * lidos < 0 → erro).  Tratar como se tivesse terminado. */
                fprintf(stderr, "Worker %d: ligação fechada inesperadamente\n", i);
                close(client_fds[i]);
                client_fds[i] = -1;
                resultados++;
                continue;
            }

            if (tipo == MSG_PROGRESSO) {
                /* MSG_PROGRESSO: o worker informa quantos bytes já processou.
                 * read() lê a estrutura ProgressUpdate que vem imediatamente
                 * a seguir ao int do tipo, conforme o protocolo. */
                ProgressUpdate pu;
                read(client_fds[i], &pu, sizeof(pu));
                /* Actualizar o estado deste worker no array de progresso */
                progressos[pu.worker_index] = pu;
                /* Redesenhar o dashboard in-place no terminal */
                desenhar_dashboard(progressos, num_procs);

            } else if (tipo == MSG_RESULTADO) {
                /* MSG_RESULTADO: o worker terminou e envia as métricas finais.
                 * read() lê a estrutura WorkerResult com todos os contadores
                 * e o Top 10 de IPs deste worker. */
                WorkerResult r;
                read(client_fds[i], &r, sizeof(r));
                /* Fundir as métricas deste worker no total global */
                acumular(&total, &r, (char (*)[IP_LEN])ip_list_global, ip_count_global, &ip_num_global);

                /* Marcar este worker como 100% terminado no dashboard */
                progressos[i].bytes_done = progressos[i].bytes_total;
                desenhar_dashboard(progressos, num_procs);

                /* Fechar o socket deste worker — a comunicação terminou */
                close(client_fds[i]);
                client_fds[i] = -1;
                resultados++;
            }
        }
    }

    /* ── 7. Fechar o socket servidor e remover o ficheiro do socket ── */
    /* close() liberta o file descriptor do socket servidor no processo pai */
    close(server_fd);
    /* unlink() remove o ficheiro especial de socket do sistema de ficheiros.
     * Sem este passo, o ficheiro ficaria em /tmp e impediria futuras
     * execuções de fazerem bind() no mesmo caminho (EADDRINUSE). */
    unlink(SOCKET_PATH);

    /* ── 8. Esperar que todos os filhos terminem (recolher zombies) ── */
    for (int i = 0; i < num_procs; i++) {
        int status;
        /* waitpid() suspende o pai até o filho com PID pids[i] terminar,
         * recolhendo o seu estado de saída.  Sem waitpid(), os filhos
         * terminados ficariam em estado zombie (entrada na tabela de
         * processos sem recursos, mas não removidos). */
        waitpid(pids[i], &status, 0);
        /* WIFEXITED verifica se o filho terminou normalmente (via exit/return).
         * WEXITSTATUS extrai o código de saída passado a exit(). */
        if (WIFEXITED(status) && WEXITSTATUS(status) != 0)
            fprintf(stderr, "Worker %d terminou com erro %d\n", i, WEXITSTATUS(status));
    }

    /* ── 9. Relatório final ── */
    long elapsed = (long)(time(NULL) - t_inicio);

    imprimir_relatorio(&total, modo);
    printf("Tempo de processamento: %ldmin %02lds\n", elapsed / 60, elapsed % 60);

    /* Libertar toda a memória alocada dinamicamente */
    free(progressos);
    free(client_fds);
    free(pids);
    free(configs);
    libertar_ficheiros(ficheiros, total_ficheiros);

    return 0;
}
