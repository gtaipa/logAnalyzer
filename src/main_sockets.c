#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <errno.h>

#include "worker.h"
#include "ipc.h"

#define MAX_WORKERS 64

static struct {
    pid_t pid;
    long  lines_done;
    long  lines_total;
} worker_status[MAX_WORKERS];

static int    g_num_workers  = 0;
static time_t g_start_time   = 0;
static long   g_total_errors = 0;

static void draw_dashboard(void) {
    int linhas = g_num_workers + 5;
    printf("\033[%dA", linhas);
    printf("\033[J");

    time_t elapsed = time(NULL) - g_start_time;
    int hh = elapsed / 3600;
    int mm = (elapsed % 3600) / 60;
    int ss = elapsed % 60;

    long total_done  = 0;
    long total_total = 0;
    for (int i = 0; i < g_num_workers; i++) {
        total_done  += worker_status[i].lines_done;
        total_total += worker_status[i].lines_total;
    }
    int total_pct = (total_total > 0)
                    ? (int)(total_done * 100 / total_total) : 0;
    if (total_pct > 100) total_pct = 100;

    printf("╔══════════════════════════════════════════╗\n");
    printf("║       LOG ANALYZER - Real-time Monitor   ║\n");
    printf("╠══════════════════════════════════════════╣\n");

    for (int i = 0; i < g_num_workers; i++) {
        int pct = (worker_status[i].lines_total > 0)
                  ? (int)(worker_status[i].lines_done * 100
                          / worker_status[i].lines_total) : 0;
        if (pct > 100) pct = 100;

        char bar[21];
        int filled = pct / 5;
        for (int b = 0; b < 20; b++) bar[b] = (b < filled) ? '#' : '.';
        bar[20] = '\0';

        printf("║ Worker %-2d [%s] %3d%%           ║\n", i + 1, bar, pct);
    }

    printf("╠══════════════════════════════════════════╣\n");

    char tot_bar[21];
    int tot_filled = total_pct / 5;
    for (int b = 0; b < 20; b++) tot_bar[b] = (b < tot_filled) ? '#' : '.';
    tot_bar[20] = '\0';

    printf("║ Total     [%s] %3d%%           ║\n", tot_bar, total_pct);
    printf("║ Elapsed: %02d:%02d:%02d | Errors: %-6ld      ║\n", hh, mm, ss, g_total_errors);
    printf("╚══════════════════════════════════════════╝\n");
}

static void sigalrm_handler(int sig) {
    (void)sig;
    draw_dashboard();
    alarm(1);
}

#define MSG_PROGRESS 1
#define MSG_RESULT   2

typedef struct {
    int type;   
} MsgHeader;

/* Função auxiliar para gerar o relatório no terminal e/ou ficheiro */
void gerar_relatorio(WorkerResult total, char *modo, char *output_file) {
    FILE *out = stdout; // Por defeito escreve no terminal
    FILE *f = NULL;

    if (output_file != NULL) {
        f = fopen(output_file, "w");
        if (f) {
            out = f;
            printf("\n[INFO] A gravar relatorio no ficheiro: %s\n", output_file);
        } else {
            perror("Erro ao criar ficheiro de output");
        }
    }

    fprintf(out, "\n=== RELATORIO FINAL (%s) ===\n", modo);
    fprintf(out, "Total de linhas : %ld\n", total.total_lines);

    if (strcmp(modo, "security") == 0 || strcmp(modo, "full") == 0) {
        fprintf(out, "\n--- ALERTAS DE SEGURANCA ---\n");
        fprintf(out, "WARNINGS        : %ld\n", total.count_warn);
        fprintf(out, "ERRORS          : %ld\n", total.count_error);
        fprintf(out, "CRITICAL        : %ld\n", total.count_critical);
        fprintf(out, "HTTP 4xx/5xx    : %ld\n", total.count_4xx + total.count_5xx);
    }
    
    if (strcmp(modo, "traffic") == 0 || strcmp(modo, "full") == 0) {
        fprintf(out, "\n--- ESTATISTICAS DE TRAFEGO ---\n");
        fprintf(out, "INFO            : %ld\n", total.count_info);
        fprintf(out, "DEBUG           : %ld\n", total.count_debug);
        fprintf(out, "IP Mais Frequente: %s\n", total.top_ip[0] != '\0' ? total.top_ip : "N/A");
    }

    fprintf(out, "=================================\n");

    if (f) fclose(f);
}

