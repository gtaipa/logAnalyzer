#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ipc.h"
#include "parser.h"
#include "posix_io.h"
#include "worker.h"

#define MSG_PROGRESS 1
#define MSG_RESULT 2

typedef struct {
    int type;
} MsgHeader;

#define DASHBOARD_BAR_WIDTH 20

static void desenhar_barra_progresso(char *buffer, size_t tamanho, long feito, long total) {
    int preenchidos = 0;

    if (total > 0 && feito > 0) {
        preenchidos = (int)((feito * DASHBOARD_BAR_WIDTH) / total);
        if (preenchidos > DASHBOARD_BAR_WIDTH) {
            preenchidos = DASHBOARD_BAR_WIDTH;
        }
    }

    int pos = 0;
    if (tamanho == 0) {
        return;
    }

    pos += snprintf(buffer + pos, tamanho - (size_t)pos, "[");
    for (int i = 0; i < DASHBOARD_BAR_WIDTH && (size_t)pos < tamanho; i++) {
        pos += snprintf(buffer + pos, tamanho - (size_t)pos, "%s",
                        i < preenchidos ? "█" : "░");
    }
    if ((size_t)pos < tamanho) {
        snprintf(buffer + pos, tamanho - (size_t)pos, "]");
    }
}

static void desenhar_dashboard(const ProgressUpdate *progressos, int num_processos) {
    posix_writef(STDOUT_FILENO, "\033[H\033[J");
    posix_writef(STDOUT_FILENO, "=== PROGRESSO DOS WORKERS ===\n\n");

    for (int i = 0; i < num_processos; i++) {
        long total = progressos[i].lines_total;
        long feito = progressos[i].lines_done;
        int percentagem = 0;

        if (total > 0) {
            if (feito > total) {
                feito = total;
            }
            percentagem = (int)((feito * 100) / total);
        }

        char barra[128];
        desenhar_barra_progresso(barra, sizeof(barra), feito, total);

        posix_writef(STDOUT_FILENO, "Worker %2d %s %3d%% (%ld/%ld linhas)\n",
               progressos[i].worker_index, barra, percentagem, feito, total);
    }

}

static void atualizar_dashboard_se_preciso(const ProgressUpdate *progressos,
                                           int num_processos,
                                           time_t *ultimo_refresh) {
    time_t agora = time(NULL);
    if (agora == (time_t)-1) {
        return;
    }

    if (*ultimo_refresh == 0 || difftime(agora, *ultimo_refresh) >= 1.0) {
        desenhar_dashboard(progressos, num_processos);
        *ultimo_refresh = agora;
    }
}

static void libertar_ficheiros(char **ficheiros, int total_ficheiros) {
    // 1. Libertar cada caminho antes do vetor principal evita perdas de memoria no processo pai.
    if (ficheiros == NULL) {
        return;
    }

    for (int i = 0; i < total_ficheiros; i++) {
        free(ficheiros[i]);
    }

    free(ficheiros);
}

static int converter_num_processos(const char *texto) {
    char *fim = NULL;

    // 2. Validar o numero de processos impede fork()s indevidos e evita divisao por zero.
    errno = 0;
    long valor = strtol(texto, &fim, 10);
    if (errno != 0 || fim == texto || *fim != '\0' || valor <= 0) {
        posix_writef(STDERR_FILENO, "Numero de processos invalido: %s\n", texto);
        exit(1);
    }

    return (int)valor;
}

static void acumular_resultado(WorkerResult *total, const WorkerResult *r) {
    // 3. O pai agrega resultados porque cada filho tem espaco de enderecamento proprio apos fork().
    total->total_lines += r->total_lines;
    total->count_debug += r->count_debug;
    total->count_info += r->count_info;
    total->count_warn += r->count_warn;
    total->count_error += r->count_error;
    total->count_critical += r->count_critical;
    total->count_4xx += r->count_4xx;
    total->count_5xx += r->count_5xx;

    if (total->top_ip[0] == '\0' && r->top_ip[0] != '\0') {
        strncpy(total->top_ip, r->top_ip, IP_LEN - 1);
        total->top_ip[IP_LEN - 1] = '\0';
    }
}

