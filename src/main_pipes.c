/**
 * @file main_pipes.c
 * @brief Processo PAI - Arquitectura Multi-Processo com Pipes POSIX
 * @author Implementação do Analisador de Logs
 * @version 1.0
 * @date 2026
 * 
 * @details
 * Implementação da Fase 1A (15% do projeto).
 * Processo pai que coordena N workers via pipes POSIX.
 * 
 * FLUXO:
 *  1. Descobre ficheiros .log/.json no diretório
 *  2. Calcula total de bytes
 *  3. Divide intervalo [0, total_bytes) em N slices iguais
 *  4. Cria N pipes e N processos filho
 *  5. Usa select() para multiplexagem assíncrona de N pipes
 *  6. Lê mensagens MSG_PROGRESSO de cada worker e desenha dashboard
 *  7. Lê MSG_RESULTADO final e acumula métricas
 *  8. Aguarda todos workers e imprime relatório
 * 
 * COMUNICAÇÃO:
 *  - Unidirecional: filho escreve, pai lê
 *  - MSG_PROGRESSO: int(tipo) + ProgressUpdate struct
 *  - MSG_RESULTADO: int(tipo) + WorkerResult struct
 */

#include <dirent.h> // Incluir header para navegação de diretórios
#include <errno.h> // Incluir header com variável de erro
#include <fcntl.h> // Incluir header com flags de ficheiro
#include <stdio.h> // Incluir header com funções de I/O padrão
#include <stdlib.h> // Incluir header com funções gerais
#include <string.h> // Incluir header com funções de string
#include <sys/stat.h> // Incluir header com estruturas de ficheiro
#include <sys/wait.h> // Incluir header com funções de espera de processos
#include <sys/select.h> // Incluir header com função select
#include <time.h> // Incluir header com funções de tempo
#include <unistd.h> // Incluir header com funções POSIX

#include "ipc.h" // Incluir header com definições de IPC
#include "parser.h" // Incluir header com funções de parsing
#include "posix_io.h" // Incluir header com funções de I/O POSIX
#include "worker.h" // Incluir header com definições do worker

#define MSG_PROGRESSO 1 // Definir tipo de mensagem de progresso
#define MSG_RESULTADO 2 // Definir tipo de mensagem de resultado
#define LARGURA_BARRA 20 // Definir largura da barra de progresso
#define BUF_SIZE 4096 // Definir tamanho do buffer de leitura

/**
 * @brief Declaração da função do worker (definida em worker_pipes.c)
 */
void run_worker_pipe(char **ficheiros, int total_ficheiros, int pipe_fd_write, // Declarar função do worker
                     int worker_index, off_t byte_inicio, off_t byte_fim, int verbose);

/**
 * @brief Desenha dashboard com barras de progresso de todos os workers
 * 
 * @param progressos Array com atualizações de progresso
 * @param num_workers Número de workers
 */
static void desenhar_dashboard(ProgressUpdate *progressos, int num_workers) { // Função para desenhar dashboard
    printf("\033[%dA", num_workers); // Subir cursor N linhas
    printf("\033[J"); // Apagar tudo abaixo do cursor

    for (int i = 0; i < num_workers; i++) { // Ciclo para cada worker
        long feitas = progressos[i].bytes_done; // Obter bytes processados
        long total  = progressos[i].bytes_total; // Obter bytes totais

        int pct = (total > 0) ? (int)(feitas * 100 / total) : 0; // Calcular percentagem
        if (pct > 100) pct = 100; // Limitar percentagem a 100%

        char barra[LARGURA_BARRA + 1]; // Declarar array para barra
        int cheio = pct * LARGURA_BARRA / 100; // Calcular parte cheia
        for (int b = 0; b < LARGURA_BARRA; b++) // Ciclo para cada caracter
            barra[b] = (b < cheio) ? '#' : '.'; // Preencher barra
        barra[LARGURA_BARRA] = '\0'; // Terminar string

        printf("Worker %2d [%s] %3d%% (%ld/%ld bytes)\n", // Imprimir linha do worker
               i, barra, pct, feitas, total);
    }
    fflush(stdout); // Forçar escrita no stdout
}

/**
 * @brief Liberta memória alocada para array de ficheiros
 * 
 * @param ficheiros Array de ponteiros para strings
 * @param total_ficheiros Número de strings
 */