int main(int argc, char *argv[]) {
    if (argc < 4) {
        printf("Uso: %s <diretorio> <num_processos> <modo> [--verbose] [--output=ficheiro.txt]\n", argv[0]);
        exit(1);
    }

    char *diretorio   = argv[1];
    int num_processos = atoi(argv[2]);
    char *modo        = argv[3];

    int verbose = 0;
    char *output_file = NULL;

    /* Parse dos argumentos facultativos */
    for (int i = 4; i < argc; i++) {
        if (strcmp(argv[i], "--verbose") == 0) {
            verbose = 1;
        } else if (strncmp(argv[i], "--output=", 9) == 0) {
            output_file = argv[i] + 9; // Guarda o nome do ficheiro (tudo a seguir ao "=")
        }
    }

    int capacidade = 10, total_ficheiros = 0;
    char **ficheiros = malloc(capacidade * sizeof(char *));
    if (!ficheiros) { perror("malloc"); exit(1); }

    DIR *dir = opendir(diretorio);
    if (!dir) { perror("opendir"); exit(1); }

    struct dirent *entrada;
    while ((entrada = readdir(dir)) != NULL) {
        char *nome = entrada->d_name;
        int len = strlen(nome);
        if (len > 4 && strcmp(nome + len - 4, ".log") == 0) {
            if (total_ficheiros == capacidade) {
                capacidade *= 2;
                ficheiros = realloc(ficheiros, capacidade * sizeof(char *));
                if (!ficheiros) { perror("realloc"); exit(1); }
            }
            char caminho[512];
            snprintf(caminho, sizeof(caminho), "%s/%s", diretorio, nome);
            ficheiros[total_ficheiros++] = strdup(caminho);
        }
    }
    closedir(dir);

    if (total_ficheiros == 0) {
        printf("Nenhum ficheiro .log encontrado.\n");
        exit(0);
    }
    if (num_processos > total_ficheiros) num_processos = total_ficheiros;

    int server_fd = create_server_socket();
    if (server_fd < 0) exit(1);

    g_num_workers = num_processos;
    g_start_time  = time(NULL);
    memset(worker_status, 0, sizeof(worker_status));

    int ficheiros_por_processo = total_ficheiros / num_processos;

    for (int i = 0; i < num_processos; i++) {
        pid_t pid = fork();
        if (pid < 0) { perror("fork"); exit(1); }

        if (pid == 0) {
            close(server_fd);
            int inicio = i * ficheiros_por_processo;
            int fim    = (i == num_processos - 1) ? total_ficheiros : inicio + ficheiros_por_processo;
            run_worker(ficheiros, inicio, fim, i, verbose);
            exit(0);
        }
    }

    for (int i = 0; i < g_num_workers + 5; i++) printf("\n");

    signal(SIGALRM, sigalrm_handler);
    alarm(1);

    WorkerResult total = {0};
    int workers_done = 0;
    
    int client_sockets[MAX_WORKERS];
    for (int i = 0; i < MAX_WORKERS; i++) client_sockets[i] = 0;

    while (workers_done < num_processos) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(server_fd, &readfds);
        int max_sd = server_fd;

        for (int i = 0; i < num_processos; i++) {
            int sd = client_sockets[i];
            if (sd > 0) FD_SET(sd, &readfds);
            if (sd > max_sd) max_sd = sd;
        }

        int activity = select(max_sd + 1, &readfds, NULL, NULL, NULL);
        if ((activity < 0) && (errno != EINTR)) perror("select error");
        if (activity < 0) continue;

        if (FD_ISSET(server_fd, &readfds)) {
            int new_socket = accept_client(server_fd);
            if (new_socket >= 0) {
                for (int i = 0; i < num_processos; i++) {
                    if (client_sockets[i] == 0) {
                        client_sockets[i] = new_socket;
                        break;
                    }
                }
            }
        }

        for (int i = 0; i < num_processos; i++) {
            int sd = client_sockets[i];
            if (sd > 0 && FD_ISSET(sd, &readfds)) {
                MsgHeader hdr;
                ssize_t valread = readn(sd, &hdr, sizeof(hdr)); 

                if (valread <= 0) {
                    close(sd);
                    client_sockets[i] = 0;
                    workers_done++;
                } else {
                    if (hdr.type == MSG_PROGRESS) {
                        ProgressUpdate pu;
                        readn(sd, &pu, sizeof(pu));
                        int idx = pu.worker_index;
                        if (idx >= 0 && idx < g_num_workers) {
                            worker_status[idx].pid         = pu.pid;
                            worker_status[idx].lines_done  = pu.lines_done;
                            worker_status[idx].lines_total = pu.lines_total;
                        }
                    } else if (hdr.type == MSG_RESULT) {
                        WorkerResult r;
                        readn(sd, &r, sizeof(r));
                        total.total_lines    += r.total_lines;
                        total.count_debug    += r.count_debug;
                        total.count_info     += r.count_info;
                        total.count_warn     += r.count_warn;
                        total.count_error    += r.count_error;
                        total.count_critical += r.count_critical;
                        total.count_4xx      += r.count_4xx;
                        total.count_5xx      += r.count_5xx;
                        if (total.top_ip[0] == '\0' || r.count_info > total.count_info) {
                            strncpy(total.top_ip, r.top_ip, IP_LEN);
                        }
                        g_total_errors += total.count_error;
                    }
                }
            }
        }
    }

    alarm(0); 
    draw_dashboard();

    close(server_fd);
    unlink(SOCKET_PATH);
    for (int i = 0; i < num_processos; i++) wait(NULL);

    /* Chama a nova função para imprimir (terminal ou ficheiro) */
    gerar_relatorio(total, modo, output_file);

    for (int i = 0; i < total_ficheiros; i++) free(ficheiros[i]);
    free(ficheiros);

    return 0;
}