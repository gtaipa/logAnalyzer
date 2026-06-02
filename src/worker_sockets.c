/**
 * @file worker_sockets.c
 * @brief Processo FILHO - Arquitectura Multi-Processo com Sockets Unix Domain
 * @author Implementação do Analisador de Logs
 * @version 1.0
 * @date 2026
 * 
 * @details
 * Implementação da Fase 1A (15% do projeto) com sockets Unix Domain.
 * Cada processo filho conecta-se ao servidor pai, recebe configuração
 * (intervalo de bytes), processa ficheiros, envia progresso e resultado via socket.
 * 
 * FLUXO:
 *  1. Conecta ao servidor pai via Unix Domain Socket
 *  2. Recebe MSG_CONFIG com byte_inicio, byte_fim, worker_index
 *  3. Processa ficheiros no intervalo [byte_inicio, byte_fim)
 *  4. Envia MSG_PROGRESSO a cada 100 linhas
 *  5. Envia MSG_RESULTADO com métricas finais e top-10 IPs
 * 
 * COMUNICAÇÃO:
 *  - Bidirecional: filho lê config, depois escreve progresso/resultado
 *  - MSG_CONFIG: int(tipo) + WorkerConfig struct
 *  - MSG_PROGRESSO: int(tipo) + ProgressUpdate struct
 *  - MSG_RESULTADO: int(tipo) + WorkerResult struct
 */

#include "worker.h" // Incluir header do worker
#include "parser.h" // Incluir header do parser
#include "ipc.h" // Incluir header de IPC

#include <stdio.h> // Incluir header com funções de I/O padrão
#include <stdlib.h> // Incluir header com funções gerais
#include <string.h> // Incluir header com funções de string
#include <unistd.h> // Incluir header com funções POSIX
#include <fcntl.h> // Incluir header com flags de ficheiro
#include <errno.h> // Incluir header com variável de erro
#include <sys/stat.h> // Incluir header com estruturas de ficheiro
#include <sys/types.h> // Incluir header com tipos de dados

#define BUF_SIZE  4096 // Definir tamanho do buffer
#define LINHA_MAX  512 // Definir tamanho máximo de linha

/**
 * @brief Envia mensagem de progresso ao servidor pai via socket
 * 
 * @param sock Descritor do socket ligado
 * @param worker_index Índice do worker
 * @param bytes_done Bytes já processados nesta quota
 * @param bytes_total Bytes totais desta quota
 */
static void enviar_progresso(int sock, int worker_index, long bytes_done, long bytes_total) { // Função para enviar progresso
    int tipo = MSG_PROGRESSO; // Atribuir tipo de mensagem
    write(sock, &tipo, sizeof(tipo)); // Escrever tipo no socket

    ProgressUpdate pu; // Declarar estrutura de progresso
    pu.pid          = getpid(); // Atribuir PID
    pu.worker_index = worker_index; // Atribuir índice do worker
    pu.bytes_done   = bytes_done; // Atribuir bytes processados
    pu.bytes_total  = bytes_total; // Atribuir bytes totais
    write(sock, &pu, sizeof(pu)); // Escrever progresso no socket
}

/**
 * @brief Prepara resultado final com métricas e top-10 IPs ordenados
 * 
 * @details
 * Ordena IPs por frequência (decrescente) via bubble sort.
 * Copia os 10 mais frequentes para resultado.
 * 
 * @param m Métricas acumuladas
 * @param r Estrutura de resultado a preencher
 */
