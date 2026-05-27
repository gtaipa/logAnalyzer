#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <unistd.h>

#include "ipc.h"
#include "parser.h"
#include "posix_io.h"
#include "worker.h"

#define MSG_PROGRESSO 1
#define MSG_RESULTADO 2
#define LARGURA_BARRA 20
#define BUF_SIZE 4096

void run_worker_pipe(char **ficheiros, int total_ficheiros, int pipe_fd_write,
                     int worker_index, off_t byte_inicio, off_t byte_fim, int verbose);

static void desenhar_dashboard(ProgressUpdate *progressos, int num_workers) {
    printf("\033[%dA", num_workers);
    printf("\033[J");

    for (int i = 0; i < num_workers; i++) {
        long feitas = progressos[i].bytes_done;
        long total  = progressos[i].bytes_total;

        int pct = (total > 0) ? (int)(feitas * 100 / total) : 0;
        if (pct > 100) pct = 100;

        char barra[LARGURA_BARRA + 1];
        int cheio = pct * LARGURA_BARRA / 100;
        for (int b = 0; b < LARGURA_BARRA; b++)
            barra[b] = (b < cheio) ? '#' : '.';
        barra[LARGURA_BARRA] = '\0';

        printf("Worker %2d [%s] %3d%% (%ld/%ld bytes)\n",
               i, barra, pct, feitas, total);
    }
    fflush(stdout);
}

static void libertar_ficheiros(char **ficheiros, int total_ficheiros) {
    if (ficheiros == NULL) return;
    for (int i = 0; i < total_ficheiros; i++) free(ficheiros[i]);
    free(ficheiros);
}

static off_t obter_bytes_totais(char **ficheiros, int total_ficheiros) {
    off_t total = 0;
    struct stat st;
    for (int i = 0; i < total_ficheiros; i++) {
        if (stat(ficheiros[i], &st) == 0)
            total += st.st_size;
    }
    return total;
}

static int converter_num_processos(const char *texto) {
    char *fim = NULL;
    errno = 0;
    long valor = strtol(texto, &fim, 10);
    if (errno != 0 || fim == texto || *fim != '\0' || valor <= 0) {
        posix_writef(STDERR_FILENO, "Numero de processos invalido: %s\n", texto);
        exit(1);
    }
    return (int)valor;
}

static void acumular_resultado(WorkerResult *total, const WorkerResult *r, 
                               char ip_list_global[256][IP_LEN], long ip_count_global[256], int *ip_num_global) {
    total->total_lines += r->total_lines;
    total->count_debug += r->count_debug;
    total->count_info += r->count_info;
    total->count_warn += r->count_warn;
    total->count_error += r->count_error;
    total->count_critical += r->count_critical;
    total->count_4xx += r->count_4xx;
    total->count_5xx += r->count_5xx;

    for (int k = 0; k < 10; k++) {
        if (r->top_ips[k][0] == '\0' || r->top_ips_counts[k] <= 0) continue;

        int found = -1;
        for (int i = 0; i < *ip_num_global; i++) {
            if (strcmp(ip_list_global[i], r->top_ips[k]) == 0) {
                found = i;
                break;
            }
        }
        if (found == -1 && *ip_num_global < 256) {
            strncpy(ip_list_global[*ip_num_global], r->top_ips[k], IP_LEN - 1);
            ip_list_global[*ip_num_global][IP_LEN - 1] = '\0';
            ip_count_global[*ip_num_global] = r->top_ips_counts[k];
            (*ip_num_global)++;
        } else if (found >= 0) {
            ip_count_global[found] += r->top_ips_counts[k];
        }
    }

    for (int i = 0; i < r->num_alerts && total->num_alerts < MAX_ALERTS; i++) {
        strncpy(total->alerts[total->num_alerts], r->alerts[i], ALERT_LEN - 1);
        total->alerts[total->num_alerts][ALERT_LEN - 1] = '\0';
        total->num_alerts++;
    }
    
    for (int i = 0; i < *ip_num_global - 1; i++) {
        for (int j = 0; j < *ip_num_global - i - 1; j++) {
            if (ip_count_global[j] < ip_count_global[j + 1]) {
                long tmp_count = ip_count_global[j];
                ip_count_global[j] = ip_count_global[j + 1];
                ip_count_global[j + 1] = tmp_count;

                char tmp_ip[IP_LEN];
                strncpy(tmp_ip, ip_list_global[j], IP_LEN);
                strncpy(ip_list_global[j], ip_list_global[j + 1], IP_LEN);
                strncpy(ip_list_global[j + 1], tmp_ip, IP_LEN);
            }
        }
    }

    memset(total->top_ips, 0, sizeof(total->top_ips));
    memset(total->top_ips_counts, 0, sizeof(total->top_ips_counts));
    int limite = *ip_num_global < 10 ? *ip_num_global : 10;
    for (int i = 0; i < limite; i++) {
        strncpy(total->top_ips[i], ip_list_global[i], IP_LEN - 1);
        total->top_ips[i][IP_LEN - 1] = '\0';
        total->top_ips_counts[i] = ip_count_global[i];
    }
}

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

