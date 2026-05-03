#include "worker.h"
#include "parser.h"
#include "ipc.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BUF_SIZE 4096
#define LINE_MAX_LOCAL 512
#define PROGRESS_INTERVAL 100

#define MSG_PROGRESS 1
#define MSG_RESULT 2

typedef struct {
    int type;
} MsgHeader;

static void get_top_ip(const Metrics *m, char top_ip[IP_LEN]) {
    // 1. Inicializar com "-" garante um valor binario valido mesmo quando nenhum IP e encontrado.
    strncpy(top_ip, "-", IP_LEN - 1);
    top_ip[IP_LEN - 1] = '\0';

    // 2. O worker calcula localmente o IP mais frequente porque a memoria nao e partilhada com o pai.
    long top_count = 0;
    for (int i = 0; i < m->ip_num; i++) {
        if (m->ip_count[i] > top_count) {
            top_count = m->ip_count[i];
            strncpy(top_ip, m->ip_list[i], IP_LEN - 1);
            top_ip[IP_LEN - 1] = '\0';
        }
    }
}

static int processar_linha(char *line, LogFormat *fmt, Metrics *m) {
    // 3. A primeira linha valida revela o formato do log; depois reutilizamos esse formato para eficiencia.
    if (*fmt == FORMAT_UNKNOWN) {
        *fmt = detect_format(line);
    }

    // 4. A linha reconstruida a partir dos bytes do Kernel e entregue ao parser logico do projeto.
    LogEntry entry;
    if (parse_line(line, *fmt, &entry) == 0) {
        update_metrics(m, &entry);
    }

    return 0;
}

static int contar_linhas_posix(const char *path, long *total_linhas) {
    // 5. open(..., O_RDONLY) pede ao Kernel um descritor de ficheiro sem criar abstracoes de stdio.
    int fd = open(path, O_RDONLY);
    if (fd == -1) {
        perror("open");
        return -1;
    }

    // 6. read() trabalha com blocos de bytes; contar linhas exige procurar '\n' manualmente em cada chunk.
    char buf[BUF_SIZE];
    long count = 0;
    int viu_byte = 0;
    char ultimo = '\0';

    while (1) {
        ssize_t bytes_read = read(fd, buf, sizeof(buf));

        // 7. EINTR significa que um sinal interrompeu a chamada; repetimos porque ainda nao ha erro real.
        if (bytes_read == -1 && errno == EINTR) {
            continue;
        }

        // 8. Qualquer outro erro de read() invalida a contagem e obriga a fechar o descritor do Kernel.
        if (bytes_read == -1) {
            perror("read");
            if (close(fd) == -1) {
                perror("close");
            }
            return -1;
        }

        // 9. read() devolver 0 e EOF; o Kernel indica que nao ha mais bytes no ficheiro.
        if (bytes_read == 0) {
            break;
        }

        for (ssize_t i = 0; i < bytes_read; i++) {
            viu_byte = 1;
            ultimo = buf[i];
            if (buf[i] == '\n') {
                count++;
            }
        }
    }

    // 10. Se a ultima linha nao terminou com '\n', ela tambem conta como uma linha logica.
    if (viu_byte && ultimo != '\n') {
        count++;
    }

    // 11. close(fd) devolve o descritor ao Kernel assim que a contagem auxiliar termina.
    if (close(fd) == -1) {
        perror("close");
        return -1;
    }

    *total_linhas = count;
    return 0;
}

static int enviar_progresso(int sock_fd, int worker_index, long lines_done, long lines_total) {
    // 12. O cabecalho identifica a proxima mensagem no fluxo binario continuo do socket.
    MsgHeader hdr = { MSG_PROGRESS };
    if (writen(sock_fd, &hdr, sizeof(hdr)) == -1) {
        perror("writen");
        return -1;
    }

    // 13. ProgressUpdate e enviado em binario para o pai conseguir ler exatamente sizeof(ProgressUpdate).
    ProgressUpdate pu;
    pu.pid = getpid();
    pu.worker_index = worker_index;
    pu.lines_done = lines_done;
    pu.lines_total = lines_total;

    if (writen(sock_fd, &pu, sizeof(pu)) == -1) {
        perror("writen");
        return -1;
    }

    return 0;
}

