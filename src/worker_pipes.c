/**
 * @file worker_pipes.c
 * @brief Processo FILHO do sistema de análise de logs paralelo (versão com pipes anónimos).
 *
 * @details
 * Este ficheiro implementa a lógica executada por cada processo filho criado pelo pai
 * (main_pipes.c). O filho recebe uma fatia do espaço de bytes total dos ficheiros de log
 * e é responsável por:
 *
 *  1. Iterar sobre os ficheiros de log que contêm bytes da sua fatia [byte_inicio, byte_fim).
 *  2. Usar open(2) + lseek(2) para posicionar-se directamente no offset correcto,
 *     sem ler dados irrelevantes.
 *  3. Ler blocos do ficheiro com read(2), acumular caracteres linha a linha e parsear
 *     cada linha completa com as funções do módulo parser.
 *  4. Enviar actualizações de progresso periódicas ao pai via pipe (MSG_PROGRESSO).
 *  5. No final, serializar as métricas recolhidas e enviar o resultado final (MSG_RESULTADO).
 *
 * Conceitos de SO demonstrados:
 *  - open(2) / close(2): abertura e fecho de descritores de ficheiro
 *  - lseek(2): posicionamento arbitrário dentro de um ficheiro (acesso aleatório)
 *  - read(2): leitura de dados de um descritor de ficheiro para memória do processo
 *  - write(2): escrita de dados serializados no pipe (IPC filho → pai)
 *  - Gestão correcta de descritores: fechar as pontas do pipe não utilizadas
 *  - Partição de trabalho: cada worker processa apenas a sua fatia de bytes,
 *    garantindo que não há sobreposição nem lacunas entre workers
 *
 * Protocolo de mensagens no pipe:
 *  - Cada mensagem começa sempre com um int 'tipo' (MSG_PROGRESSO ou MSG_RESULTADO).
 *  - O campo 'tipo' é seguido imediatamente pela estrutura de dados correspondente
 *    (ProgressUpdate ou WorkerResult).
 *  - O pai identifica o tipo antes de ler o resto, o que torna o protocolo auto-descritivo.
 */

#include "ipc.h"
#include "parser.h"
#include "posix_io.h"
#include "worker.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* Tamanho do buffer de leitura de blocos do ficheiro (em bytes).
 * Ler em blocos é mais eficiente do que ler byte a byte (reduz syscalls). */
#define BUF_SIZE       4096
/* Comprimento máximo de uma linha de log que este worker consegue processar */
#define LINE_MAX_LOCAL 512
//test
/* Tipo de mensagem: actualização de progresso (enviada periodicamente) */
#define MSG_PROGRESSO 1
/* Tipo de mensagem: resultado final com todas as métricas (enviada uma vez, no fim) */
#define MSG_RESULTADO 2

/* ─────────────────────────────────────────────────────────────────────────────
 * Funções para enviar mensagens ao pai via pipe
 * ───────────────────────────────────────────────────────────────────────────── */

/**
 * @brief Envia uma mensagem de progresso ao processo pai através do pipe.
 *
 * @details
 * A mensagem é composta por dois write(2) consecutivos e atómicos (porque cada
 * um é menor que PIPE_BUF = 4096 bytes no Linux):
 *  1. Um int com o valor MSG_PROGRESSO — permite ao pai identificar o tipo.
 *  2. Uma estrutura ProgressUpdate com os campos de progresso.
 *
 * Esta função é chamada de 100 em 100 linhas processadas para não saturar o pipe
 * com actualizações demasiado frequentes.
 *
 * @param pipe_fd      Descritor de ficheiro da ponta de escrita do pipe anónimo.
 * @param worker_index Índice do worker (0..N-1), para o pai saber de quem é a actualização.
 * @param bytes_done   Bytes já processados dentro da fatia deste worker.
 * @param bytes_total  Tamanho total da fatia deste worker (quota).
 */