static void preparar_resultado(const Metrics *m, WorkerResult *r) { // Função para preparar resultado
    memset(r, 0, sizeof(*r)); // Limpar estrutura
    r->pid            = getpid(); // Atribuir PID
    r->total_lines    = m->total_lines; // Atribuir total de linhas
    r->count_debug    = m->count_debug; // Atribuir contagem DEBUG
    r->count_info     = m->count_info; // Atribuir contagem INFO
    r->count_warn     = m->count_warn; // Atribuir contagem WARN
    r->count_error    = m->count_error; // Atribuir contagem ERROR
    r->count_critical = m->count_critical; // Atribuir contagem CRITICAL
    r->count_4xx      = m->count_4xx; // Atribuir contagem HTTP 4xx
    r->count_5xx      = m->count_5xx; // Atribuir contagem HTTP 5xx

    char ips[MAX_IPS][IP_LEN]; // Declarar array para IPs
    long counts[MAX_IPS]; // Declarar array para contagens
    int n = m->ip_num; // Obter número de IPs
    if (n > MAX_IPS) n = MAX_IPS; // Limitar número de IPs

    for (int i = 0; i < n; i++) { // Ciclo para cada IP
        strncpy(ips[i], m->ip_list[i], IP_LEN - 1); // Copiar IP
        ips[i][IP_LEN - 1] = '\0'; // Terminar string
        counts[i] = m->ip_count[i]; // Copiar contagem
    }

    for (int i = 0; i < n - 1; i++) { // Ciclo externo bubble sort
        for (int j = 0; j < n - i - 1; j++) { // Ciclo interno bubble sort
            if (counts[j] < counts[j + 1]) { // Se contagem menor
                long tmp_count = counts[j]; // Guardar temp
                counts[j] = counts[j + 1]; // Mover maior
                counts[j + 1] = tmp_count; // Colocar menor

                char tmp_ip[IP_LEN]; // Declarar IP temp
                strncpy(tmp_ip, ips[j], IP_LEN); // Guardar IP temp
                strncpy(ips[j], ips[j + 1], IP_LEN); // Mover IP maior
                strncpy(ips[j + 1], tmp_ip, IP_LEN); // Colocar IP menor
            }
        }
    }

    int limite = n < 10 ? n : 10; // Definir limite até 10
    for (int i = 0; i < limite; i++) { // Ciclo para top 10
        strncpy(r->top_ips[i], ips[i], IP_LEN - 1); // Copiar IP
        r->top_ips[i][IP_LEN - 1] = '\0'; // Terminar string
        r->top_ips_counts[i] = counts[i]; // Copiar contagem
    }

    r->num_alerts = m->num_alerts < MAX_ALERTS ? m->num_alerts : MAX_ALERTS; // Atribuir alertas
    for (int i = 0; i < r->num_alerts; i++) { // Ciclo para cada alerta
        strncpy(r->alerts[i], m->alerts[i], ALERT_LEN - 1); // Copiar alerta
        r->alerts[i][ALERT_LEN - 1] = '\0'; // Terminar string
    }
}

/**
 * @brief Envia resultado final ao servidor pai via socket
 * 
 * @param sock Descritor do socket ligado
 * @param m Métricas acumuladas
 */
static void enviar_resultado(int sock, Metrics *m) { // Função para enviar resultado
    int tipo = MSG_RESULTADO; // Atribuir tipo
    write(sock, &tipo, sizeof(tipo)); // Escrever tipo no socket

    WorkerResult r; // Declarar resultado
    preparar_resultado(m, &r); // Preparar resultado
    write(sock, &r, sizeof(r)); // Escrever resultado no socket
}

/**
 * @brief Processa intervalo de bytes específico com leitura via lseek
 * 
 * @details
 * Itera sobre ficheiros, calculando intersecção com intervalo [byte_inicio, byte_fim).
 * Para cada ficheiro, usa lseek() para saltar para byte_inicio local.
 * Lê linhas, parseia e atualiza métricas. Envia progresso a cada 100 linhas.
 * 
 * @param ficheiros Array de caminhos de ficheiros
 * @param total_ficheiros Número de ficheiros
 * @param m Métricas a preencher
 * @param sock Socket para enviar progresso/resultado
 * @param worker_index Índice do worker
 * @param byte_inicio Byte inicial (inclusivo)
 * @param byte_fim Byte final (exclusivo)
 * @param verbose Flag para saída detalhada
 */
