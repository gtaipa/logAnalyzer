/**
 * @file main_sockets.c
 * @brief Processo PAI - Arquitectura Multi-Processo com Sockets Unix Domain
 * @author Implementação do Analisador de Logs
 * @version 1.0
 * @date 2026
 * 
 * @details
 * Implementação da Fase 1A (15% do projeto) com sockets Unix Domain.
 * Processo pai que atua como servidor, aceita conexões de N workers,
 * envia configuração para cada um, lê progresso/resultado.
 * 
 * FLUXO:
 *  1. Cria socket Unix Domain e binds a SOCKET_PATH
 *  2. Escuta por conexões (listen backlog = 64)
 *  3. Forks N workers, cada um conecta de volta
 *  4. Para cada conexão, envia MSG_CONFIG com intervalo de bytes
 *  5. Usa select() para multiplexagem assíncrona
 *  6. Lê MSG_PROGRESSO de cada worker, desenha dashboard
 *  7. Lê MSG_RESULTADO final e acumula métricas
 *  8. Aguarda workers, imprime relatório, remove socket
 * 
 * COMUNICAÇÃO:
 *  - Bidirecional: pai envia config, filho envia progresso/resultado
 *  - MSG_CONFIG: int(tipo) + WorkerConfig struct
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
#include <sys/wait.h> // Incluir header com funções de espera
#include <sys/select.h> // Incluir header com select
#include <sys/socket.h> // Incluir header com sockets
#include <sys/un.h> // Incluir header com sockets Unix
#include <time.h> // Incluir header com funções de tempo
#include <unistd.h> // Incluir header com funções POSIX

#include "ipc.h" // Incluir header de IPC
#include "parser.h" // Incluir header do parser
#include "worker.h" // Incluir header do worker

#define MSG_PROGRESSO 1 // Definir tipo de mensagem progresso
#define MSG_RESULTADO 2 // Definir tipo de mensagem resultado

#define LARGURA_BARRA 20 // Definir largura da barra

/**
 * @brief Desenha dashboard com barras de progresso de todos os workers
 * 
 * @param progressos Array com atualizações de progresso
 * @param num_workers Número de workers
 */
static void desenhar_dashboard(ProgressUpdate *progressos, int num_workers) { // Função para desenhar dashboard
    printf("\033[%dA", num_workers); // Subir cursor
    printf("\033[J"); // Apagar linhas

    for (int i = 0; i < num_workers; i++) { // Ciclo para cada worker
        long feitas = progressos[i].bytes_done; // Obter bytes feitos
        long total  = progressos[i].bytes_total; // Obter total de bytes

        int pct = (total > 0) ? (int)(feitas * 100 / total) : 0; // Calcular percentagem
        if (pct > 100) pct = 100; // Limitar percentagem

        char barra[LARGURA_BARRA + 1]; // Declarar array para barra
        int cheio = pct * LARGURA_BARRA / 100; // Calcular parte cheia
        for (int b = 0; b < LARGURA_BARRA; b++) // Ciclo para barra
            barra[b] = (b < cheio) ? '#' : '.'; // Desenhar barra
        barra[LARGURA_BARRA] = '\0'; // Terminar string

        printf("Worker %2d [%s] %3d%% (%ld/%ld bytes)\n", // Imprimir linha
               i, barra, pct, feitas, total);
    }

    fflush(stdout); // Forçar flush
}

/**
 * @brief Liberta memória alocada para array de ficheiros
 * 
 * @param ficheiros Array de ponteiros para strings
 * @param total Número de strings
 */
static void libertar_ficheiros(char **ficheiros, int total) { // Função para libertar memória
    for (int i = 0; i < total; i++) free(ficheiros[i]); // Libertar cada string
    free(ficheiros); // Libertar array
}

/**
 * @brief Calcula total de bytes em todos os ficheiros
 * 
 * @param ficheiros Array de caminhos
 * @param total_ficheiros Número de ficheiros
 * @return Total de bytes
 */
static off_t obter_bytes_totais(char **ficheiros, int total_ficheiros) { // Função para calcular bytes
    off_t total = 0; // Inicializar total
    struct stat st; // Declarar estrutura
    for (int i = 0; i < total_ficheiros; i++) { // Ciclo para cada ficheiro
        if (stat(ficheiros[i], &st) == 0) // Se conseguir obter stats
            total += st.st_size; // Adicionar tamanho
    }
    return total; // Retornar total
}