static void gerar_relatorio(WorkerResult total, char *modo, char *output_file) {
    // 4. Por omissao, o relatorio e escrito no stdout; opcionalmente redireciona-se para ficheiro com open().
    int fd_out = STDOUT_FILENO;
    int fd_file = -1;

    if (output_file != NULL) {
        fd_file = open(output_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd_file == -1) {
            perror("open");
            exit(1);
        }
        fd_out = fd_file;
        posix_writef(STDOUT_FILENO, "\n[INFO] A gravar relatorio no ficheiro: %s\n", output_file);
    }

    // 5. Construir o relatorio em memoria permite fazer uma escrita POSIX unica e controlada.
    char buffer[4096];
    int len = 0;

    len += snprintf(buffer + len, sizeof(buffer) - (size_t)len,
                    "\n=== RELATORIO FINAL (%s) ===\n", modo);
    len += snprintf(buffer + len, sizeof(buffer) - (size_t)len,
                    "Total de linhas : %ld\n", total.total_lines);

    if (strcmp(modo, "security") == 0 || strcmp(modo, "full") == 0) {
        len += snprintf(buffer + len, sizeof(buffer) - (size_t)len,
                        "\n--- ALERTAS DE SEGURANCA ---\n");
        len += snprintf(buffer + len, sizeof(buffer) - (size_t)len,
                        "WARNINGS        : %ld\n", total.count_warn);
        len += snprintf(buffer + len, sizeof(buffer) - (size_t)len,
                        "ERRORS          : %ld\n", total.count_error);
        len += snprintf(buffer + len, sizeof(buffer) - (size_t)len,
                        "CRITICAL        : %ld\n", total.count_critical);
        len += snprintf(buffer + len, sizeof(buffer) - (size_t)len,
                        "HTTP 4xx/5xx    : %ld\n", total.count_4xx + total.count_5xx);
    }

    if (strcmp(modo, "traffic") == 0 || strcmp(modo, "full") == 0) {
        len += snprintf(buffer + len, sizeof(buffer) - (size_t)len,
                        "\n--- ESTATISTICAS DE TRAFEGO ---\n");
        len += snprintf(buffer + len, sizeof(buffer) - (size_t)len,
                        "INFO            : %ld\n", total.count_info);
        len += snprintf(buffer + len, sizeof(buffer) - (size_t)len,
                        "DEBUG           : %ld\n", total.count_debug);
        len += snprintf(buffer + len, sizeof(buffer) - (size_t)len,
                        "IP Mais Frequente: %s\n",
                        total.top_ip[0] != '\0' ? total.top_ip : "N/A");
    }

    len += snprintf(buffer + len, sizeof(buffer) - (size_t)len,
                    "=================================\n");

    if (len < 0 || (size_t)len >= sizeof(buffer)) {
        posix_writef(STDERR_FILENO, "Erro: relatorio excede o buffer interno\n");
        if (fd_file != -1 && close(fd_file) == -1) {
            perror("close");
        }
        exit(1);
    }

    // 6. writen() garante que todo o relatorio e enviado, mesmo que write() faca escritas parciais.
    if (writen(fd_out, buffer, (size_t)len) == -1) {
        perror("writen");
        if (fd_file != -1 && close(fd_file) == -1) {
            perror("close");
        }
        exit(1);
    }

    if (fd_file != -1 && close(fd_file) == -1) {
        perror("close");
        exit(1);
    }
}