static void enviar_progresso(int pipe_fd, int worker_index, long bytes_done, long bytes_total) {
    int tipo = MSG_PROGRESSO;
    /* write(2): syscall que escreve 'sizeof(tipo)' bytes do buffer '&tipo' no descritor pipe_fd.
     * Retorna o número de bytes escritos (deve ser igual a sizeof(tipo)). */
    write(pipe_fd, &tipo, sizeof(tipo));

    /* Preenche a estrutura de progresso com os dados actuais */
    ProgressUpdate pu;
    pu.pid          = getpid();      /* getpid(): PID do processo filho actual */
    pu.worker_index = worker_index;  /* índice lógico para o dashboard do pai */
    pu.bytes_done   = bytes_done;    /* progresso actual dentro da quota */
    pu.bytes_total  = bytes_total;   /* quota total deste worker */
    /* Envia a estrutura completa de uma vez (write atómico se < PIPE_BUF) */
    write(pipe_fd, &pu, sizeof(pu));
}

/**
 * @brief Prepara a estrutura WorkerResult a partir das métricas recolhidas.
 *
 * @details
 * Copia os contadores simples da estrutura Metrics para WorkerResult e,
 * adicionalmente, ordena os IPs por contagem decrescente (bubble sort) e
 * guarda apenas os top-10 para envio eficiente pelo pipe.
 *
 * A ordenação é feita aqui (no filho) para reduzir o trabalho do pai: cada
 * filho envia já um top-10 parcialmente ordenado, simplificando o merge.
 *
 * @param m  Estrutura Metrics com as métricas brutas recolhidas durante o parsing.
 * @param r  Estrutura WorkerResult a preencher (resultado de saída).
 */
static void preparar_resultado(const Metrics *m, WorkerResult *r) {
    /* Zera toda a estrutura para garantir que campos não preenchidos ficam a 0/NULL */
    memset(r, 0, sizeof(*r));

    /* Copia o PID e os contadores simples */
    r->pid            = getpid();
    r->total_lines    = m->total_lines;
    r->count_debug    = m->count_debug;
    r->count_info     = m->count_info;
    r->count_warn     = m->count_warn;
    r->count_error    = m->count_error;
    r->count_critical = m->count_critical;
    r->count_4xx      = m->count_4xx;
    r->count_5xx      = m->count_5xx;

    /* Copia a tabela de IPs para arrays locais que serão ordenados */
    char ips[MAX_IPS][IP_LEN];
    long counts[MAX_IPS];
    int n = m->ip_num;
    if (n > MAX_IPS) n = MAX_IPS; /* protecção de limite superior */

    for (int i = 0; i < n; i++) {
        strncpy(ips[i], m->ip_list[i], IP_LEN - 1);
        ips[i][IP_LEN - 1] = '\0'; /* garante terminação nula mesmo se ip_list[i] for longo */
        counts[i] = m->ip_count[i];
    }

    /* Bubble sort descendente: coloca o IP mais frequente no índice 0.
     * Troca simultânea do array de contagens e do array de strings de IP
     * para manter a correspondência entre eles. */
    for (int i = 0; i < n - 1; i++) {
        for (int j = 0; j < n - i - 1; j++) {
            if (counts[j] < counts[j + 1]) {
                /* Troca as contagens */
                long tmp_count = counts[j];
                counts[j]     = counts[j + 1];
                counts[j + 1] = tmp_count;

                /* Troca as strings de IP correspondentes */
                char tmp_ip[IP_LEN];
                strncpy(tmp_ip, ips[j], IP_LEN);
                strncpy(ips[j], ips[j + 1], IP_LEN);
                strncpy(ips[j + 1], tmp_ip, IP_LEN);
            }
        }
    }

    /* Copia apenas os primeiros 10 IPs (ou menos, se n < 10) para o resultado */
    int limite = n < 10 ? n : 10;
    for (int i = 0; i < limite; i++) {
        strncpy(r->top_ips[i], ips[i], IP_LEN - 1);
        r->top_ips[i][IP_LEN - 1] = '\0';
        r->top_ips_counts[i] = counts[i];
    }

    /* Copia os alertas críticos, respeitando o limite máximo definido em ipc.h */
    r->num_alerts = m->num_alerts < MAX_ALERTS ? m->num_alerts : MAX_ALERTS;
    for (int i = 0; i < r->num_alerts; i++) {
        strncpy(r->alerts[i], m->alerts[i], ALERT_LEN - 1);
        r->alerts[i][ALERT_LEN - 1] = '\0';
    }
}