/**
 * @brief Lê diretório e retorna array de ficheiros .log/.json
 * 
 * @param dir Caminho do diretório
 * @param total_out Ponteiro para variável onde guardar número de ficheiros
 * @return Array de caminhos (alocado dinamicamente)
 */
static char **ler_directorio(const char *dir, int *total_out) { // Função para ler diretório
    int capacidade = 10; // Inicializar capacidade
    int total = 0; // Inicializar total
    char **ficheiros = malloc(capacidade * sizeof(char *)); // Alocar array
    if (!ficheiros) { perror("malloc"); exit(1); } // Se erro, sair

    DIR *d = opendir(dir); // Abrir diretório
    if (!d) { perror("opendir"); exit(1); } // Se erro, sair

    struct dirent *entrada; // Declarar entrada
    while ((entrada = readdir(d)) != NULL) { // Ciclo enquanto há entradas
        char *nome = entrada->d_name; // Obter nome
        int len = strlen(nome); // Obter comprimento

        int e_log  = (len > 4 && strcmp(nome + len - 4, ".log")  == 0); // Verificar .log
        int e_json = (len > 5 && strcmp(nome + len - 5, ".json") == 0); // Verificar .json
        if (!e_log && !e_json) continue; // Se nem log nem json, continuar

        if (total == capacidade) { // Se capacidade atingida
            capacidade *= 2; // Duplicar capacidade
            ficheiros = realloc(ficheiros, capacidade * sizeof(char *)); // Realocar
            if (!ficheiros) { perror("realloc"); exit(1); } // Se erro, sair
        }

        char caminho[512]; // Declarar caminho
        snprintf(caminho, sizeof(caminho), "%s/%s", dir, nome); // Construir caminho
        ficheiros[total++] = strdup(caminho); // Guardar cópia
    }

    closedir(d); // Fechar diretório
    *total_out = total; // Atribuir total
    return ficheiros; // Retornar array
}

/**
 * @brief Acumula resultado de um worker na tabela global
 * 
 * @details
 * Atualiza métricas globais, faz merge de IPs, reordena por frequência.
 * 
 * @param total Resultado global acumulado
 * @param r Resultado de um worker
 * @param ip_list_global Tabela global de IPs
 * @param ip_count_global Tabela global de contagens
 * @param ip_num_global Ponteiro para número de IPs na tabela
 */
static void acumular(WorkerResult *total, WorkerResult *r, // Função para acumular
                     char ip_list_global[256][IP_LEN], long ip_count_global[256], int *ip_num_global) {
    total->total_lines    += r->total_lines; // Acumular linhas
    total->count_debug    += r->count_debug; // Acumular debug
    total->count_info     += r->count_info; // Acumular info
    total->count_warn     += r->count_warn; // Acumular warn
    total->count_error    += r->count_error; // Acumular error
    total->count_critical += r->count_critical; // Acumular critical
    total->count_4xx      += r->count_4xx; // Acumular 4xx
    total->count_5xx      += r->count_5xx; // Acumular 5xx

    for (int k = 0; k < 10; k++) { // Ciclo para top 10
        if (r->top_ips[k][0] == '\0' || r->top_ips_counts[k] <= 0) continue; // Se vazio, continuar

        int found = -1; // Inicializar procura
        for (int i = 0; i < *ip_num_global; i++) { // Ciclo na tabela
            if (strcmp(ip_list_global[i], r->top_ips[k]) == 0) { // Se encontrado
                found = i; // Guardar índice
                break; // Sair
            }
        }
        
        if (found == -1 && *ip_num_global < 256) { // Se novo e espaço
            strncpy(ip_list_global[*ip_num_global], r->top_ips[k], IP_LEN - 1); // Copiar IP
            ip_list_global[*ip_num_global][IP_LEN - 1] = '\0'; // Terminar string
            ip_count_global[*ip_num_global] = r->top_ips_counts[k]; // Atribuir contagem
            (*ip_num_global)++; // Incrementar
        } else if (found >= 0) { // Se existe
            ip_count_global[found] += r->top_ips_counts[k]; // Acumular contagem
        }
    }

    for (int i = 0; i < r->num_alerts && total->num_alerts < MAX_ALERTS; i++) { // Ciclo alertas
        strncpy(total->alerts[total->num_alerts], r->alerts[i], ALERT_LEN - 1); // Copiar
        total->alerts[total->num_alerts][ALERT_LEN - 1] = '\0'; // Terminar
        total->num_alerts++; // Incrementar
    }
    
    for (int i = 0; i < *ip_num_global - 1; i++) { // Ciclo externo bubble sort
        for (int j = 0; j < *ip_num_global - i - 1; j++) { // Ciclo interno
            if (ip_count_global[j] < ip_count_global[j + 1]) { // Se menor
                long tmp_count = ip_count_global[j]; // Guardar temp
                ip_count_global[j] = ip_count_global[j + 1]; // Mover maior
                ip_count_global[j + 1] = tmp_count; // Colocar menor

                char tmp_ip[IP_LEN]; // Declarar IP temp
                strncpy(tmp_ip, ip_list_global[j], IP_LEN); // Guardar IP
                strncpy(ip_list_global[j], ip_list_global[j + 1], IP_LEN); // Mover IP
                strncpy(ip_list_global[j + 1], tmp_ip, IP_LEN); // Colocar IP
            }
        }
    }

    memset(total->top_ips, 0, sizeof(total->top_ips)); // Limpar top IPs
    memset(total->top_ips_counts, 0, sizeof(total->top_ips_counts)); // Limpar contagens
    int limite = *ip_num_global < 10 ? *ip_num_global : 10; // Definir limite
    for (int i = 0; i < limite; i++) { // Ciclo para top IPs
        strncpy(total->top_ips[i], ip_list_global[i], IP_LEN - 1); // Copiar IP
        total->top_ips[i][IP_LEN - 1] = '\0'; // Terminar string
        total->top_ips_counts[i] = ip_count_global[i]; // Atribuir contagem
    }
}