static void libertar_ficheiros(char **ficheiros, int total_ficheiros) { // Função para libertar memória de ficheiros
    if (ficheiros == NULL) return; // Se nulo, retornar
    for (int i = 0; i < total_ficheiros; i++) free(ficheiros[i]); // Libertar cada string
    free(ficheiros); // Libertar array de ponteiros
}

/**
 * @brief Calcula total de bytes em todos os ficheiros
 * 
 * @param ficheiros Array de caminhos
 * @param total_ficheiros Número de ficheiros
 * @return Total de bytes
 */
static off_t obter_bytes_totais(char **ficheiros, int total_ficheiros) { // Função para calcular bytes totais
    off_t total = 0; // Inicializar total
    struct stat st; // Declarar estrutura de stats
    for (int i = 0; i < total_ficheiros; i++) { // Ciclo para cada ficheiro
        if (stat(ficheiros[i], &st) == 0) // Se conseguir obter stats
            total += st.st_size; // Adicionar tamanho
    }
    return total; // Retornar total
}

/**
 * @brief Converte argumento de texto em número de processos com validação
 * 
 * @param texto String com número
 * @return Número convertido (> 0)
 */
static int converter_num_processos(const char *texto) { // Função para converter texto em número
    char *fim = NULL; // Declarar ponteiro para fim
    errno = 0; // Limpar errno
    long valor = strtol(texto, &fim, 10); // Converter para longo
    if (errno != 0 || fim == texto || *fim != '\0' || valor <= 0) { // Se erro de conversão
        posix_writef(STDERR_FILENO, "Numero de processos invalido: %s\n", texto); // Imprimir erro
        exit(1); // Sair com erro
    }
    return (int)valor; // Retornar como inteiro
}

/**
 * @brief Acumula resultado de um worker na tabela global
 * 
 * @details
 * Atualiza métricas globais, merge de IPs, reordenação.
 * 
 * @param total Resultado global acumulado
 * @param r Resultado de um worker
 * @param ip_list_global Tabela global de IPs
 * @param ip_count_global Tabela global de contagens
 * @param ip_num_global Ponteiro para número de IPs na tabela
 */
static void acumular_resultado(WorkerResult *total, const WorkerResult *r, // Função para acumular resultados
                               char ip_list_global[256][IP_LEN], long ip_count_global[256], int *ip_num_global) {
    total->total_lines += r->total_lines; // Acumular linhas totais
    total->count_debug += r->count_debug; // Acumular contagem DEBUG
    total->count_info  += r->count_info; // Acumular contagem INFO
    total->count_warn  += r->count_warn; // Acumular contagem WARN
    total->count_error += r->count_error; // Acumular contagem ERROR
    total->count_critical += r->count_critical; // Acumular contagem CRITICAL
    total->count_4xx += r->count_4xx; // Acumular contagem HTTP 4xx
    total->count_5xx += r->count_5xx; // Acumular contagem HTTP 5xx

    for (int k = 0; k < 10; k++) { // Ciclo para top 10 IPs do worker
        if (r->top_ips[k][0] == '\0' || r->top_ips_counts[k] <= 0) continue; // Se vazio, continuar

        int found = -1; // Inicializar flag de procura
        for (int i = 0; i < *ip_num_global; i++) { // Ciclo na tabela global
            if (strcmp(ip_list_global[i], r->top_ips[k]) == 0) { // Se IP encontrado
                found = i; // Guardar índice
                break; // Sair do ciclo
            }
        }
        if (found == -1 && *ip_num_global < 256) { // Se IP novo e espaço disponível
            strncpy(ip_list_global[*ip_num_global], r->top_ips[k], IP_LEN - 1); // Copiar IP
            ip_list_global[*ip_num_global][IP_LEN - 1] = '\0'; // Terminar string
            ip_count_global[*ip_num_global] = r->top_ips_counts[k]; // Atribuir contagem
            (*ip_num_global)++; // Incrementar contador
        } else if (found >= 0) { // Se IP já existe
            ip_count_global[found] += r->top_ips_counts[k]; // Acumular contagem
        }
    }

    for (int i = 0; i < r->num_alerts && total->num_alerts < MAX_ALERTS; i++) { // Ciclo para alertas
        strncpy(total->alerts[total->num_alerts], r->alerts[i], ALERT_LEN - 1); // Copiar alerta
        total->alerts[total->num_alerts][ALERT_LEN - 1] = '\0'; // Terminar string
        total->num_alerts++; // Incrementar contador
    }

    for (int i = 0; i < *ip_num_global - 1; i++) { // Ciclo externo de bubble sort
        for (int j = 0; j < *ip_num_global - i - 1; j++) { // Ciclo interno de bubble sort
            if (ip_count_global[j] < ip_count_global[j + 1]) { // Se contagem menor
                long tmp_count = ip_count_global[j]; // Guardar contagem temp
                ip_count_global[j] = ip_count_global[j + 1]; // Mover maior
                ip_count_global[j + 1] = tmp_count; // Colocar menor

                char tmp_ip[IP_LEN]; // Declarar IP temp
                strncpy(tmp_ip, ip_list_global[j], IP_LEN); // Guardar IP temp
                strncpy(ip_list_global[j], ip_list_global[j + 1], IP_LEN); // Mover IP maior
                strncpy(ip_list_global[j + 1], tmp_ip, IP_LEN); // Colocar IP menor
            }
        }
    }

    memset(total->top_ips, 0, sizeof(total->top_ips)); // Limpar top IPs
    memset(total->top_ips_counts, 0, sizeof(total->top_ips_counts)); // Limpar contagens
    int limite = *ip_num_global < 10 ? *ip_num_global : 10; // Definir limite
    for (int i = 0; i < limite; i++) { // Ciclo para os top IPs
        strncpy(total->top_ips[i], ip_list_global[i], IP_LEN - 1); // Copiar IP
        total->top_ips[i][IP_LEN - 1] = '\0'; // Terminar string
        total->top_ips_counts[i] = ip_count_global[i]; // Atribuir contagem
    }
}