static void ler_diretorio_logs(const char *diretorio, char ***ficheiros_out, int *total_out) {
    // 7. Reservar um vetor dinamico permite guardar todos os caminhos antes de dividir trabalho pelos filhos.
    int capacidade = 10;
    int total_ficheiros = 0;
    char **ficheiros = malloc((size_t)capacidade * sizeof(char *));
    if (ficheiros == NULL) {
        perror("malloc");
        exit(1);
    }

    // 8. O pai percorre o diretorio antes do fork para que todos os filhos herdem a mesma lista de caminhos.
    DIR *dir = opendir(diretorio);
    if (dir == NULL) {
        perror("opendir");
        libertar_ficheiros(ficheiros, total_ficheiros);
        exit(1);
    }

    struct dirent *entrada;
    errno = 0;
    while ((entrada = readdir(dir)) != NULL) {
        char *nome = entrada->d_name;
        size_t len = strlen(nome);

        if (!((len > 4 && strcmp(nome + len - 4, ".log") == 0) ||
              (len > 5 && strcmp(nome + len - 5, ".json") == 0))) {
            continue;
        }

        // 9. Aumentar o vetor antes de inserir evita escrever para alem da memoria reservada.
        if (total_ficheiros == capacidade) {
            capacidade *= 2;
            char **novo = realloc(ficheiros, (size_t)capacidade * sizeof(char *));
            if (novo == NULL) {
                perror("realloc");
                if (closedir(dir) == -1) {
                    perror("closedir");
                }
                libertar_ficheiros(ficheiros, total_ficheiros);
                exit(1);
            }
            ficheiros = novo;
        }

        // 10. Construir o caminho absoluto relativo ao diretorio permite ao worker abrir cada log corretamente.
        char caminho[512];
        int escritos = snprintf(caminho, sizeof(caminho), "%s/%s", diretorio, nome);
        if (escritos < 0 || (size_t)escritos >= sizeof(caminho)) {
            posix_writef(STDERR_FILENO, "Caminho demasiado longo: %s/%s\n", diretorio, nome);
            if (closedir(dir) == -1) {
                perror("closedir");
            }
            libertar_ficheiros(ficheiros, total_ficheiros);
            exit(1);
        }

        ficheiros[total_ficheiros] = strdup(caminho);
        if (ficheiros[total_ficheiros] == NULL) {
            perror("strdup");
            if (closedir(dir) == -1) {
                perror("closedir");
            }
            libertar_ficheiros(ficheiros, total_ficheiros);
            exit(1);
        }
        total_ficheiros++;
    }

    // 11. readdir() usa NULL tanto para fim como para erro; errno distingue os dois casos.
    if (errno != 0) {
        perror("readdir");
        if (closedir(dir) == -1) {
            perror("closedir");
        }
        libertar_ficheiros(ficheiros, total_ficheiros);
        exit(1);
    }

    if (closedir(dir) == -1) {
        perror("closedir");
        libertar_ficheiros(ficheiros, total_ficheiros);
        exit(1);
    }

    *ficheiros_out = ficheiros;
    *total_out = total_ficheiros;
}

