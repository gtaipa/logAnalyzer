/**
 * @file main_pipes.c
 * @brief Processo PAI do sistema de análise de logs paralelo (versão com pipes anónimos).
 *
 * @details
 * Este ficheiro implementa o processo principal (pai) que coordena toda a análise
 * paralela de ficheiros de log. A arquitectura segue o padrão clássico de
 * Sistemas Operativos "mestre–trabalhador" (master–worker):
 *
 *  1. O pai lê o directório de logs e calcula o tamanho total em bytes.
 *  2. Divide esse espaço de bytes em N fatias contíguas, uma por worker.
 *  3. Cria N pipes anónimos (pipe(2)) — um por filho — para IPC unidireccional
 *     filho → pai.
 *  4. Faz fork(2) N vezes; cada filho fecha a ponta de leitura do seu pipe,
 *     executa run_worker_pipe() e escreve mensagens tipadas no pipe.
 *  5. O pai fecha as pontas de escrita e entra num ciclo select(2) multiplexado
 *     que lê, sem bloquear num único filho, mensagens de dois tipos:
 *       - MSG_PROGRESSO : actualiza o dashboard no terminal.
 *       - MSG_RESULTADO : acumula as métricas globais e fecha o pipe.
 *  6. Após todos os filhos terminarem, waitpid(2) recolhe os estados de saída
 *     e o relatório final é impresso.
 *
 * Conceitos de SO demonstrados:
 *  - fork/exec pattern (só fork, sem exec — o código do filho está na mesma imagem)
 *  - Pipes anónimos como canal de IPC (inter-process communication)
 *  - select(2) para I/O multiplexado não bloqueante sobre múltiplos descritores
 *  - waitpid(2) para recolha de processos zombie e obtenção do exit status
 *  - Partilha de memória via herança de descritores após fork
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
#include <time.h>
#include <unistd.h>

#include "ipc.h"
#include "parser.h"
#include "posix_io.h"
#include "worker.h"

/* Tipo de mensagem enviada pelo filho: actualização de progresso */
#define MSG_PROGRESSO 1
/* Tipo de mensagem enviada pelo filho: resultado final das métricas */
#define MSG_RESULTADO 2
/* Largura visual (em caracteres '#') da barra de progresso no dashboard */
#define LARGURA_BARRA 20
/* Tamanho do buffer genérico de leitura (em bytes) */
#define BUF_SIZE 4096

/* Declaração antecipada da função de worker (implementada em worker_pipes.c) */
void run_worker_pipe(char **ficheiros, int total_ficheiros, int pipe_fd_write,
                     int worker_index, off_t byte_inicio, off_t byte_fim, int verbose);

/* ─────────────────────────────────────────────────────────────────────────────
 * Funções auxiliares estáticas (internas a este ficheiro)
 * ───────────────────────────────────────────────────────────────────────────── */

/**
 * @brief Redesenha o dashboard de progresso de todos os workers no terminal.
 *
 * @details
 * Usa sequências de escape ANSI para subir N linhas (\033[NA) e apagar até
 * ao fim do ecrã (\033[J), sobrepondo a informação actualizada sem fazer scroll.
 * É chamada sempre que chega uma mensagem MSG_PROGRESSO ou MSG_RESULTADO.
 *
 * @param progressos  Array com o último ProgressUpdate recebido de cada worker.
 * @param num_workers Número total de workers (e de linhas no dashboard).
 */
static void desenhar_dashboard(ProgressUpdate *progressos, int num_workers) {
    /* Sobe num_workers linhas no terminal para reescrever por cima */
    printf("\033[%dA", num_workers);
    /* Apaga tudo desde o cursor até ao fim do ecrã */
    printf("\033[J");

    for (int i = 0; i < num_workers; i++) {
        long feitas = progressos[i].bytes_done;
        long total  = progressos[i].bytes_total;

        /* Calcula percentagem; protege divisão por zero quando total==0 */
        int pct = (total > 0) ? (int)(feitas * 100 / total) : 0;
        if (pct > 100) pct = 100;

        /* Constrói string da barra: '#' para a fracção completa, '.' para o resto */
        char barra[LARGURA_BARRA + 1];
        int cheio = pct * LARGURA_BARRA / 100;
        for (int b = 0; b < LARGURA_BARRA; b++)
            barra[b] = (b < cheio) ? '#' : '.';
        barra[LARGURA_BARRA] = '\0'; /* terminador obrigatório */

        printf("Worker %2d [%s] %3d%% (%ld/%ld bytes)\n",
               i, barra, pct, feitas, total);
    }
    fflush(stdout); /* força a escrita imediata no terminal (stdout é normalmente com buffer de linha) */
}