/**
 * @brief Imprime relatório final com estatísticas
 * 
 * @param total Resultado global acumulado
 * @param modo Modo de análise utilizado
 */
static void imprimir_relatorio(const WorkerResult *total, char *modo) { // Função para imprimir relatório
    printf("\n=== RELATORIO FINAL (%s) ===\n", modo); // Imprimir cabeçalho
    printf("Total de linhas  : %ld\n", total->total_lines); // Imprimir total linhas
    printf("DEBUG            : %ld\n", total->count_debug); // Imprimir DEBUG
    printf("INFO             : %ld\n", total->count_info); // Imprimir INFO
    printf("WARNINGS         : %ld\n", total->count_warn); // Imprimir WARNINGS
    printf("ERRORS           : %ld\n", total->count_error); // Imprimir ERRORS
    printf("CRITICAL         : %ld\n", total->count_critical); // Imprimir CRITICAL
    printf("HTTP 4xx         : %ld\n", total->count_4xx); // Imprimir HTTP 4xx
    printf("HTTP 5xx         : %ld\n", total->count_5xx); // Imprimir HTTP 5xx
    printf("\n--- TOP 10 IPs ---\n"); // Imprimir título
    for (int i = 0; i < 10; i++) { // Ciclo para top 10
        if (total->top_ips[i][0] == '\0' || total->top_ips_counts[i] <= 0) break; // Se vazio, parar
        printf("%2d. %s (%ld pedidos)\n", i + 1, total->top_ips[i], total->top_ips_counts[i]); // Imprimir IP
    }

    printf("\n--- ALERTAS CRITICOS ---\n"); // Imprimir título
    if (total->num_alerts == 0) { // Se sem alertas
        printf("Sem alertas criticos.\n"); // Imprimir mensagem
    } else { // Se tem alertas
        for (int i = 0; i < total->num_alerts; i++) { // Ciclo para cada alerta
            printf("%2d. %s\n", i + 1, total->alerts[i]); // Imprimir alerta
        }
    }
    printf("=================================\n"); // Imprimir rodapé
}

/**
 * @brief Função principal do processo PAI
 * 
 * @param argc Número de argumentos
 * @param argv Argumentos: programa, diretório, num_processos, modo, [--verbose]
 * @return 0 em sucesso, 1 em erro
 */