int main(int argc, char *argv[]) {
    // 12. Validar argumentos antes de criar sockets ou processos evita recursos abertos em casos triviais de erro.
    if (argc < 4) {
        posix_writef(STDOUT_FILENO, "Uso: %s <diretorio> <num_processos> <modo> [--verbose] [--output=ficheiro.txt]\n",
               argv[0]);
        exit(1);
    }

    char *diretorio = argv[1];
    int num_processos = converter_num_processos(argv[2]);
    char *modo = argv[3];

    int verbose = 0;
    char *output_file = NULL;

    // 13. Interpretar opcoes adicionais no pai define configuracao comum antes de criar workers.
    for (int i = 4; i < argc; i++) {
        if (strcmp(argv[i], "--verbose") == 0) {
            verbose = 1;
        } else if (strncmp(argv[i], "--output=", 9) == 0) {
            output_file = argv[i] + 9;
        }
    }

    // 14. Configurar o modo do parser antes do fork permite que os filhos herdem esta configuracao.
    if (parser_set_mode_from_string(modo) != 0) {
        posix_writef(STDERR_FILENO, "Modo invalido: %s (use security|performance|traffic|full)\n", modo);
        exit(1);
    }

    char **ficheiros = NULL;
    int total_ficheiros = 0;
    ler_diretorio_logs(diretorio, &ficheiros, &total_ficheiros);

    // 15. Sem ficheiros nao ha comunicacao: o programa termina antes de criar socket Unix ou filhos.
    if (total_ficheiros == 0) {
        posix_writef(STDOUT_FILENO, "Nenhum ficheiro .log ou .json encontrado.\n");
        libertar_ficheiros(ficheiros, total_ficheiros);
        exit(0);
    }

    if (num_processos > total_ficheiros) {
        num_processos = total_ficheiros;
    }

    // 16. O servidor cria o socket antes do fork para reservar SOCKET_PATH no file system e aceitar os filhos.
    int server_fd = create_server_socket();
    if (server_fd == -1) {
        libertar_ficheiros(ficheiros, total_ficheiros);
        exit(1);
    }

    pid_t *pids = malloc((size_t)num_processos * sizeof(pid_t));
    if (pids == NULL) {
        perror("malloc");
        if (close(server_fd) == -1) {
            perror("close");
        }
        if (unlink(SOCKET_PATH) == -1) {
            perror("unlink");
        }
        libertar_ficheiros(ficheiros, total_ficheiros);
        exit(1);
    }

    int ficheiros_por_processo = total_ficheiros / num_processos;

    // 17. Esvaziar buffers stdio antes do fork evita que os filhos repitam texto ja impresso pelo pai.
    if (fflush(NULL) == EOF) {
        perror("fflush");
        free(pids);
        if (close(server_fd) == -1) {
            perror("close");
        }
        if (unlink(SOCKET_PATH) == -1) {
            perror("unlink");
        }
        libertar_ficheiros(ficheiros, total_ficheiros);
        exit(1);
    }

    for (int i = 0; i < num_processos; i++) {
        pid_t pid;

        // 18. Criar cada worker com o padrao classico POSIX de fork() e verificar erro imediatamente.
        if ((pid = fork()) == -1) {
            perror("fork");
            free(pids);
            if (close(server_fd) == -1) {
                perror("close");
            }
            if (unlink(SOCKET_PATH) == -1) {
                perror("unlink");
            }
            libertar_ficheiros(ficheiros, total_ficheiros);
            exit(1);
        }

        if (pid == 0) {
            // 19. O filho fecha o socket servidor herdado porque apenas precisa de se ligar como cliente.
            if (close(server_fd) == -1) {
                perror("close");
                exit(1);
            }

            // 20. Cada filho recebe um intervalo disjunto de ficheiros para evitar trabalho duplicado.
            int inicio = i * ficheiros_por_processo;
            int fim = (i == num_processos - 1) ? total_ficheiros : inicio + ficheiros_por_processo;

            run_worker(ficheiros, inicio, fim, i, verbose);
            exit(0);
        }

        pids[i] = pid;
    }

    int *client_fds = malloc((size_t)num_processos * sizeof(int));
    if (client_fds == NULL) {
        perror("malloc");
        free(pids);
        if (close(server_fd) == -1) {
            perror("close");
        }
        if (unlink(SOCKET_PATH) == -1) {
            perror("unlink");
        }
        libertar_ficheiros(ficheiros, total_ficheiros);
        exit(1);
    }

    // 21. O pai aceita uma ligacao Unix Domain Socket por worker usando accept_client(), como servidor POSIX.
    for (int i = 0; i < num_processos; i++) {
        client_fds[i] = accept_client(server_fd);
        if (client_fds[i] == -1) {
            free(client_fds);
            free(pids);
            if (close(server_fd) == -1) {
                perror("close");
            }
            if (unlink(SOCKET_PATH) == -1) {
                perror("unlink");
            }
            libertar_ficheiros(ficheiros, total_ficheiros);
            exit(1);
        }
    }

    WorkerResult total = {0};
    int resultados_recebidos = 0;
    ProgressUpdate *progressos = calloc((size_t)num_processos, sizeof(ProgressUpdate));
    if (progressos == NULL) {
        perror("calloc");
        free(client_fds);
        free(pids);
        if (close(server_fd) == -1) {
            perror("close");
        }
        if (unlink(SOCKET_PATH) == -1) {
            perror("unlink");
        }
        libertar_ficheiros(ficheiros, total_ficheiros);
        exit(1);
    }

    for (int i = 0; i < num_processos; i++) {
        progressos[i].pid = pids[i];
        progressos[i].worker_index = i;
        progressos[i].lines_done = 0;
        progressos[i].lines_total = 0;
    }

    time_t ultimo_refresh_dashboard = 0;
    if (verbose) {
        atualizar_dashboard_se_preciso(progressos, num_processos, &ultimo_refresh_dashboard);
    }

    // 22. Ler mensagens dos clientes; cada WorkerResult e recebido com readn() para evitar leituras parciais.
    while (resultados_recebidos < num_processos) {
        for (int i = 0; i < num_processos; i++) {
            if (client_fds[i] == -1) {
                continue;
            }

            MsgHeader hdr;
            ssize_t lidos = readn(client_fds[i], &hdr, sizeof(hdr));
            if (lidos == -1) {
                perror("readn");
                exit(1);
            }
            if (lidos == 0) {
                posix_writef(STDERR_FILENO, "Erro: worker fechou socket antes de enviar resultado\n");
                exit(1);
            }
            if (lidos != (ssize_t)sizeof(hdr)) {
                posix_writef(STDERR_FILENO, "Erro: cabecalho incompleto recebido do worker\n");
                exit(1);
            }

            if (hdr.type == MSG_PROGRESS) {
                // 23. ProgressUpdate tambem e lido com readn(), mantendo alinhamento correto do stream.
                ProgressUpdate progresso;
                lidos = readn(client_fds[i], &progresso, sizeof(progresso));
                if (lidos == -1) {
                    perror("readn");
                    exit(1);
                }
                if (lidos != (ssize_t)sizeof(progresso)) {
                    posix_writef(STDERR_FILENO, "Erro: progresso incompleto recebido do worker\n");
                    exit(1);
                }
                if (progresso.worker_index >= 0 && progresso.worker_index < num_processos) {
                    progressos[progresso.worker_index] = progresso;
                }
                if (verbose) {
                    atualizar_dashboard_se_preciso(progressos, num_processos,
                                                   &ultimo_refresh_dashboard);
                }
            } else if (hdr.type == MSG_RESULT) {
                // 24. A estrutura WorkerResult contem o resumo final e e lida obrigatoriamente com readn().
                WorkerResult r;
                lidos = readn(client_fds[i], &r, sizeof(r));
                if (lidos == -1) {
                    perror("readn");
                    exit(1);
                }
                if (lidos != (ssize_t)sizeof(r)) {
                    posix_writef(STDERR_FILENO, "Erro: WorkerResult incompleto recebido do worker\n");
                    exit(1);
                }

                acumular_resultado(&total, &r);
                resultados_recebidos++;
                for (int j = 0; j < num_processos; j++) {
                    if (progressos[j].pid == r.pid) {
                        progressos[j].lines_done = progressos[j].lines_total;
                        break;
                    }
                }

                // 25. Depois do resultado final, o pai fecha o socket cliente porque ja recebeu tudo desse worker.
                if (close(client_fds[i]) == -1) {
                    perror("close");
                    exit(1);
                }
                client_fds[i] = -1;
            } else {
                posix_writef(STDERR_FILENO, "Erro: tipo de mensagem desconhecido: %d\n", hdr.type);
                exit(1);
            }
        }
    }

    if (verbose) {
        time_t agora = time(NULL);
        if (agora != (time_t)-1 && ultimo_refresh_dashboard != 0 &&
            difftime(agora, ultimo_refresh_dashboard) < 1.0) {
            sleep(1);
        }
        desenhar_dashboard(progressos, num_processos);
        posix_writef(STDOUT_FILENO, "\n");
    }

    // 26. Fechar o socket servidor impede novas ligacoes depois de todos os resultados terem chegado.
    if (close(server_fd) == -1) {
        perror("close");
        exit(1);
    }

    // 27. Remover SOCKET_PATH no final limpa o ficheiro especial criado pelo Unix Domain Socket.
    if (unlink(SOCKET_PATH) == -1) {
        perror("unlink");
        exit(1);
    }

    // 28. O pai recolhe explicitamente cada worker e interpreta o estado com macros POSIX.
    for (int i = 0; i < num_processos; i++) {
        int status;
        if (waitpid(pids[i], &status, 0) == -1) {
            perror("waitpid");
            exit(1);
        }

        if (WIFEXITED(status)) {
            int codigo = WEXITSTATUS(status);
            if (codigo != 0) {
                posix_writef(STDERR_FILENO, "[PAI] Filho %d terminou com codigo %d\n", pids[i], codigo);
                exit(1);
            }
        } else {
            posix_writef(STDERR_FILENO, "[PAI] Filho %d nao terminou por exit normal\n", pids[i]);
            exit(1);
        }
    }

    gerar_relatorio(total, modo, output_file);

    // 29. Libertar memoria do pai apos fechar sockets e recolher filhos conclui a limpeza do processo principal.
    free(progressos);
    free(client_fds);
    free(pids);
    libertar_ficheiros(ficheiros, total_ficheiros);

    exit(0);
}