/**
 * @brief Liberta a memória alocada dinamicamente para a lista de caminhos de ficheiros.
 *
 * @details
 * Cada entrada do array foi criada com strdup() (que internamente chama malloc()),
 * pelo que cada string deve ser libertada individualmente antes de libertar o array.
 *
 * @param ficheiros       Array de ponteiros para strings com os caminhos.
 * @param total_ficheiros Número de entradas válidas no array.
 */
static void libertar_ficheiros(char **ficheiros, int total_ficheiros) {
    if (ficheiros == NULL) return;
    for (int i = 0; i < total_ficheiros; i++) free(ficheiros[i]); /* liberta cada string */
    free(ficheiros); /* liberta o array de ponteiros */
}

/**
 * @brief Soma o tamanho em bytes de todos os ficheiros da lista.
 *
 * @details
 * Usa a chamada ao sistema stat(2) para obter os metadados de cada ficheiro
 * (nomeadamente st_size) sem precisar de os abrir.  Esta informação é usada
 * para calcular as fatias de bytes atribuídas a cada worker.
 *
 * @param ficheiros       Array de caminhos absolutos para os ficheiros de log.
 * @param total_ficheiros Número de ficheiros no array.
 * @return Tamanho acumulado em bytes (tipo off_t, que suporta ficheiros > 2 GB).
 */
static off_t obter_bytes_totais(char **ficheiros, int total_ficheiros) {
    off_t total = 0;
    struct stat st; /* estrutura preenchida por stat(2) com metadados do ficheiro */
    for (int i = 0; i < total_ficheiros; i++) {
        /* stat(2): syscall POSIX que preenche 'st' com info do ficheiro sem o abrir */
        if (stat(ficheiros[i], &st) == 0)
            total += st.st_size; /* st_size: tamanho do ficheiro em bytes */
    }
    return total;
}

/**
 * @brief Converte a string do argumento de linha de comando no número de processos.
 *
 * @details
 * Usa strtol() em vez de atoi() para detectar erros de conversão (valor não
 * numérico, overflow, valor negativo ou zero).  Em caso de erro termina o
 * processo imediatamente, pois sem um número válido de workers não há execução.
 *
 * @param texto String ASCII recebida de argv[].
 * @return Número inteiro positivo de processos a criar.
 */
static int converter_num_processos(const char *texto) {
    char *fim = NULL; /* apontará para o primeiro caracter não convertido */
    errno = 0;        /* limpa errno antes da chamada para detectar overflow */
    long valor = strtol(texto, &fim, 10); /* converte base 10 */
    /* Validação: errno != 0 → overflow; fim==texto → sem dígitos; *fim!='\0' → lixo no fim; valor<=0 → inválido */
    if (errno != 0 || fim == texto || *fim != '\0' || valor <= 0) {
        posix_writef(STDERR_FILENO, "Numero de processos invalido: %s\n", texto);
        exit(1);
    }
    return (int)valor;
}

/**
 * @brief Acumula as métricas de um worker nos totais globais e actualiza o top-10 de IPs.
 *
 * @details
 * Esta função é chamada pelo pai sempre que recebe um MSG_RESULTADO de um filho.
 * Realiza três operações principais:
 *  1. Soma contadores simples (linhas, níveis de log, códigos HTTP).
 *  2. Faz merge da tabela de IPs do worker com a tabela global, acumulando contagens.
 *  3. Ordena a tabela global por contagem decrescente (bubble sort) e actualiza
 *     o top-10 armazenado em WorkerResult.
 *
 * Nota de SO: toda esta lógica corre no processo pai após leitura do pipe —
 * não há memória partilhada entre pai e filhos; os dados chegam serializados
 * pela pipe e são deserializados aqui.
 *
 * @param total           Acumulador global de resultados (actualizado in-place).
 * @param r               Resultado parcial recebido do worker via pipe.
 * @param ip_list_global  Tabela global de strings de IPs (máx. 256 entradas).
 * @param ip_count_global Contagens correspondentes a ip_list_global.
 * @param ip_num_global   Ponteiro para o número actual de IPs na tabela global.
 */
