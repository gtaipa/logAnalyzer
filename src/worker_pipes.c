#include "ipc.h"
#include "parser.h"
#include "posix_io.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BUF_SIZE 4096
#define LINE_MAX_LOCAL 512

static void get_top_ip(const Metrics *m, char top_ip[IP_LEN]) {
    // 1. Inicializar o resultado com "-" garante uma resposta valida mesmo quando nao ha IPs no log.
    strncpy(top_ip, "-", IP_LEN - 1);
    top_ip[IP_LEN - 1] = '\0';

    // 2. Percorrer as metricas acumuladas em memoria local do filho para descobrir o IP mais frequente.
    long top_count = 0;
    for (int i = 0; i < m->ip_num; i++) {
        if (m->ip_count[i] > top_count) {
            top_count = m->ip_count[i];
            strncpy(top_ip, m->ip_list[i], IP_LEN - 1);
            top_ip[IP_LEN - 1] = '\0';
        }
    }
}

static int processar_linha(char *linha, LogFormat *fmt, Metrics *m) {
    // 3. Detetar o formato apenas na primeira linha valida evita repetir trabalho desnecessario.
    if (*fmt == FORMAT_UNKNOWN) {
        *fmt = detect_format(linha);
    }

    // 4. Converter texto bruto numa LogEntry permite reutilizar o parser existente sem usar I/O de alto nivel.
    LogEntry entry;
    if (parse_line(linha, *fmt, &entry) == 0) {
        update_metrics(m, &entry);
    }

    return 0;
}

static int process_file(const char *path, Metrics *m) {
    // 5. Abrir o ficheiro com open(..., O_RDONLY) usa diretamente a API POSIX e devolve um descritor inteiro.
    int fd = open(path, O_RDONLY);
    if (fd == -1) {
        perror("open");
        return -1;
    }

    // 6. O buffer de chunks recebe bytes crus vindos do kernel; nao usamos abstracoes stdio.
    char buf[BUF_SIZE];

    // 7. O buffer de linha junta bytes ate encontrar '\n' ou '\r', porque read() nao conhece linhas de texto.
    char line[LINE_MAX_LOCAL];
    int line_len = 0;
    LogFormat fmt = FORMAT_UNKNOWN;

    while (1) {
        // 8. read() pede ao kernel ate BUF_SIZE bytes; o valor devolvido diz quantos bytes chegaram realmente.
        ssize_t bytes_read = read(fd, buf, sizeof(buf));

        // 9. Se read() for interrompido por um sinal, repetimos a chamada; isto e comportamento POSIX normal.
        if (bytes_read == -1 && errno == EINTR) {
            continue;
        }

        // 10. Qualquer outro erro de read() e reportado e o descritor do log e fechado antes de sair da funcao.
        if (bytes_read == -1) {
            perror("read");
            if (close(fd) == -1) {
                perror("close");
            }
            return -1;
        }

        // 11. read() devolver 0 significa EOF: o ficheiro acabou e nao ha mais bytes para consumir.
        if (bytes_read == 0) {
            break;
        }

        // 12. Percorrer byte a byte e necessario porque uma linha pode vir partida entre dois chunks de read().
        for (ssize_t i = 0; i < bytes_read; i++) {
            char c = buf[i];

            // 13. '\n' ou '\r' fecha uma linha logica; so depois disso enviamos a linha ao parser.
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
                continue;
            }

            // 14. Guardar o byte na linha atual respeita o limite do buffer e evita overflow.
            if (line_len < LINE_MAX_LOCAL - 1) {
                line[line_len++] = c;
            }
        }
    }

    // 15. Se o ficheiro nao terminar com newline, a ultima linha acumulada tambem tem de ser processada.
    if (line_len > 0) {
        line[line_len] = '\0';
        if (processar_linha(line, &fmt, m) == -1) {
            if (close(fd) == -1) {
                perror("close");
            }
            return -1;
        }
    }

    // 16. Fechar o descritor do log no worker liberta o recurso POSIX assim que o ficheiro deixa de ser usado.
    if (close(fd) == -1) {
        perror("close");
        return -1;
    }

    return 0;
}

static void preparar_resultado(const Metrics *m, WorkerResult *result) {
    // 17. Copiar as metricas para uma estrutura simples cria uma mensagem binaria compacta para o pipe.
    result->pid = getpid();
    result->total_lines = m->total_lines;
    result->count_debug = m->count_debug;
    result->count_info = m->count_info;
    result->count_warn = m->count_warn;
    result->count_error = m->count_error;
    result->count_critical = m->count_critical;
    result->count_4xx = m->count_4xx;
    result->count_5xx = m->count_5xx;
    get_top_ip(m, result->top_ip);
}

static int escrever_resultado_pipe(int pipe_fd_write, const WorkerResult *result) {
    // 18. Enviar pelo pipe com write() usa a chamada ao sistema POSIX pedida, sem wrappers de alto nivel.
    const char *ptr = (const char *)result;
    size_t bytes_por_enviar = sizeof(*result);

    while (bytes_por_enviar > 0) {
        // 19. write() pode escrever menos bytes do que os pedidos; por isso avancamos o ponteiro e repetimos.
        ssize_t bytes_escritos = write(pipe_fd_write, ptr, bytes_por_enviar);

        // 20. Se write() for interrompido por sinal, tentamos novamente sem perder bytes da estrutura.
        if (bytes_escritos == -1 && errno == EINTR) {
            continue;
        }

        // 21. Qualquer outro erro significa que o pai pode ter fechado o pipe ou ocorreu falha real de escrita.
        if (bytes_escritos == -1) {
            perror("write");
            return -1;
        }

        ptr += bytes_escritos;
        bytes_por_enviar -= (size_t)bytes_escritos;
    }

    return 0;
}

void run_worker_pipe(char **ficheiros, int inicio, int fim, int pipe_fd_write, int verbose) {
    // 22. Inicializar metricas no filho garante acumulacao independente da memoria do processo pai.
    Metrics m;
    init_metrics(&m);

    if (verbose) {
        posix_writef(STDOUT_FILENO, "[Filho %d] Vou processar %d ficheiro(s) (%d..%d)\n",
               getpid(), fim - inicio, inicio, fim - 1);
    }

    // 23. Processar cada caminho atribuido a este worker usando apenas open(), read() e close() no ficheiro.
    for (int i = inicio; i < fim; i++) {
        if (verbose) {
            posix_writef(STDOUT_FILENO, "[Filho %d] A processar: %s\n", getpid(), ficheiros[i]);
        }

        if (process_file(ficheiros[i], &m) == -1) {
            if (close(pipe_fd_write) == -1) {
                perror("close");
            }
            exit(1);
        }
    }

    // 24. Preparar a mensagem final apenas depois de todos os logs do intervalo terem sido analisados.
    WorkerResult result;
    preparar_resultado(&m, &result);

    // 25. Escrever a estrutura no pipe com write(); o pai le exatamente o mesmo tamanho com readn().
    if (escrever_resultado_pipe(pipe_fd_write, &result) == -1) {
        if (close(pipe_fd_write) == -1) {
            perror("close");
        }
        exit(1);
    }

    // 26. Fechar a extremidade de escrita do pipe antes de exit(0) sinaliza EOF ao pai e evita descritores abertos.
    if (close(pipe_fd_write) == -1) {
        perror("close");
        exit(1);
    }

    exit(0);
}
