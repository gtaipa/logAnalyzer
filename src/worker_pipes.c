/**
 * @file worker_pipes.c
 * @brief Processo FILHO - Arquitectura Multi-Processo com Pipes
 * @author Implementação do Analisador de Logs
 * @version 1.0
 * @date 2026
 * 
 * @details
 * Implementação da Fase 1A (15% do projeto).
 * Cada processo filho processa um intervalo de bytes específico dos ficheiros.
 * Comunica progresso e resultados finais via pipe POSIX ao processo pai.
 * 
 * FLUXO:
 *  1. Recebe intervalo de bytes [byte_inicio, byte_fim)
 *  2. Para cada ficheiro, calcula intersecção com intervalo
 *  3. Usa lseek() para saltar para byte_inicio local no ficheiro
 *  4. Lê linhas e parseia até atingir byte_fim
 *  5. A cada 100 linhas, envia MSG_PROGRESSO ao pai
 *  6. No fim, envia MSG_RESULTADO com métricas finais e top-10 IPs
 * 
 * COMUNICAÇÃO:
 *  - MSG_PROGRESSO: int(tipo) + ProgressUpdate struct
 *  - MSG_RESULTADO: int(tipo) + WorkerResult struct
 */

#include "ipc.h" // Incluir header com definições de IPC
#include "parser.h" // Incluir header com funções de parsing
#include "posix_io.h" // Incluir header com funções de I/O POSIX
#include "worker.h" // Incluir header com definições do worker

#include <errno.h> // Incluir header com variável de erro
#include <fcntl.h> // Incluir header com flags de ficheiro
#include <stdio.h> // Incluir header com funções de I/O padrão
#include <stdlib.h> // Incluir header com funções gerais
#include <string.h> // Incluir header com funções de string
#include <sys/stat.h> // Incluir header com estruturas de ficheiro
#include <sys/types.h> // Incluir header com tipos de dados
#include <unistd.h> // Incluir header com funções POSIX

#define BUF_SIZE       4096 // Definir tamanho do buffer de leitura
#define LINE_MAX_LOCAL 512 // Definir tamanho máximo de uma linha local
#define MSG_PROGRESSO 1 // Definir tipo de mensagem de progresso
#define MSG_RESULTADO 2 // Definir tipo de mensagem de resultado

/**
 * @brief Envia mensagem de progresso ao processo pai via pipe
 * 
 * @param pipe_fd Descritor de ficheiro do pipe de escrita
 * @param worker_index Índice do worker (0..N-1)
 * @param bytes_done Bytes já processados nesta quota
 * @param bytes_total Bytes totais desta quota
 */