int main(int argc, char *argv[]) { // Função principal
    if (argc < 4) { // Se argumentos insuficientes
        posix_writef(STDOUT_FILENO, "Uso: %s <diretorio> <num_processos> <modo> [--verbose]\n", argv[0]); // Imprimir uso
        exit(1); // Sair com erro
    }

    char *diretorio     = argv[1]; // Obter diretório
    int   num_processos = converter_num_processos(argv[2]); // Converter número de processos
    char *modo          = argv[3]; // Obter modo

    int verbose = 0; // Inicializar verbose
    for (int i = 4; i < argc; i++) { // Ciclo para argumentos
        if (strcmp(argv[i], "--verbose") == 0) verbose = 1; // Se verbose, marcar
    }

    if (parser_set_mode_from_string(modo) != 0) { // Se erro ao definir modo
        posix_writef(STDERR_FILENO, "Modo invalido: %s\n", modo); // Imprimir erro
        exit(1); // Sair com erro
    }

    int   capacidade      = 10; // Inicializar capacidade
    int   total_ficheiros = 0; // Inicializar total
    char **ficheiros      = malloc((size_t)capacidade * sizeof(char *)); // Alocar array

    DIR *dir = opendir(diretorio); // Abrir diretório
    if (dir == NULL) { perror("opendir"); exit(1); } // Se erro, sair

    struct dirent *entrada; // Declarar entrada de diretório
    while ((entrada = readdir(dir)) != NULL) { // Ciclo enquanto há entradas
        char *nome = entrada->d_name; // Obter nome
        size_t len = strlen(nome); // Obter comprimento

        if (!((len > 4 && strcmp(nome + len - 4, ".log") == 0) || // Se not .log
              (len > 5 && strcmp(nome + len - 5, ".json") == 0))) { // Se not .json
            continue; // Continuar
        }

        if (total_ficheiros == capacidade) { // Se capacidade atingida
            capacidade *= 2; // Duplicar capacidade
            ficheiros = realloc(ficheiros, (size_t)capacidade * sizeof(char *)); // Realocar
        }

        char caminho[512]; // Declarar buffer para caminho
        snprintf(caminho, sizeof(caminho), "%s/%s", diretorio, nome); // Construir caminho
        ficheiros[total_ficheiros++] = strdup(caminho); // Guardar cópia
    }
    closedir(dir); // Fechar diretório

    if (total_ficheiros == 0) { // Se sem ficheiros
        posix_writef(STDOUT_FILENO, "Nenhum ficheiro encontrado.\n"); // Imprimir mensagem
        libertar_ficheiros(ficheiros, total_ficheiros); // Libertar memória
        exit(0); // Sair com sucesso
    }

    posix_writef(STDOUT_FILENO, "A calcular dimensao total...\n"); // Imprimir mensagem
    off_t total_bytes = obter_bytes_totais(ficheiros, total_ficheiros); // Calcular total
    posix_writef(STDOUT_FILENO, "Total de bytes encontrados: %lld\n\n", (long long)total_bytes); // Imprimir total

    off_t bytes_por_worker = total_bytes / num_processos; // Calcular bytes por worker

    WorkerConfig *configs = malloc((size_t)num_processos * sizeof(WorkerConfig)); // Alocar configs
    for (int i = 0; i < num_processos; i++) { // Ciclo para cada worker
        configs[i].worker_index        = i; // Atribuir índice
        configs[i].byte_inicio         = (off_t)i * bytes_por_worker; // Atribuir início
        configs[i].byte_fim            = (i == num_processos - 1) ? total_bytes // Atribuir fim
                                         : configs[i].byte_inicio + bytes_por_worker;
        configs[i].total_bytes_globais = total_bytes; // Atribuir total global
    }

    pid_t *pids          = malloc((size_t)num_processos * sizeof(pid_t)); // Alocar PIDs
    int   *pipes_leitura = malloc((size_t)num_processos * sizeof(int)); // Alocar pipes
    for (int i = 0; i < num_processos; i++) pipes_leitura[i] = -1; // Inicializar pipes

    fflush(NULL); // Esvaziar todos os buffers

    time_t t_inicio = time(NULL); // Obter tempo inicial

    for (int i = 0; i < num_processos; i++) { // Ciclo para cada worker
        int fd[2]; // Declarar array de file descriptors

        if (pipe(fd) == -1) { perror("pipe"); exit(1); } // Se erro ao criar pipe

        pid_t pid = fork(); // Criar processo filho
        if (pid < 0) { perror("fork"); exit(1); } // Se erro no fork

        if (pid == 0) { // Se processo filho

            close(fd[0]); // Fechar leitura

            for (int j = 0; j < i; j++) { // Ciclo para pipes de irmãos
                if (pipes_leitura[j] != -1) close(pipes_leitura[j]); // Fechar pipe
            }

            run_worker_pipe(ficheiros, total_ficheiros, fd[1], i, // Executar worker
                            configs[i].byte_inicio, configs[i].byte_fim, verbose);
            exit(0); // Sair
        }

        close(fd[1]); // Fechar escrita no pai
        pids[i]          = pid; // Guardar PID
        pipes_leitura[i] = fd[0]; // Guardar pipe
    }

    ProgressUpdate *progressos = calloc(num_processos, sizeof(ProgressUpdate)); // Alocar progressos
    for (int i = 0; i < num_processos; i++) { // Ciclo para cada worker
        progressos[i].worker_index = i; // Atribuir índice
        printf("Worker %2d [....................] -- Aguardar...\n", i); // Imprimir linha
    }
    fflush(stdout); // Forçar escrita

    WorkerResult total = {0}; // Inicializar total
    int resultados = 0; // Inicializar contador

    char ip_list_global[256][IP_LEN]; // Declarar lista global de IPs
    long ip_count_global[256] = {0}; // Declarar contagens globais
    int  ip_num_global        = 0; // Inicializar número de IPs

    while (resultados < num_processos) { // Ciclo enquanto resultados pendentes
        fd_set readfds; // Declarar conjunto de file descriptors
        FD_ZERO(&readfds); // Limpar conjunto
        int max_fd = -1; // Inicializar max fd

        for (int i = 0; i < num_processos; i++) { // Ciclo para cada pipe
            if (pipes_leitura[i] != -1) { // Se pipe ativo
                FD_SET(pipes_leitura[i], &readfds); // Adicionar ao conjunto
                if (pipes_leitura[i] > max_fd) max_fd = pipes_leitura[i]; // Atualizar max
            }
        }

        struct timeval tv; // Declarar timeout
        tv.tv_sec  = 1; // Atribuir segundos
        tv.tv_usec = 0; // Atribuir microsegundos

        int activity = select(max_fd + 1, &readfds, NULL, NULL, &tv); // Esperar por activity
        if (activity < 0) { perror("select"); exit(1); } // Se erro no select
        if (activity == 0) continue; // Se timeout, continuar

        for (int i = 0; i < num_processos; i++) { // Ciclo para cada pipe
            if (pipes_leitura[i] != -1 && FD_ISSET(pipes_leitura[i], &readfds)) { // Se pipe pronto
                int tipo; // Declarar tipo
                ssize_t lidos = read(pipes_leitura[i], &tipo, sizeof(tipo)); // Ler tipo

                if (lidos <= 0) { // Se EOF ou erro
                    close(pipes_leitura[i]); // Fechar pipe
                    pipes_leitura[i] = -1; // Marcar como inativo
                    resultados++; // Incrementar contador
                    continue; // Continuar
                }

                if (tipo == MSG_PROGRESSO) { // Se mensagem de progresso
                    ProgressUpdate pu; // Declarar progresso
                    read(pipes_leitura[i], &pu, sizeof(pu)); // Ler progresso
                    progressos[pu.worker_index] = pu; // Atualizar progresso
                    desenhar_dashboard(progressos, num_processos); // Desenhar dashboard

                } else if (tipo == MSG_RESULTADO) { // Se mensagem de resultado
                    WorkerResult r; // Declarar resultado
                    read(pipes_leitura[i], &r, sizeof(r)); // Ler resultado
                    acumular_resultado(&total, &r, (char (*)[IP_LEN])ip_list_global, ip_count_global, &ip_num_global); // Acumular

                    progressos[i].bytes_done = progressos[i].bytes_total; // Marcar 100%
                    desenhar_dashboard(progressos, num_processos); // Desenhar dashboard

                    close(pipes_leitura[i]); // Fechar pipe
                    pipes_leitura[i] = -1; // Marcar como inativo
                    resultados++; // Incrementar contador
                }
            }
        }
    }

    for (int i = 0; i < num_processos; i++) { // Ciclo para cada worker
        int status; // Declarar status
        waitpid(pids[i], &status, 0); // Esperar pelo worker
    }

    long elapsed = (long)(time(NULL) - t_inicio); // Calcular tempo

    imprimir_relatorio(&total, modo); // Imprimir relatório
    printf("Tempo de processamento: %ldmin %02lds\n", elapsed / 60, elapsed % 60); // Imprimir tempo

    free(progressos); // Libertar progressos
    free(pipes_leitura); // Libertar pipes
    free(pids); // Libertar PIDs
    free(configs); // Libertar configs
    libertar_ficheiros(ficheiros, total_ficheiros); // Libertar ficheiros

    return 0; // Retornar sucesso
}