static int process_file_posix(const char *path, Metrics *m, int verbose,
                              int sock_fd, int worker_index,
                              long *lines_done, long lines_total) {
    // 14. O worker abre cada ficheiro diretamente no Kernel com open(), obtendo apenas um descritor inteiro.
    int fd = open(path, O_RDONLY);
    if (fd == -1) {
        perror("open");
        return -1;
    }

    if (verbose) {
        dprintf(STDERR_FILENO, "[Filho %d] A abrir: %s\n", getpid(), path);
    }

    // 15. Este buffer recebe bytes crus; como read() nao sabe o que e uma linha, fazemos parsing manual.
    char buf[BUF_SIZE];
    char line[LINE_MAX_LOCAL];
    int line_len = 0;
    LogFormat fmt = FORMAT_UNKNOWN;

    while (1) {
        // 16. read() pode devolver qualquer quantidade positiva ate BUF_SIZE, logo o codigo aceita chunks parciais.
        ssize_t bytes_read = read(fd, buf, sizeof(buf));

        // 17. Se um sinal interromper read(), errno == EINTR; a leitura e repetida sem perder estado da linha.
        if (bytes_read == -1 && errno == EINTR) {
            continue;
        }

        // 18. Erros reais de leitura sao reportados e o descritor do ficheiro e fechado antes de regressar.
        if (bytes_read == -1) {
            perror("read");
            if (close(fd) == -1) {
                perror("close");
            }
            return -1;
        }

        // 19. EOF encerra o ciclo; qualquer linha incompleta sera processada no bloco seguinte.
        if (bytes_read == 0) {
            break;
        }

        // 20. Percorrer byte a byte e obrigatorio porque uma linha pode estar dividida entre dois chunks.
        for (ssize_t i = 0; i < bytes_read; i++) {
            char c = buf[i];

            // 21. Ao encontrar fim de linha, fechamos a string C e atualizamos as metricas.
            if (c == '\n' || c == '\r') {
                if (line_len == 0) {
                    continue;
                }

                line[line_len] = '\0';
                if (processar_linha(line, &fmt, m) == -1) {
                    if (close(fd) == -1) {
                        perror("close");
                    }
                    return -1;
                }

                line_len = 0;
                (*lines_done)++;

                // 22. O progresso segue pelo mesmo socket como mensagem binaria, antes do resultado final.
                if ((*lines_done % PROGRESS_INTERVAL) == 0) {
                    if (enviar_progresso(sock_fd, worker_index, *lines_done, lines_total) == -1) {
                        if (close(fd) == -1) {
                            perror("close");
                        }
                        return -1;
                    }
                }

                continue;
            }

            // 23. Guardar caracteres ate ao limite local evita overflow durante a reconstrucao manual da linha.
            if (line_len < LINE_MAX_LOCAL - 1) {
                line[line_len++] = c;
            }
        }
    }

    // 24. Logs podem nao terminar em newline; nesse caso ainda existe uma ultima linha a processar.
    if (line_len > 0) {
        line[line_len] = '\0';
        if (processar_linha(line, &fmt, m) == -1) {
            if (close(fd) == -1) {
                perror("close");
            }
            return -1;
        }
        (*lines_done)++;
    }

    // 25. Fechar o descritor do ficheiro confirma que o worker ja terminou o contacto com esse objeto do Kernel.
    if (close(fd) == -1) {
        perror("close");
        return -1;
    }

    return 0;
}

static void preparar_resultado(const Metrics *m, WorkerResult *r) {
    // 26. WorkerResult e a mensagem final compacta que atravessa o socket como bytes binarios consecutivos.
    r->pid = getpid();
    r->total_lines = m->total_lines;
    r->count_debug = m->count_debug;
    r->count_info = m->count_info;
    r->count_warn = m->count_warn;
    r->count_error = m->count_error;
    r->count_critical = m->count_critical;
    r->count_4xx = m->count_4xx;
    r->count_5xx = m->count_5xx;
    get_top_ip(m, r->top_ip);
}

static int enviar_resultado_final(int sock_fd, const Metrics *m) {
    // 27. O cabecalho MSG_RESULT diz ao pai que os proximos bytes formam uma estrutura WorkerResult.
    MsgHeader hdr = { MSG_RESULT };
    if (writen(sock_fd, &hdr, sizeof(hdr)) == -1) {
        perror("writen");
        return -1;
    }

    // 28. Enviar a struct por writen() evita que uma escrita parcial deixe o pai com dados incompletos.
    WorkerResult r;
    preparar_resultado(m, &r);

    if (writen(sock_fd, &r, sizeof(r)) == -1) {
        perror("writen");
        return -1;
    }

    return 0;
}

void run_worker(char **ficheiros, int inicio, int fim, int worker_index, int verbose) {
    // 29. O filho inicia a sua execucao IPC ligando-se ao socket servidor criado pelo pai.
    int sock_fd = connect_to_server();
    if (sock_fd == -1) {
        perror("connect_to_server");
        exit(1);
    }

    // 30. As metricas vivem apenas neste processo filho e serao enviadas ao pai no fim.
    Metrics m;
    init_metrics(&m);

    // 31. A contagem previa de linhas usa open/read para calcular progresso sem recorrer a stdio.
    long lines_total = 0;
    for (int i = inicio; i < fim; i++) {
        long ficheiro_linhas = 0;
        if (contar_linhas_posix(ficheiros[i], &ficheiro_linhas) == -1) {
            if (close(sock_fd) == -1) {
                perror("close");
            }
            exit(1);
        }
        lines_total += ficheiro_linhas;
    }

    // 32. Cada ficheiro atribuido ao worker e analisado por chunks de bytes lidos com read().
    long lines_done = 0;
    for (int i = inicio; i < fim; i++) {
        if (process_file_posix(ficheiros[i], &m, verbose, sock_fd, worker_index,
                               &lines_done, lines_total) == -1) {
            if (close(sock_fd) == -1) {
                perror("close");
            }
            exit(1);
        }
    }

    // 33. Enviar progresso final permite ao pai saber que este cliente chegou a 100% antes do resultado.
    if (enviar_progresso(sock_fd, worker_index, lines_total, lines_total) == -1) {
        if (close(sock_fd) == -1) {
            perror("close");
        }
        exit(1);
    }

    // 34. O resultado final segue pelo socket em formato binario continuo usando a funcao segura writen().
    if (enviar_resultado_final(sock_fd, &m) == -1) {
        if (close(sock_fd) == -1) {
            perror("close");
        }
        exit(1);
    }

    // 35. close(sock_fd) encerra a ligacao cliente e sinaliza ao pai que nao havera mais bytes deste worker.
    if (close(sock_fd) == -1) {
        perror("close");
        exit(1);
    }
}