/**
 * @brief Serializa e envia o resultado final das métricas ao processo pai via pipe.
 *
 * @details
 * Envia dois write(2) sequenciais:
 *  1. O tipo MSG_RESULTADO (int).
 *  2. A estrutura WorkerResult completa.
 * O pai, ao ler MSG_RESULTADO, saberá que deve ler sizeof(WorkerResult) bytes a seguir.
 *
 * @param pipe_fd Descritor de ficheiro da ponta de escrita do pipe.
 * @param m       Métricas recolhidas pelo worker ao longo do processamento.
 */
static void enviar_resultado(int pipe_fd, Metrics *m) {
    int tipo = MSG_RESULTADO;
    /* Envia o tipo da mensagem primeiro — o pai usa-o para saber o que vem a seguir */
    write(pipe_fd, &tipo, sizeof(tipo));

    /* Prepara a estrutura de resultado (ordenação de IPs, cópia de alertas, etc.) */
    WorkerResult r;
    preparar_resultado(m, &r);
    /* Envia a estrutura completa de uma vez */
    write(pipe_fd, &r, sizeof(r));
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Função principal do worker
 * ───────────────────────────────────────────────────────────────────────────── */

/**
 * @brief Função principal do processo filho: processa a fatia de bytes atribuída.
 *
 * @details
 * Esta função implementa o núcleo do worker. A ideia central é que o espaço de
 * bytes de todos os ficheiros de log é visto como um espaço linear contínuo.
 * Cada worker recebe um intervalo [byte_inicio, byte_fim) nesse espaço e deve
 * processar exactamente os bytes correspondentes.
 *
 * Algoritmo detalhado:
 *  1. Para cada ficheiro, calcula o intervalo de bytes locais que pertencem à
 *     fatia deste worker (offset dentro do ficheiro).
 *  2. Usa open(2) para abrir o ficheiro e lseek(2) para saltar para local_start,
 *     evitando ler bytes desnecessários.
 *  3. Se local_start > 0, avança byte a byte até ao próximo '\n' para garantir
 *     que só processamos linhas completas (a linha a meio pertence ao worker anterior).
 *  4. Lê blocos com read(2) e acumula caracteres num buffer de linha até '\n'.
 *  5. A cada linha completa: detecta o formato (detect_format), parseia
 *     (parse_line) e actualiza as métricas (update_metrics).
 *  6. A cada 100 linhas envia uma actualização de progresso ao pai (enviar_progresso).
 *  7. Pára quando file_pos >= local_end E a linha actual foi terminada com '\n'.
 *  8. No fim de todos os ficheiros: envia progresso a 100% e o resultado final.
 *
 * @param ficheiros        Array de caminhos absolutos de todos os ficheiros de log.
 * @param total_ficheiros  Número de ficheiros no array.
 * @param pipe_fd_write    Ponta de escrita do pipe anónimo para comunicar com o pai.
 * @param worker_index     Índice lógico deste worker (0..N-1).
 * @param byte_inicio      Offset global (em bytes) onde começa a fatia deste worker.
 * @param byte_fim         Offset global (em bytes) onde termina a fatia (exclusivo).
 * @param verbose          Se != 0, imprime mensagens de debug no stdout.
 */
void run_worker_pipe(char **ficheiros, int total_ficheiros, int pipe_fd_write,
                     int worker_index, off_t byte_inicio, off_t byte_fim, int verbose) {

    /* Inicializa a estrutura de métricas (contadores a zero, tabelas vazias) */
    Metrics m;
    init_metrics(&m);

    off_t quota         = byte_fim - byte_inicio; /* tamanho total da fatia deste worker */
    off_t global_offset = 0; /* acumulador: bytes globais até ao início do ficheiro actual */
    long  linhas_feitas = 0; /* contador de linhas, para cadenciar o envio de progresso */

    char buf[BUF_SIZE];       /* buffer de blocos lidos com read(2) */
    char linha[LINE_MAX_LOCAL]; /* buffer de acumulação de uma linha de log */

    /* Em modo verbose, imprime o intervalo de bytes atribuído a este worker */
    if (verbose)
        posix_writef(STDOUT_FILENO,
                     "[Worker %d PID %d] intervalo bytes: [%lld, %lld)\n",
                     worker_index, (int)getpid(),
                     (long long)byte_inicio, (long long)byte_fim);

    /* ── Iteração sobre todos os ficheiros ── */
    for (int i = 0; i < total_ficheiros; i++) {
        /* stat(2): obtém os metadados do ficheiro sem o abrir,
         * nomeadamente st_size (tamanho em bytes). */
        struct stat st;
        if (stat(ficheiros[i], &st) != 0) continue; /* ignora ficheiro inacessível */
        off_t fsize = st.st_size;

        /* Ficheiro completamente antes da nossa fatia → ignorar e avançar offset */
        if (global_offset + fsize <= byte_inicio) {
            global_offset += fsize;
            continue;
        }
        /* Ficheiro completamente depois da nossa fatia → não há mais nada a fazer */
        if (global_offset >= byte_fim) break;

        /* Calcula os offsets locais (dentro deste ficheiro) da nossa fatia:
         *   local_start: onde começamos a ler neste ficheiro
         *   local_end:   onde paramos de ler neste ficheiro */
        off_t local_start = (byte_inicio > global_offset) ? byte_inicio - global_offset : 0;
        off_t local_end   = (byte_fim < global_offset + fsize) ? byte_fim - global_offset : fsize;

        /* open(2): abre o ficheiro em modo leitura (O_RDONLY).
         * Retorna um descritor de ficheiro inteiro, ou -1 em caso de erro. */
        int fd = open(ficheiros[i], O_RDONLY);
        if (fd < 0) { global_offset += fsize; continue; } /* abre falhou, passa ao próximo */

        if (verbose)
            posix_writef(STDOUT_FILENO,
                         "[Worker %d] %s local [%lld-%lld]\n",
                         worker_index, ficheiros[i],
                         (long long)local_start, (long long)local_end);

        /* Saltar directamente para o offset de início */
        /* lseek(2): reposiciona o cursor de leitura do ficheiro para local_start.
         * SEEK_SET: offset relativo ao início do ficheiro.
         * Isto é acesso aleatório — não é necessário ler os bytes anteriores,
         * o que seria muito ineficiente para ficheiros grandes. */
        if (lseek(fd, local_start, SEEK_SET) < 0) {
            perror("lseek");
            close(fd);
            global_offset += fsize;
            continue;
        }

        off_t file_pos = local_start; /* posição actual de leitura dentro do ficheiro */

        /*
         * Se não estamos no início do ficheiro, estamos potencialmente a meio de
         * uma linha que pertence ao worker anterior.  Avançamos até ao próximo '\n'.
         *
         * Motivo: os workers dividem o espaço de bytes, não as linhas. A fronteira
         * entre dois workers pode cair a meio de uma linha; essa linha pertence
         * ao worker cuja fatia começa antes dela (o worker anterior).
         * Este worker deve começar a processar apenas a linha seguinte.
         */
        if (local_start > 0) {
            char c;
            ssize_t r;
            /* Lê byte a byte até encontrar '\n' (fim da linha parcial) */
            while ((r = read(fd, &c, 1)) == 1) {
                file_pos++;
                if (c == '\n') break; /* encontrou o início da próxima linha completa */
            }
            if (r <= 0) {
                /* EOF ou erro — não há linhas completas neste ficheiro para este worker */
                close(fd);
                global_offset += fsize;
                continue;
            }
        }

        int len  = 0;            /* número de caracteres acumulados na linha actual */
        int done = 0;            /* flag de paragem: 1 quando a fatia foi completamente processada */
        LogFormat fmt = FORMAT_UNKNOWN; /* formato detectado dinamicamente na primeira linha */

        /* ── Ciclo principal de leitura e parsing ── */
        while (!done) {
            /* read(2): lê até BUF_SIZE bytes do descritor fd para buf.
             * Retorna: > 0 = bytes lidos; 0 = EOF; -1 = erro.
             * Ler em blocos minimiza o número de syscalls (chamadas ao kernel),
             * sendo muito mais eficiente do que ler um byte de cada vez. */
            ssize_t n = read(fd, buf, BUF_SIZE);
            if (n <= 0) break; /* EOF ou erro: termina o processamento deste ficheiro */

            /* Processa cada byte do bloco lido */
            for (int b = 0; b < (int)n && !done; b++) {
                char c = buf[b];
                file_pos++; /* avança a posição local no ficheiro */

                if (c == '\n') {
                    /* Fim de linha: parseia se houver conteúdo acumulado */
                    if (len > 0) {
                        linha[len] = '\0'; /* termina a string da linha */

                        /* detect_format: determina o formato do log (nginx, syslog, etc.)
                         * É chamado apenas uma vez por ficheiro (quando fmt == FORMAT_UNKNOWN)
                         * para evitar repetir a detecção em cada linha. */
                        if (fmt == FORMAT_UNKNOWN) fmt = detect_format(linha);

                        /* parse_line: parseia a linha de acordo com o formato detectado
                         * e preenche a estrutura LogEntry com os campos extraídos. */
                        LogEntry entry;
                        if (parse_line(linha, fmt, &entry) == 0)
                            /* update_metrics: actualiza os contadores (níveis de log, IPs, etc.)
                             * com base nos campos da LogEntry parseada. */
                            update_metrics(&m, &entry);
                        len = 0; /* reinicia o acumulador para a próxima linha */
                    }

                    linhas_feitas++;
                    /* Envia progresso ao pai de 100 em 100 linhas para não saturar o pipe */
                    if (linhas_feitas % 100 == 0) {
                        /* Calcula bytes processados relativamente ao início da nossa quota */
                        off_t bytes_done = global_offset + file_pos - byte_inicio;
                        if (bytes_done > quota) bytes_done = quota; /* não ultrapassa 100% */
                        enviar_progresso(pipe_fd_write, worker_index,
                                         (long)bytes_done, (long)quota);
                    }

                    /* Completámos a última linha da nossa fatia → podemos parar */
                    /* Só paramos APÓS o '\n': garante que não cortamos uma linha a meio */
                    if (file_pos >= local_end) done = 1;

                } else if (c != '\r') {
                    /* Acumula o caracter na linha actual (ignora '\r' para compatibilidade
                     * com ficheiros de log em formato Windows CRLF) */
                    if (len < LINE_MAX_LOCAL - 1) linha[len++] = c;
                    /*
                     * Se já passámos local_end mas ainda não vimos '\n', continuamos
                     * a acumular para terminar a linha actual (sem entrar no done).
                     * Isto garante que as linhas que cruzam a fronteira da fatia são
                     * processadas integralmente por este worker (e não pelo seguinte).
                     */
                }
            }
        }

        /* Trata a última linha do ficheiro se não terminar com '\n'
         * (alguns ficheiros de log omitem o terminador de linha final). */
        if (len > 0) {
            linha[len] = '\0';
            if (fmt == FORMAT_UNKNOWN) fmt = detect_format(linha);
            LogEntry entry;
            if (parse_line(linha, fmt, &entry) == 0)
                update_metrics(&m, &entry);
        }

        /* close(2): fecha o descritor do ficheiro, libertando o recurso no kernel.
         * É boa prática fechar os descritores assim que deixam de ser necessários,
         * evitando esgotar o limite de descritores abertos por processo (RLIMIT_NOFILE). */
        close(fd);
        global_offset += fsize; /* avança o offset global para o próximo ficheiro */
    }

    /* ── Envio do progresso final e do resultado ── */

    /* Progresso final a 100%: garante que o dashboard mostra a barra cheia */
    enviar_progresso(pipe_fd_write, worker_index, (long)quota, (long)quota);

    /* Resultado final: envia todas as métricas acumuladas ao pai */
    enviar_resultado(pipe_fd_write, &m);

    /* close(2) no pipe de escrita: sinaliza EOF ao pai.
     * O pai, ao tentar ler deste pipe, receberá 0 bytes (EOF), indicando
     * que este worker terminou. Sem este close, o pai ficaria bloqueado
     * no select/read à espera de dados que nunca chegam. */
    if (close(pipe_fd_write) == -1) {
        perror("close");
        exit(1);
    }

    /* O filho termina com exit(0). O pai recolherá este exit status com waitpid(2).
     * exit(0) indica sucesso; o pai pode verificar com WIFEXITED() e WEXITSTATUS(). */
    exit(0);
}