static void enviar_progresso(int pipe_fd, int worker_index, long bytes_done, long bytes_total) { // Função para enviar progresso ao pai
    int tipo = MSG_PROGRESSO; // Atribuir tipo de mensagem
    write(pipe_fd, &tipo, sizeof(tipo)); // Escrever tipo no pipe

    ProgressUpdate pu; // Declarar estrutura de progresso
    pu.pid          = getpid(); // Atribuir PID do processo
    pu.worker_index = worker_index; // Atribuir índice do worker
    pu.bytes_done   = bytes_done; // Atribuir bytes processados
    pu.bytes_total  = bytes_total; // Atribuir bytes totais
    write(pipe_fd, &pu, sizeof(pu)); // Escrever estrutura no pipe
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
static void preparar_resultado(const Metrics *m, WorkerResult *r) { // Função para preparar resultado final
    memset(r, 0, sizeof(*r)); // Limpar estrutura de resultado
    r->pid            = getpid(); // Atribuir PID do processo
    r->total_lines    = m->total_lines; // Atribuir total de linhas
    r->count_debug    = m->count_debug; // Atribuir contagem de DEBUG
    r->count_info     = m->count_info; // Atribuir contagem de INFO
    r->count_warn     = m->count_warn; // Atribuir contagem de WARN
    r->count_error    = m->count_error; // Atribuir contagem de ERROR
    r->count_critical = m->count_critical; // Atribuir contagem de CRITICAL
    r->count_4xx      = m->count_4xx; // Atribuir contagem de HTTP 4xx
    r->count_5xx      = m->count_5xx; // Atribuir contagem de HTTP 5xx

    char ips[MAX_IPS][IP_LEN]; // Declarar array para IPs
    long counts[MAX_IPS]; // Declarar array para contagens
    int n = m->ip_num; // Atribuir número de IPs
    if (n > MAX_IPS) n = MAX_IPS; // Limitar número de IPs ao máximo

    for (int i = 0; i < n; i++) { // Ciclo para cada IP
        strncpy(ips[i], m->ip_list[i], IP_LEN - 1); // Copiar IP
        ips[i][IP_LEN - 1] = '\0'; // Terminar string
        counts[i] = m->ip_count[i]; // Copiar contagem
    }

    for (int i = 0; i < n - 1; i++) { // Ciclo externo de bubble sort
        for (int j = 0; j < n - i - 1; j++) { // Ciclo interno de bubble sort
            if (counts[j] < counts[j + 1]) { // Se contagem menor que próxima
                long tmp_count = counts[j]; // Guardar contagem temporária
                counts[j] = counts[j + 1]; // Mover contagem maior
                counts[j + 1] = tmp_count; // Colocar contagem menor

                char tmp_ip[IP_LEN]; // Declarar IP temporário
                strncpy(tmp_ip, ips[j], IP_LEN); // Guardar IP temporário
                strncpy(ips[j], ips[j + 1], IP_LEN); // Mover IP maior
                strncpy(ips[j + 1], tmp_ip, IP_LEN); // Colocar IP menor
            }
        }
    }

    int limite = n < 10 ? n : 10; // Definir limite até 10 IPs
    for (int i = 0; i < limite; i++) { // Ciclo para os top 10 IPs
        strncpy(r->top_ips[i], ips[i], IP_LEN - 1); // Copiar IP top
        r->top_ips[i][IP_LEN - 1] = '\0'; // Terminar string
        r->top_ips_counts[i] = counts[i]; // Copiar contagem top
    }

    r->num_alerts = m->num_alerts < MAX_ALERTS ? m->num_alerts : MAX_ALERTS; // Atribuir número de alertas
    for (int i = 0; i < r->num_alerts; i++) { // Ciclo para cada alerta
        strncpy(r->alerts[i], m->alerts[i], ALERT_LEN - 1); // Copiar alerta
        r->alerts[i][ALERT_LEN - 1] = '\0'; // Terminar string
    }
}

/**
 * @brief Envia resultado final ao processo pai via pipe
 * 
 * @param pipe_fd Descritor de ficheiro do pipe de escrita
 * @param m Métricas acumuladas
 */
static void enviar_resultado(int pipe_fd, Metrics *m) { // Função para enviar resultado final
    int tipo = MSG_RESULTADO; // Atribuir tipo de mensagem
    write(pipe_fd, &tipo, sizeof(tipo)); // Escrever tipo no pipe

    WorkerResult r; // Declarar estrutura de resultado
    preparar_resultado(m, &r); // Preparar resultado
    write(pipe_fd, &r, sizeof(r)); // Escrever resultado no pipe
}

/**
 * @brief Função principal do processo filho - processa intervalo de bytes via pipe
 * 
 * @details
 * Processa ficheiros dentro do intervalo [byte_inicio, byte_fim).
 * Usa lseek() para saltar para posições específicas. Envia progresso
 * e resultado final via pipe ao pai.
 * 
 * @param ficheiros Array de caminhos de ficheiros
 * @param total_ficheiros Número de ficheiros
 * @param pipe_fd_write Descritor do pipe para escrita
 * @param worker_index Índice do worker
 * @param byte_inicio Byte inicial (inclusivo)
 * @param byte_fim Byte final (exclusivo)
 * @param verbose Flag para saída detalhada
 */
void run_worker_pipe(char **ficheiros, int total_ficheiros, int pipe_fd_write, // Função principal do worker com pipes
                     int worker_index, off_t byte_inicio, off_t byte_fim, int verbose) {
    Metrics m; // Declarar estrutura de métricas
    init_metrics(&m); // Inicializar métricas

    off_t quota          = byte_fim - byte_inicio; // Calcular quota de bytes
    off_t global_offset  = 0; // Inicializar offset global
    long  linhas_feitas  = 0; // Inicializar contador de linhas

    char buf[BUF_SIZE]; // Declarar buffer de leitura
    char linha[LINE_MAX_LOCAL]; // Declarar buffer de linha

    if (verbose) // Se modo verbose ativo
        posix_writef(STDOUT_FILENO, // Escrever em STDOUT
                     "[Worker %d PID %d] intervalo bytes: [%lld, %lld)\n",
                     worker_index, (int)getpid(),
                     (long long)byte_inicio, (long long)byte_fim);

    for (int i = 0; i < total_ficheiros; i++) { // Ciclo para cada ficheiro
        struct stat st; // Declarar estrutura de estatísticas
        if (stat(ficheiros[i], &st) != 0) continue; // Se erro ao obter stats, continuar
        off_t fsize = st.st_size; // Obter tamanho do ficheiro

        if (global_offset + fsize <= byte_inicio) { // Se ficheiro está completamente antes
            global_offset += fsize; // Avançar offset global
            continue; // Continuar com próximo ficheiro
        }
        if (global_offset >= byte_fim) break; // Se ficheiro está completamente depois, parar

        off_t local_start = (byte_inicio > global_offset) ? byte_inicio - global_offset : 0; // Calcular início local
        off_t local_end   = (byte_fim < global_offset + fsize) ? byte_fim - global_offset : fsize; // Calcular fim local

        int fd = open(ficheiros[i], O_RDONLY); // Abrir ficheiro para leitura
        if (fd < 0) { global_offset += fsize; continue; } // Se erro, continuar

        if (verbose) // Se modo verbose ativo
            posix_writef(STDOUT_FILENO, // Escrever em STDOUT
                         "[Worker %d] %s local [%lld-%lld]\n",
                         worker_index, ficheiros[i],
                         (long long)local_start, (long long)local_end);

        if (lseek(fd, local_start, SEEK_SET) < 0) { // Se erro ao saltar para offset
            perror("lseek"); // Imprimir erro
            close(fd); // Fechar ficheiro
            global_offset += fsize; // Avançar offset global
            continue; // Continuar com próximo ficheiro
        }

        off_t file_pos = local_start; // Inicializar posição do ficheiro

        if (local_start > 0) { // Se não está no início do ficheiro
            char c; // Declarar caracter
            ssize_t r; // Declarar bytes lidos
            while ((r = read(fd, &c, 1)) == 1) { // Ciclo enquanto ler caracteres
                file_pos++; // Avançar posição
                if (c == '\n') break; // Se encontrar newline, sair
            }
            if (r <= 0) { // Se erro ou EOF
                close(fd); // Fechar ficheiro
                global_offset += fsize; // Avançar offset global
                continue; // Continuar com próximo ficheiro
            }
        }

        int len  = 0; // Inicializar comprimento da linha
        int done = 0; // Inicializar flag de conclusão
        LogFormat fmt = FORMAT_UNKNOWN; // Inicializar formato de log

        while (!done) { // Ciclo enquanto não terminar
            ssize_t n = read(fd, buf, BUF_SIZE); // Ler do ficheiro
            if (n <= 0) break; // Se EOF ou erro, sair

            for (int b = 0; b < (int)n && !done; b++) { // Ciclo para cada byte
                char c = buf[b]; // Obter caracter
                file_pos++; // Avançar posição

                if (c == '\n') { // Se caracter é newline
                    if (len > 0) { // Se linha tem conteúdo
                        linha[len] = '\0'; // Terminar string
                        if (fmt == FORMAT_UNKNOWN) fmt = detect_format(linha); // Detetar formato
                        LogEntry entry; // Declarar entrada de log
                        if (parse_line(linha, fmt, &entry) == 0) // Se parsing com sucesso
                            update_metrics(&m, &entry); // Atualizar métricas
                        len = 0; // Resetar comprimento
                    }

                    linhas_feitas++; // Incrementar contador de linhas
                    if (linhas_feitas % 100 == 0) { // A cada 100 linhas
                        off_t bytes_done = global_offset + file_pos - byte_inicio; // Calcular bytes processados
                        if (bytes_done > quota) bytes_done = quota; // Limitar aos bytes da quota
                        enviar_progresso(pipe_fd_write, worker_index, // Enviar progresso
                                         (long)bytes_done, (long)quota);
                    }

                    if (file_pos >= local_end) done = 1; // Se passou fim, marcar como concluído

                } else if (c != '\r') { // Se não é carriage return
                    if (len < LINE_MAX_LOCAL - 1) linha[len++] = c; // Adicionar caracter à linha
                }
            }
        }

        if (len > 0) { // Se linha tem conteúdo no fim do ficheiro
            linha[len] = '\0'; // Terminar string
            if (fmt == FORMAT_UNKNOWN) fmt = detect_format(linha); // Detetar formato
            LogEntry entry; // Declarar entrada de log
            if (parse_line(linha, fmt, &entry) == 0) // Se parsing com sucesso
                update_metrics(&m, &entry); // Atualizar métricas
        }

        close(fd); // Fechar ficheiro
        global_offset += fsize; // Avançar offset global
    }

    enviar_progresso(pipe_fd_write, worker_index, (long)quota, (long)quota); // Enviar progresso final 100%

    enviar_resultado(pipe_fd_write, &m); // Enviar resultado final

    if (close(pipe_fd_write) == -1) { // Se erro ao fechar pipe
        perror("close"); // Imprimir erro
        exit(1); // Sair com erro
    }

    exit(0); // Sair com sucesso
}