static void acumular_resultado(WorkerResult *total, const WorkerResult *r,
                               char ip_list_global[256][IP_LEN], long ip_count_global[256], int *ip_num_global) {
    /* 1. Somar contadores simples */
    total->total_lines += r->total_lines;
    total->count_debug += r->count_debug;
    total->count_info  += r->count_info;
    total->count_warn  += r->count_warn;
    total->count_error += r->count_error;
    total->count_critical += r->count_critical;
    total->count_4xx += r->count_4xx;
    total->count_5xx += r->count_5xx;

    /* 2. Merge dos top-10 IPs do worker na tabela global */
    for (int k = 0; k < 10; k++) {
        /* Ignora entradas vazias ou com contagem nula */
        if (r->top_ips[k][0] == '\0' || r->top_ips_counts[k] <= 0) continue;

        /* Pesquisa linear: verifica se este IP já existe na tabela global */
        int found = -1;
        for (int i = 0; i < *ip_num_global; i++) {
            if (strcmp(ip_list_global[i], r->top_ips[k]) == 0) {
                found = i;
                break;
            }
        }
        if (found == -1 && *ip_num_global < 256) {
            /* IP novo: adiciona à tabela global */
            strncpy(ip_list_global[*ip_num_global], r->top_ips[k], IP_LEN - 1);
            ip_list_global[*ip_num_global][IP_LEN - 1] = '\0'; /* garante terminação segura */
            ip_count_global[*ip_num_global] = r->top_ips_counts[k];
            (*ip_num_global)++;
        } else if (found >= 0) {
            /* IP já existente: acumula a contagem */
            ip_count_global[found] += r->top_ips_counts[k];
        }
    }

    /* 3. Copia alertas do worker para o total, até ao limite MAX_ALERTS */
    for (int i = 0; i < r->num_alerts && total->num_alerts < MAX_ALERTS; i++) {
        strncpy(total->alerts[total->num_alerts], r->alerts[i], ALERT_LEN - 1);
        total->alerts[total->num_alerts][ALERT_LEN - 1] = '\0';
        total->num_alerts++;
    }

    /* 4. Bubble sort descendente sobre a tabela global de IPs
     *    (O(n²) aceitável aqui porque n ≤ 256 e corre apenas no pai) */
    for (int i = 0; i < *ip_num_global - 1; i++) {
        for (int j = 0; j < *ip_num_global - i - 1; j++) {
            if (ip_count_global[j] < ip_count_global[j + 1]) {
                /* Troca contagens */
                long tmp_count = ip_count_global[j];
                ip_count_global[j] = ip_count_global[j + 1];
                ip_count_global[j + 1] = tmp_count;

                /* Troca strings de IP correspondentes */
                char tmp_ip[IP_LEN];
                strncpy(tmp_ip, ip_list_global[j], IP_LEN);
                strncpy(ip_list_global[j], ip_list_global[j + 1], IP_LEN);
                strncpy(ip_list_global[j + 1], tmp_ip, IP_LEN);
            }
        }
    }

    /* 5. Actualiza o top-10 no acumulador global com os primeiros 10 da tabela ordenada */
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
 * @brief Imprime o relatório final agregado de todas as métricas.
 *
 * @param total Estrutura com os resultados acumulados de todos os workers.
 * @param modo  String que identifica o modo de parsing usado (ex: "nginx", "syslog").
 */
static void imprimir_relatorio(const WorkerResult *total, char *modo) {
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

/* ─────────────────────────────────────────────────────────────────────────────
 * main — ponto de entrada do processo pai
 * ───────────────────────────────────────────────────────────────────────────── */

/**
 * @brief Ponto de entrada do programa. Orquestra toda a análise paralela via pipes.
 *
 * @details
 * Fluxo de execução resumido:
 *  1. Valida argumentos e configura o parser de logs.
 *  2. Enumera ficheiros .log e .json no directório indicado.
 *  3. Calcula o tamanho total e divide-o em N fatias (uma por worker).
 *  4. Para cada worker: cria um pipe anónimo + chama fork().
 *     - Filho: fecha ponta de leitura, chama run_worker_pipe(), termina.
 *     - Pai:   fecha ponta de escrita, guarda ponta de leitura.
 *  5. Entra no ciclo select(2) para ler mensagens de todos os pipes sem bloquear.
 *  6. Após receber MSG_RESULTADO de todos os filhos, chama waitpid(2) para cada um.
 *  7. Imprime o relatório final e liberta todos os recursos.
 *
 * @param argc Número de argumentos de linha de comando.
 * @param argv argv[1]=diretório, argv[2]=num_processos, argv[3]=modo, [argv[4]=--verbose]
 * @return 0 em caso de sucesso; 1 em caso de erro.
 */
int main(int argc, char *argv[]) {
    /* Verifica que foram fornecidos pelo menos os 3 argumentos obrigatórios */
    if (argc < 4) {
        posix_writef(STDOUT_FILENO, "Uso: %s <diretorio> <num_processos> <modo> [--verbose]\n", argv[0]);
        exit(1);
    }

    char *diretorio     = argv[1];
    int   num_processos = converter_num_processos(argv[2]);
    char *modo          = argv[3];

    /* Detecta flag opcional --verbose para logs detalhados nos workers */
    int verbose = 0;
    for (int i = 4; i < argc; i++) {
        if (strcmp(argv[i], "--verbose") == 0) verbose = 1;
    }

    /* Configura o parser de logs de acordo com o modo pedido (nginx, syslog, etc.) */
    if (parser_set_mode_from_string(modo) != 0) {
        posix_writef(STDERR_FILENO, "Modo invalido: %s\n", modo);
        exit(1);
    }

    /* ── Fase 1: Enumeração dos ficheiros de log no directório ── */

    int   capacidade      = 10; /* capacidade inicial do array dinâmico */
    int   total_ficheiros = 0;
    char **ficheiros      = malloc((size_t)capacidade * sizeof(char *));

    /* opendir(3): abre o directório para iteração via readdir(3) */
    DIR *dir = opendir(diretorio);
    if (dir == NULL) { perror("opendir"); exit(1); }

    struct dirent *entrada;
    /* readdir(3): retorna a próxima entrada do directório; NULL no fim ou em erro */
    while ((entrada = readdir(dir)) != NULL) {
        char *nome = entrada->d_name;
        size_t len = strlen(nome);

        /* Filtra apenas ficheiros com extensão .log ou .json */
        if (!((len > 4 && strcmp(nome + len - 4, ".log") == 0) ||
              (len > 5 && strcmp(nome + len - 5, ".json") == 0))) {
            continue;
        }

        /* Duplica a capacidade do array se necessário (estratégia de crescimento exponencial) */
        if (total_ficheiros == capacidade) {
            capacidade *= 2;
            ficheiros = realloc(ficheiros, (size_t)capacidade * sizeof(char *));
        }

        /* Constrói o caminho absoluto e guarda uma cópia independente com strdup */
        char caminho[512];
        snprintf(caminho, sizeof(caminho), "%s/%s", diretorio, nome);
        ficheiros[total_ficheiros++] = strdup(caminho); /* strdup aloca e copia a string */
    }
    closedir(dir); /* fecha o descritor de directório */

    if (total_ficheiros == 0) {
        posix_writef(STDOUT_FILENO, "Nenhum ficheiro encontrado.\n");
        libertar_ficheiros(ficheiros, total_ficheiros);
        exit(0);
    }

    /* ── Fase 2: Cálculo do espaço de bytes e divisão em fatias ── */

    posix_writef(STDOUT_FILENO, "A calcular dimensao total...\n");
    off_t total_bytes = obter_bytes_totais(ficheiros, total_ficheiros);
    posix_writef(STDOUT_FILENO, "Total de bytes encontrados: %lld\n\n", (long long)total_bytes);

    /* Divisão inteira; o último worker absorve os bytes residuais */
    off_t bytes_por_worker = total_bytes / num_processos;

    /* Array de configurações: cada entrada define a fatia [byte_inicio, byte_fim) do worker */
    WorkerConfig *configs = malloc((size_t)num_processos * sizeof(WorkerConfig));
    for (int i = 0; i < num_processos; i++) {
        configs[i].worker_index        = i;
        configs[i].byte_inicio         = (off_t)i * bytes_por_worker;
        /* O último worker vai até ao fim para cobrir o resto da divisão inteira */
        configs[i].byte_fim            = (i == num_processos - 1) ? total_bytes
                                         : configs[i].byte_inicio + bytes_por_worker;
        configs[i].total_bytes_globais = total_bytes;
    }

    /* ── Fase 3: Criação dos pipes e processos filhos (fork) ── */

    pid_t *pids          = malloc((size_t)num_processos * sizeof(pid_t));
    int   *pipes_leitura = malloc((size_t)num_processos * sizeof(int));
    for (int i = 0; i < num_processos; i++) pipes_leitura[i] = -1; /* -1 indica pipe ainda não criado */

    /* fflush(NULL): esvazia todos os buffers de stdio antes do fork para evitar
     * que dados pendentes sejam escritos duas vezes (pelo pai e pelo filho) */
    fflush(NULL);

    time_t t_inicio = time(NULL); /* marca o início para calcular o tempo total */

    for (int i = 0; i < num_processos; i++) {
        int fd[2]; /* fd[0] = ponta de leitura; fd[1] = ponta de escrita */

        /* pipe(2): cria um canal de comunicação unidireccional no kernel.
         * Os dados escritos em fd[1] ficam num buffer circular do kernel
         * até serem lidos em fd[0]. */
        if (pipe(fd) == -1) { perror("pipe"); exit(1); }

        /* fork(2): duplica o processo actual.
         * No filho: retorna 0.
         * No pai:   retorna o PID do filho (> 0).
         * Em erro:  retorna -1. */
        pid_t pid = fork();
        if (pid < 0) { perror("fork"); exit(1); }

        if (pid == 0) {
            /* ── Código do FILHO ── */

            /* O filho só escreve no pipe; fecha a ponta de leitura (fd[0])
             * para não manter descritores desnecessários abertos.
             * Boas práticas de SO: fechar todas as pontas não usadas. */
            close(fd[0]);

            /* Fecha também as pontas de leitura dos pipes dos irmãos já criados
             * (herdadas por herança de descritores via fork).
             * Se não forem fechadas, o pai nunca verá EOF neles quando esse filho terminar. */
            for (int j = 0; j < i; j++) {
                if (pipes_leitura[j] != -1) close(pipes_leitura[j]);
            }

            /* Executa a lógica do worker: processa a fatia de bytes atribuída
             * e envia mensagens (progresso + resultado) pelo pipe fd[1]. */
            run_worker_pipe(ficheiros, total_ficheiros, fd[1], i,
                            configs[i].byte_inicio, configs[i].byte_fim, verbose);
            exit(0); /* o filho termina aqui; run_worker_pipe faz exit() internamente */
        }

        /* ── Código do PAI (continua após fork) ── */

        /* O pai só lê do pipe; fecha a ponta de escrita (fd[1]).
         * IMPORTANTE: se o pai mantiver fd[1] aberto, nunca receberá EOF
         * quando o filho fechar o seu fd[1], bloqueando o select() indefinidamente. */
        close(fd[1]);
        pids[i]          = pid;         /* guarda o PID para waitpid posterior */
        pipes_leitura[i] = fd[0];       /* guarda a ponta de leitura para o select loop */
    }

    /* ── Fase 4: Inicialização do dashboard no terminal ── */

    /* Preparar o dashboard no terminal */
    ProgressUpdate *progressos = calloc(num_processos, sizeof(ProgressUpdate)); /* calloc inicializa a zero */
    for (int i = 0; i < num_processos; i++) {
        progressos[i].worker_index = i;
        /* Imprime uma linha de marcador para cada worker; serão sobrepostas depois */
        printf("Worker %2d [....................] -- Aguardar...\n", i);
    }
    fflush(stdout); /* garante que as linhas iniciais aparecem antes do primeiro update */

    /* Estrutura de resultados globais, inicializada a zero */
    WorkerResult total = {0};
    int resultados = 0; /* conta quantos workers já enviaram MSG_RESULTADO */

    /* Tabela global de IPs: acumulada à medida que chegam resultados dos workers */
    char ip_list_global[256][IP_LEN];
    long ip_count_global[256] = {0};
    int  ip_num_global        = 0;

    /* ── Fase 5: Select loop — PAI como servidor assíncrono sobre os pipes ── */

    /* Select loop (Pai servidor assíncrono para os Pipes) */
    while (resultados < num_processos) {
        /* fd_set: conjunto de descritores de ficheiro monitorados pelo select */
        fd_set readfds;
        FD_ZERO(&readfds);  /* limpa todos os bits do conjunto */
        int max_fd = -1;    /* select precisa de saber o maior fd + 1 */

        /* Adiciona ao conjunto todos os pipes ainda abertos */
        for (int i = 0; i < num_processos; i++) {
            if (pipes_leitura[i] != -1) {
                FD_SET(pipes_leitura[i], &readfds); /* activa o bit deste fd no conjunto */
                if (pipes_leitura[i] > max_fd) max_fd = pipes_leitura[i];
            }
        }

        /* Timeout de 1 segundo: se nenhum pipe tiver dados, select retorna 0
         * e o ciclo repete (permite reagir a sinais ou outros eventos futuros) */
        struct timeval tv;
        tv.tv_sec  = 1;
        tv.tv_usec = 0;

        /* select(2): bloqueia até que pelo menos um dos fds em readfds tenha dados
         * disponíveis para leitura, ou até o timeout expirar.
         * Retorna: >0 = número de fds prontos; 0 = timeout; -1 = erro. */
        int activity = select(max_fd + 1, &readfds, NULL, NULL, &tv);
        if (activity < 0) { perror("select"); exit(1); }
        if (activity == 0) continue; /* timeout: nenhum pipe com dados, volta ao início */

        /* Itera sobre todos os pipes para encontrar os que têm dados prontos */
        for (int i = 0; i < num_processos; i++) {
            /* FD_ISSET: verifica se fd[i] está marcado como pronto no conjunto */
            if (pipes_leitura[i] != -1 && FD_ISSET(pipes_leitura[i], &readfds)) {
                int tipo;
                /* Lê o campo 'tipo' da mensagem (sempre o primeiro campo enviado pelo filho) */
                ssize_t lidos = read(pipes_leitura[i], &tipo, sizeof(tipo));

                if (lidos <= 0) {
                    /* lidos == 0: EOF — o filho fechou a sua ponta do pipe (terminou).
                     * lidos  < 0: erro de leitura. Em ambos os casos, fecha o pipe. */
                    close(pipes_leitura[i]);
                    pipes_leitura[i] = -1;
                    resultados++;
                    continue;
                }

                if (tipo == MSG_PROGRESSO) {
                    /* Lê a estrutura ProgressUpdate que se segue ao campo tipo */
                    ProgressUpdate pu;
                    read(pipes_leitura[i], &pu, sizeof(pu));
                    /* Actualiza o array de progressos com a informação mais recente deste worker */
                    progressos[pu.worker_index] = pu;
                    desenhar_dashboard(progressos, num_processos);

                } else if (tipo == MSG_RESULTADO) {
                    /* Lê a estrutura WorkerResult com todas as métricas do worker */
                    WorkerResult r;
                    read(pipes_leitura[i], &r, sizeof(r));
                    /* Faz merge dos resultados parciais nos totais globais */
                    acumular_resultado(&total, &r, (char (*)[IP_LEN])ip_list_global, ip_count_global, &ip_num_global);

                    /* Marca visualmente o worker como 100% concluído no dashboard */
                    progressos[i].bytes_done = progressos[i].bytes_total;
                    desenhar_dashboard(progressos, num_processos);

                    /* Fecha o pipe deste worker e contabiliza o resultado recebido */
                    close(pipes_leitura[i]);
                    pipes_leitura[i] = -1;
                    resultados++;
                }
            }
        }
    }

    /* ── Fase 6: Recolha dos processos filhos (evitar zombies) ── */

    for (int i = 0; i < num_processos; i++) {
        int status;
        /* waitpid(2): bloqueia até o filho com PID pids[i] terminar.
         * Isto é obrigatório: sem waitpid o filho ficaria em estado zombie
         * (entrada na tabela de processos sem memória alocada) até o pai terminar. */
        waitpid(pids[i], &status, 0);
    }

    /* ── Fase 7: Relatório final e libertação de recursos ── */

    long elapsed = (long)(time(NULL) - t_inicio); /* tempo total em segundos */

    imprimir_relatorio(&total, modo);
    printf("Tempo de processamento: %ldmin %02lds\n", elapsed / 60, elapsed % 60);

    /* Libertação de toda a memória alocada dinamicamente */
    free(progressos);
    free(pipes_leitura);
    free(pids);
    free(configs);
    libertar_ficheiros(ficheiros, total_ficheiros);

    return 0;
}