int main(int argc, char *argv[]) {
    if (argc < 4) {
        posix_writef(STDOUT_FILENO, "Uso: %s <diretorio> <num_processos> <modo> [--verbose]\n", argv[0]);
        exit(1);
    }

    char *diretorio = argv[1];
    int num_processos = converter_num_processos(argv[2]);
    char *modo = argv[3];

    int verbose = 0;
    for (int i = 4; i < argc; i++) {
        if (strcmp(argv[i], "--verbose") == 0) verbose = 1;
    }

    if (parser_set_mode_from_string(modo) != 0) {
        posix_writef(STDERR_FILENO, "Modo invalido: %s\n", modo);
        exit(1);
    }

    int capacidade = 10;
    int total_ficheiros = 0;
    char **ficheiros = malloc((size_t)capacidade * sizeof(char *));

    DIR *dir = opendir(diretorio);
    if (dir == NULL) { perror("opendir"); exit(1); }

    struct dirent *entrada;
    while ((entrada = readdir(dir)) != NULL) {
        char *nome = entrada->d_name;
        size_t len = strlen(nome);

        if (!((len > 4 && strcmp(nome + len - 4, ".log") == 0) ||
              (len > 5 && strcmp(nome + len - 5, ".json") == 0))) {
            continue;
        }

        if (total_ficheiros == capacidade) {
            capacidade *= 2;
            ficheiros = realloc(ficheiros, (size_t)capacidade * sizeof(char *));
        }

        char caminho[512];
        snprintf(caminho, sizeof(caminho), "%s/%s", diretorio, nome);
        ficheiros[total_ficheiros++] = strdup(caminho);
    }
    closedir(dir);

    if (total_ficheiros == 0) {
        posix_writef(STDOUT_FILENO, "Nenhum ficheiro encontrado.\n");
        libertar_ficheiros(ficheiros, total_ficheiros);
        exit(0);
    }

    posix_writef(STDOUT_FILENO, "A calcular dimensao total...\n");
    off_t total_bytes = obter_bytes_totais(ficheiros, total_ficheiros);
    posix_writef(STDOUT_FILENO, "Total de bytes encontrados: %lld\n\n", (long long)total_bytes);

    off_t bytes_por_worker = total_bytes / num_processos;

    WorkerConfig *configs = malloc((size_t)num_processos * sizeof(WorkerConfig));
    for (int i = 0; i < num_processos; i++) {
        configs[i].worker_index         = i;
        configs[i].byte_inicio          = (off_t)i * bytes_por_worker;
        configs[i].byte_fim             = (i == num_processos - 1) ? total_bytes
                                          : configs[i].byte_inicio + bytes_por_worker;
        configs[i].total_bytes_globais  = total_bytes;
    }

    pid_t *pids = malloc((size_t)num_processos * sizeof(pid_t));
    int *pipes_leitura = malloc((size_t)num_processos * sizeof(int));
    for (int i = 0; i < num_processos; i++) pipes_leitura[i] = -1;

    fflush(NULL);

    for (int i = 0; i < num_processos; i++) {
        int fd[2];
        if (pipe(fd) == -1) { perror("pipe"); exit(1); }

        pid_t pid = fork();
        if (pid < 0) { perror("fork"); exit(1); }

        if (pid == 0) {
            close(fd[0]);
            for (int j = 0; j < i; j++) {
                if (pipes_leitura[j] != -1) close(pipes_leitura[j]);
            }
            run_worker_pipe(ficheiros, total_ficheiros, fd[1], i,
                           configs[i].byte_inicio, configs[i].byte_fim, verbose);
            exit(0);
        }

        close(fd[1]);
        pids[i] = pid;
        pipes_leitura[i] = fd[0];
    }

    // Preparar o dashboard no terminal
    ProgressUpdate *progressos = calloc(num_processos, sizeof(ProgressUpdate));
    for (int i = 0; i < num_processos; i++) {
        progressos[i].worker_index = i;
        printf("Worker %2d [....................] -- Aguardar...\n", i);
    }
    fflush(stdout);

    WorkerResult total = {0};
    int resultados = 0;
    
    char ip_list_global[256][IP_LEN];
    long ip_count_global[256] = {0};
    int ip_num_global = 0;

    // Select loop (Pai servidor assíncrono para os Pipes)
    while (resultados < num_processos) {
        fd_set readfds;
        FD_ZERO(&readfds);
        int max_fd = -1;
        
        for (int i = 0; i < num_processos; i++) {
            if (pipes_leitura[i] != -1) {
                FD_SET(pipes_leitura[i], &readfds);
                if (pipes_leitura[i] > max_fd) max_fd = pipes_leitura[i];
            }
        }
        
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        
        int activity = select(max_fd + 1, &readfds, NULL, NULL, &tv);
        if (activity < 0) { perror("select"); exit(1); }
        if (activity == 0) continue;
        
        for (int i = 0; i < num_processos; i++) {
            if (pipes_leitura[i] != -1 && FD_ISSET(pipes_leitura[i], &readfds)) {
                int tipo;
                ssize_t lidos = read(pipes_leitura[i], &tipo, sizeof(tipo));
                
                if (lidos <= 0) {
                    close(pipes_leitura[i]);
                    pipes_leitura[i] = -1;
                    resultados++;
                    continue;
                }

                if (tipo == MSG_PROGRESSO) {
                    ProgressUpdate pu;
                    read(pipes_leitura[i], &pu, sizeof(pu));
                    progressos[pu.worker_index] = pu;
                    desenhar_dashboard(progressos, num_processos);

                } else if (tipo == MSG_RESULTADO) {
                    WorkerResult r;
                    read(pipes_leitura[i], &r, sizeof(r));
                    acumular_resultado(&total, &r, (char (*)[IP_LEN])ip_list_global, ip_count_global, &ip_num_global);

                    progressos[i].bytes_done = progressos[i].bytes_total;
                    desenhar_dashboard(progressos, num_processos);

                    close(pipes_leitura[i]);
                    pipes_leitura[i] = -1;
                    resultados++;
                }
            }
        }
    }

    for (int i = 0; i < num_processos; i++) {
        int status;
        waitpid(pids[i], &status, 0);
    }

    imprimir_relatorio(&total, modo);

    free(progressos);
    free(pipes_leitura);
    free(pids);
    free(configs);
    libertar_ficheiros(ficheiros, total_ficheiros);

    return 0;
}