/**
 * @brief Imprime relatório final com estatísticas agregadas
 * 
 * @param total Resultado global acumulado
 * @param modo Modo de análise utilizado
 */
static void imprimir_relatorio(WorkerResult *total, char *modo) { // Função para imprimir relatório
    printf("\n=== RELATORIO FINAL (%s) ===\n", modo); // Imprimir cabeçalho
    printf("Total de linhas  : %ld\n", total->total_lines); // Imprimir linhas
    printf("DEBUG            : %ld\n", total->count_debug); // Imprimir debug
    printf("INFO             : %ld\n", total->count_info); // Imprimir info
    printf("WARNINGS         : %ld\n", total->count_warn); // Imprimir warn
    printf("ERRORS           : %ld\n", total->count_error); // Imprimir error
    printf("CRITICAL         : %ld\n", total->count_critical); // Imprimir critical
    printf("HTTP 4xx         : %ld\n", total->count_4xx); // Imprimir 4xx
    printf("HTTP 5xx         : %ld\n", total->count_5xx); // Imprimir 5xx
    printf("\n--- TOP 10 IPs ---\n"); // Imprimir título
    for (int i = 0; i < 10; i++) { // Ciclo para top 10
        if (total->top_ips[i][0] == '\0' || total->top_ips_counts[i] <= 0) break; // Se vazio, parar
        printf("%2d. %s (%ld pedidos)\n", i + 1, total->top_ips[i], total->top_ips_counts[i]); // Imprimir IP
    }

    printf("\n--- ALERTAS CRITICOS ---\n"); // Imprimir título
    if (total->num_alerts == 0) { // Se sem alertas
        printf("Sem alertas criticos.\n"); // Imprimir mensagem
    } else { // Se tem alertas
        for (int i = 0; i < total->num_alerts; i++) { // Ciclo alertas
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
        printf("Uso: %s <directorio> <num_processos> <modo> [--verbose]\n", argv[0]); // Imprimir uso
        exit(1); // Sair
    }

    char *dir          = argv[1]; // Obter diretório
    int   num_procs    = atoi(argv[2]); // Converter processos
    char *modo         = argv[3]; // Obter modo
    int   verbose      = (argc > 4 && strcmp(argv[4], "--verbose") == 0); // Verificar verbose

    if (parser_set_mode_from_string(modo) != 0) { // Se erro no modo
        fprintf(stderr, "Modo invalido: %s\n", modo); // Imprimir erro
        exit(1); // Sair
    }

    int total_ficheiros = 0; // Inicializar total
    char **ficheiros = ler_directorio(dir, &total_ficheiros); // Ler diretório

    if (total_ficheiros == 0) { // Se sem ficheiros
        printf("Nenhum ficheiro .log ou .json encontrado em: %s\n", dir); // Imprimir mensagem
        free(ficheiros); // Libertar memória
        exit(0); // Sair
    }

    if (num_procs > total_ficheiros) // Se mais processos que ficheiros
        num_procs = total_ficheiros; // Limitar ao número de ficheiros

    printf("Ficheiros encontrados: %d | Workers: %d | Modo: %s\n\n", // Imprimir resumo
           total_ficheiros, num_procs, modo);

    printf("A calcular dimensao total...\n"); // Imprimir mensagem
    off_t total_bytes = obter_bytes_totais(ficheiros, total_ficheiros); // Calcular bytes
    printf("Total de bytes encontrados: %lld\n\n", (long long)total_bytes); // Imprimir total

    WorkerConfig *configs = malloc(num_procs * sizeof(WorkerConfig)); // Alocar configs
    if (!configs) { perror("malloc"); exit(1); } // Se erro, sair

    off_t bytes_por_worker = total_bytes / num_procs; // Calcular bytes por worker
    for (int i = 0; i < num_procs; i++) { // Ciclo para cada worker
        configs[i].worker_index        = i; // Atribuir índice
        configs[i].byte_inicio         = (off_t)i * bytes_por_worker; // Atribuir início
        configs[i].byte_fim            = (i == num_procs - 1) ? total_bytes // Atribuir fim
                                         : configs[i].byte_inicio + bytes_por_worker;
        configs[i].total_bytes_globais = total_bytes; // Atribuir total
    }

    unlink(SOCKET_PATH); // Remover socket antigo

    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0); // Criar socket
    if (server_fd < 0) { perror("socket"); exit(1); } // Se erro, sair

    struct sockaddr_un addr; // Declarar endereço
    memset(&addr, 0, sizeof(addr)); // Limpar endereço
    addr.sun_family = AF_UNIX; // Atribuir família
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1); // Atribuir path

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { // Se erro no bind
        perror("bind"); // Imprimir erro
        exit(1); // Sair
    }

    if (listen(server_fd, 64) < 0) { // Se erro no listen
        perror("listen"); // Imprimir erro
        exit(1); // Sair
    }

    pid_t *pids = malloc(num_procs * sizeof(pid_t)); // Alocar PIDs

    fflush(NULL); // Esvaziar buffers

    time_t t_inicio = time(NULL); // Obter tempo inicial

    for (int i = 0; i < num_procs; i++) { // Ciclo para criar workers
        pid_t pid = fork(); // Criar processo
        if (pid < 0) { perror("fork"); exit(1); } // Se erro, sair

        if (pid == 0) { // Se processo filho
            if (close(server_fd) == -1) { // Se erro ao fechar
                perror("close"); // Imprimir erro
                exit(1); // Sair
            }

            run_worker(ficheiros, total_ficheiros, num_procs, i, verbose); // Executar worker
            exit(0); // Sair
        }

        pids[i] = pid; // Guardar PID
    }

    int *client_fds = malloc(num_procs * sizeof(int)); // Alocar client fds
    for (int i = 0; i < num_procs; i++) client_fds[i] = -1; // Inicializar
    
    int num_conectados = 0; // Inicializar contador
    
    while (num_conectados < num_procs) { // Ciclo enquanto há conexões pendentes
        fd_set readfds; // Declarar conjunto
        FD_ZERO(&readfds); // Limpar conjunto
        FD_SET(server_fd, &readfds); // Adicionar server fd
        
        struct timeval tv; // Declarar timeout
        tv.tv_sec = 5; // Atribuir segundos
        tv.tv_usec = 0; // Atribuir microsegundos
        
        int activity = select(server_fd + 1, &readfds, NULL, NULL, &tv); // Esperar atividade
        if (activity < 0) { perror("select"); exit(1); } // Se erro, sair
        
        if (activity == 0) { // Se timeout
            fprintf(stderr, "Timeout: aguardando conexão de worker\n"); // Imprimir mensagem
            continue; // Continuar
        }
        
        if (FD_ISSET(server_fd, &readfds)) { // Se server fd pronto
            int client_fd = accept(server_fd, NULL, NULL); // Aceitar conexão
            if (client_fd < 0) { perror("accept"); exit(1); } // Se erro, sair
            
            int idx = -1; // Inicializar índice
            for (int i = 0; i < num_procs; i++) { // Ciclo para encontrar slot
                if (client_fds[i] == -1) { // Se slot vazio
                    idx = i; // Guardar índice
                    break; // Sair
                }
            }
            
            if (idx >= 0) { // Se encontrou slot
                client_fds[idx] = client_fd; // Guardar fd
                
                int tipo = MSG_CONFIG; // Atribuir tipo
                write(client_fds[idx], &tipo, sizeof(tipo)); // Escrever tipo
                write(client_fds[idx], &configs[idx], sizeof(WorkerConfig)); // Escrever config
                
                num_conectados++; // Incrementar
                if (verbose) // Se verbose
                    printf("Worker %d conectado (total: %d/%d)\n", idx, num_conectados, num_procs); // Imprimir
            }
        }
    }

    ProgressUpdate *progressos = calloc(num_procs, sizeof(ProgressUpdate)); // Alocar progressos
    for (int i = 0; i < num_procs; i++) { // Ciclo para inicializar
        progressos[i].worker_index = i; // Atribuir índice
        printf("Worker %2d [....................] -- Aguardar...\n", i); // Imprimir
    }
    fflush(stdout); // Forçar flush

    char ip_list_global[256][IP_LEN]; // Declarar lista global
    long ip_count_global[256] = {0}; // Declarar contagens
    int ip_num_global = 0; // Inicializar
    
    WorkerResult total = {0}; // Inicializar total
    int resultados = 0; // Inicializar contador

    while (resultados < num_procs) { // Ciclo enquanto há resultados pendentes
        fd_set readfds; // Declarar conjunto
        FD_ZERO(&readfds); // Limpar conjunto
        
        int max_fd = -1; // Inicializar max fd
        for (int i = 0; i < num_procs; i++) { // Ciclo para adicionar fds
            if (client_fds[i] != -1) { // Se fd ativo
                FD_SET(client_fds[i], &readfds); // Adicionar ao conjunto
                if (client_fds[i] > max_fd) max_fd = client_fds[i]; // Atualizar max
            }
        }
        
        struct timeval tv; // Declarar timeout
        tv.tv_sec = 1; // Atribuir segundos
        tv.tv_usec = 0; // Atribuir microsegundos
        
        int activity = select(max_fd + 1, &readfds, NULL, NULL, &tv); // Esperar atividade
        if (activity < 0) { perror("select"); exit(1); } // Se erro, sair
        
        if (activity == 0) continue; // Se timeout, continuar
        
        for (int i = 0; i < num_procs; i++) { // Ciclo para cada fd
            if (client_fds[i] == -1 || !FD_ISSET(client_fds[i], &readfds)) // Se fd não pronto
                continue; // Continuar

            int tipo; // Declarar tipo
            ssize_t lidos = read(client_fds[i], &tipo, sizeof(tipo)); // Ler tipo
            if (lidos <= 0) { // Se EOF ou erro
                fprintf(stderr, "Worker %d: ligação fechada inesperadamente\n", i); // Imprimir erro
                close(client_fds[i]); // Fechar fd
                client_fds[i] = -1; // Marcar inativo
                resultados++; // Incrementar
                continue; // Continuar
            }

            if (tipo == MSG_PROGRESSO) { // Se mensagem progresso
                ProgressUpdate pu; // Declarar progresso
                read(client_fds[i], &pu, sizeof(pu)); // Ler progresso
                progressos[pu.worker_index] = pu; // Atualizar
                desenhar_dashboard(progressos, num_procs); // Desenhar

            } else if (tipo == MSG_RESULTADO) { // Se mensagem resultado
                WorkerResult r; // Declarar resultado
                read(client_fds[i], &r, sizeof(r)); // Ler resultado
                acumular(&total, &r, (char (*)[IP_LEN])ip_list_global, ip_count_global, &ip_num_global); // Acumular

                progressos[i].bytes_done = progressos[i].bytes_total; // Marcar 100%
                desenhar_dashboard(progressos, num_procs); // Desenhar

                close(client_fds[i]); // Fechar fd
                client_fds[i] = -1; // Marcar inativo
                resultados++; // Incrementar
            }
        }
    }

    close(server_fd); // Fechar servidor
    unlink(SOCKET_PATH); // Remover socket

    for (int i = 0; i < num_procs; i++) { // Ciclo para cada worker
        int status; // Declarar status
        waitpid(pids[i], &status, 0); // Esperar worker
        if (WIFEXITED(status) && WEXITSTATUS(status) != 0) // Se terminou com erro
            fprintf(stderr, "Worker %d terminou com erro %d\n", i, WEXITSTATUS(status)); // Imprimir erro
    }

    long elapsed = (long)(time(NULL) - t_inicio); // Calcular tempo

    imprimir_relatorio(&total, modo); // Imprimir relatório
    printf("Tempo de processamento: %ldmin %02lds\n", elapsed / 60, elapsed % 60); // Imprimir tempo

    free(progressos); // Libertar progressos
    free(client_fds); // Libertar client fds
    free(pids); // Libertar PIDs
    free(configs); // Libertar configs
    libertar_ficheiros(ficheiros, total_ficheiros); // Libertar ficheiros

    return 0; // Retornar sucesso
}