static void processar_por_bytes(char **ficheiros, int total_ficheiros, Metrics *m, // Função para processar bytes
                                int sock, int worker_index,
                                off_t byte_inicio, off_t byte_fim, int verbose) {
    off_t quota         = byte_fim - byte_inicio; // Calcular quota
    off_t global_offset = 0; // Inicializar offset global
    long  linhas_feitas = 0; // Inicializar contador de linhas

    char buf[BUF_SIZE]; // Declarar buffer
    char linha[LINHA_MAX]; // Declarar linha

    for (int i = 0; i < total_ficheiros; i++) { // Ciclo para cada ficheiro
        struct stat st; // Declarar estrutura de stats
        if (stat(ficheiros[i], &st) != 0) continue; // Se erro, continuar
        off_t fsize = st.st_size; // Obter tamanho

        if (global_offset + fsize <= byte_inicio) { // Se antes do início
            global_offset += fsize; // Avançar offset
            continue; // Continuar
        }
        if (global_offset >= byte_fim) break; // Se depois do fim, parar

        off_t local_start = (byte_inicio > global_offset) ? byte_inicio - global_offset : 0; // Calcular início
        off_t local_end   = (byte_fim < global_offset + fsize) ? byte_fim - global_offset : fsize; // Calcular fim

        int fd = open(ficheiros[i], O_RDONLY); // Abrir ficheiro
        if (fd < 0) { global_offset += fsize; continue; } // Se erro, continuar

        if (verbose) // Se verbose
            printf("[Worker %d] %s local [%lld-%lld]\n", // Imprimir
                   worker_index, ficheiros[i],
                   (long long)local_start, (long long)local_end);

        if (lseek(fd, local_start, SEEK_SET) < 0) { // Se erro no seek
            perror("lseek"); // Imprimir erro
            close(fd); // Fechar ficheiro
            global_offset += fsize; // Avançar offset
            continue; // Continuar
        }

        off_t file_pos = local_start; // Inicializar posição

        if (local_start > 0) { // Se não no início
            char c; // Declarar caracter
            ssize_t r; // Declarar bytes
            while ((r = read(fd, &c, 1)) == 1) { // Ciclo de leitura
                file_pos++; // Avançar posição
                if (c == '\n') break; // Se newline, sair
            }
            if (r <= 0) { // Se EOF ou erro
                close(fd); // Fechar ficheiro
                global_offset += fsize; // Avançar offset
                continue; // Continuar
            }
        }

        int len  = 0; // Inicializar comprimento
        int done = 0; // Inicializar done
        LogFormat fmt = FORMAT_UNKNOWN; // Inicializar formato

        while (!done) { // Ciclo principal
            ssize_t n = read(fd, buf, BUF_SIZE); // Ler do ficheiro
            if (n <= 0) break; // Se EOF, sair

            for (int b = 0; b < (int)n && !done; b++) { // Ciclo para bytes
                char c = buf[b]; // Obter caracter
                file_pos++; // Avançar posição

                if (c == '\n') { // Se newline
                    if (len > 0) { // Se linha tem conteúdo
                        linha[len] = '\0'; // Terminar string
                        if (fmt == FORMAT_UNKNOWN) fmt = detect_format(linha); // Detetar formato
                        LogEntry entrada; // Declarar entrada
                        if (parse_line(linha, fmt, &entrada) == 0) // Se parsing ok
                            update_metrics(m, &entrada); // Atualizar métricas
                        len = 0; // Resetar comprimento
                    }

                    linhas_feitas++; // Incrementar contador
                    if (linhas_feitas % 100 == 0) { // A cada 100 linhas
                        off_t bytes_done = global_offset + file_pos - byte_inicio; // Calcular bytes
                        if (bytes_done > quota) bytes_done = quota; // Limitar
                        enviar_progresso(sock, worker_index, // Enviar progresso
                                         (long)bytes_done, (long)quota);
                    }

                    if (file_pos >= local_end) done = 1; // Se passou fim, marcar done

                } else if (c != '\r') { // Se não carriage return
                    if (len < LINHA_MAX - 1) linha[len++] = c; // Adicionar caracter
                }
            }
        }

        if (len > 0) { // Se linha tem conteúdo no fim
            linha[len] = '\0'; // Terminar string
            if (fmt == FORMAT_UNKNOWN) fmt = detect_format(linha); // Detetar formato
            LogEntry entrada; // Declarar entrada
            if (parse_line(linha, fmt, &entrada) == 0) // Se parsing ok
                update_metrics(m, &entrada); // Atualizar métricas
        }

        close(fd); // Fechar ficheiro
        global_offset += fsize; // Avançar offset
    }
}

/**
 * @brief Função principal do processo filho - conecta ao servidor e processa bytes
 * 
 * @details
 * Cada worker filho conecta ao servidor pai, recebe configuração (intervalo de bytes),
 * processa ficheiros nesse intervalo, envia progresso e resultado via socket.
 * 
 * @param ficheiros Array de caminhos de ficheiros
 * @param total_ficheiros Número de ficheiros
 * @param num_processos Número total de workers (não utilizado)
 * @param worker_index_original Índice original (não utilizado)
 * @param verbose Flag para saída detalhada
 */
void run_worker(char **ficheiros, int total_ficheiros, int num_processos, int worker_index_original, int verbose) { // Função principal do worker
    (void)num_processos; // Não usar parâmetro
    (void)worker_index_original; // Não usar parâmetro

    int sock = connect_to_server(); // Ligar ao servidor
    if (sock < 0) { perror("connect_to_server"); exit(1); } // Se erro, sair

    int tipo; // Declarar tipo
    read(sock, &tipo, sizeof(tipo)); // Ler tipo

    if (tipo != MSG_CONFIG) { // Se tipo errado
        fprintf(stderr, "Worker esperava MSG_CONFIG, recebeu tipo %d\n", tipo); // Imprimir erro
        exit(1); // Sair com erro
    }

    WorkerConfig cfg; // Declarar config
    read(sock, &cfg, sizeof(cfg)); // Ler config

    int   worker_index = cfg.worker_index; // Obter índice
    off_t byte_inicio  = cfg.byte_inicio; // Obter início
    off_t byte_fim     = cfg.byte_fim; // Obter fim
    off_t quota        = byte_fim - byte_inicio; // Calcular quota

    if (verbose) // Se verbose
        printf("[Worker %d (PID %d)] intervalo bytes: [%lld, %lld) quota=%lld\n", // Imprimir
               worker_index, (int)getpid(),
               (long long)byte_inicio, (long long)byte_fim, (long long)quota);

    Metrics m; // Declarar métricas
    init_metrics(&m); // Inicializar métricas

    processar_por_bytes(ficheiros, total_ficheiros, &m, // Processar bytes
                        sock, worker_index, byte_inicio, byte_fim, verbose);

    enviar_progresso(sock, worker_index, (long)quota, (long)quota); // Enviar progresso final
    enviar_resultado(sock, &m); // Enviar resultado
    close(sock); // Fechar socket
}
