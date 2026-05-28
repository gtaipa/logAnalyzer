#include "ipc.h"
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>

/* =========================================================
 * connect_to_server
 *
 * Chamada pelo filho para se ligar ao servidor (pai).
 * Tenta várias vezes caso o pai ainda não esteja pronto.
 * ========================================================= */
int connect_to_server(void) {

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    /* Tentar ligar até 10 vezes (o pai pode ainda não ter feito bind) */
    int tentativas = 10;
    while (tentativas-- > 0) {
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0)
            return fd;  /* ligado com sucesso */
        usleep(50000);  /* esperar 50ms antes de tentar outra vez */
    }

    perror("connect");
    close(fd);
    return -1;
}

/* =========================================================
 * readn - Le ate nbytes de forma segura a partir de fd.
 *
 * Em pipes e sockets, uma chamada read() pode devolver menos
 * bytes do que os pedidos. Por isso, esta funcao repete read()
 * ate completar nbytes, encontrar EOF, ou detetar erro real.
 * ========================================================= */
ssize_t readn(int fd, void *ptr, size_t nbytes) {
    // 1. Preparar o contador de bytes em falta e um ponteiro byte-a-byte para avancar no buffer do chamador.
    size_t nleft = nbytes;
    char *buf_ptr = ptr;

    // 2. O ciclo existe porque read() em pipes/sockets nao garante entregar todos os nbytes numa unica chamada.
    while (nleft > 0) {
        ssize_t nread = read(fd, buf_ptr, nleft);

        // 3. Se read() foi interrompido por um sinal, errno fica EINTR; nao e erro logico, repetimos a leitura.
        if (nread == -1 && errno == EINTR) {
            continue;
        }

        // 4. Qualquer outro valor negativo representa erro real do sistema, logo a funcao sinaliza falha com -1.
        if (nread == -1) {
            return -1;
        }

        // 5. read() devolver 0 significa EOF: o escritor fechou o pipe/socket antes de enviar todos os bytes.
        if (nread == 0) {
            break;
        }

        // 6. Como a leitura pode ser parcial, descontamos os bytes recebidos e avancamos no destino.
        nleft -= (size_t)nread;
        buf_ptr += nread;
    }

    // 7. Retornar o total efetivamente lido permite ao chamador distinguir leitura completa de EOF prematuro.
    return (ssize_t)(nbytes - nleft);
}

/* =========================================================
 * writen - Escreve exatamente nbytes de forma segura em fd.
 *
 * Em pipes e sockets, write() tambem pode aceitar apenas parte
 * dos bytes pedidos. Esta funcao continua a escrever ate enviar
 * tudo, ou ate encontrar um erro real que impossibilite continuar.
 * ========================================================= */
ssize_t writen(int fd, void *ptr, size_t nbytes) {
    // 1. Preparar o contador de bytes por escrever e um ponteiro para a proxima posicao a enviar.
    size_t nleft = nbytes;
    char *buf_ptr = ptr;

    // 2. O ciclo e obrigatorio porque write() pode fazer uma escrita parcial em pipes/sockets.
    while (nleft > 0) {
        ssize_t nwritten = write(fd, buf_ptr, nleft);

        // 3. Se write() foi interrompido por um sinal, errno == EINTR; repetimos sem perder posicao no buffer.
        if (nwritten == -1 && errno == EINTR) {
            continue;
        }

        // 4. Um erro negativo diferente de EINTR indica falha real, como pipe fechado ou descritor invalido.
        if (nwritten == -1) {
            return -1;
        }

        // 5. write() devolver 0 nao faz progresso; tratamos como falha para evitar um ciclo infinito.
        if (nwritten == 0) {
            errno = EPIPE;
            return -1;
        }

        // 6. Como a escrita pode ser parcial, descontamos o que saiu e avancamos para o proximo byte.
        nleft -= (size_t)nwritten;
        buf_ptr += nwritten;
    }

    // 7. Se o ciclo terminou, todos os nbytes foram escritos com sucesso no descritor.
    return (ssize_t)nbytes;
}

/* =========================================================
 * preparar_resultado
 *
 * Converte as metricas internas de um worker num WorkerResult
 * pronto a enviar pelo pipe/socket: ordena os IPs por frequencia
 * e guarda os top 10.
 * ========================================================= */
void preparar_resultado(const Metrics *m, WorkerResult *r) {
    memset(r, 0, sizeof(*r));
    r->pid            = getpid();
    r->total_lines    = m->total_lines;
    r->count_debug    = m->count_debug;
    r->count_info     = m->count_info;
    r->count_warn     = m->count_warn;
    r->count_error    = m->count_error;
    r->count_critical = m->count_critical;
    r->count_4xx      = m->count_4xx;
    r->count_5xx      = m->count_5xx;

    char ips[MAX_IPS][IP_LEN];
    long counts[MAX_IPS];
    int n = m->ip_num < MAX_IPS ? m->ip_num : MAX_IPS;

    for (int i = 0; i < n; i++) {
        strncpy(ips[i], m->ip_list[i], IP_LEN - 1);
        ips[i][IP_LEN - 1] = '\0';
        counts[i] = m->ip_count[i];
    }
    for (int i = 0; i < n - 1; i++) {
        for (int j = 0; j < n - i - 1; j++) {
            if (counts[j] < counts[j + 1]) {
                long tmp = counts[j]; counts[j] = counts[j + 1]; counts[j + 1] = tmp;
                char tmp_ip[IP_LEN];
                strncpy(tmp_ip, ips[j], IP_LEN);
                strncpy(ips[j], ips[j + 1], IP_LEN);
                strncpy(ips[j + 1], tmp_ip, IP_LEN);
            }
        }
    }
    int limite = n < 10 ? n : 10;
    for (int i = 0; i < limite; i++) {
        strncpy(r->top_ips[i], ips[i], IP_LEN - 1);
        r->top_ips[i][IP_LEN - 1] = '\0';
        r->top_ips_counts[i] = counts[i];
    }
    r->num_alerts = m->num_alerts < MAX_ALERTS ? m->num_alerts : MAX_ALERTS;
    for (int i = 0; i < r->num_alerts; i++) {
        strncpy(r->alerts[i], m->alerts[i], ALERT_LEN - 1);
        r->alerts[i][ALERT_LEN - 1] = '\0';
    }
}

/* =========================================================
 * acumular_resultado
 *
 * Funde um WorkerResult parcial no total agregado pelo pai.
 * ip_list_global/ip_count_global/ip_num_global mantêm o estado
 * entre chamadas sucessivas (uma por worker).
 * ========================================================= */
void acumular_resultado(WorkerResult *total, const WorkerResult *r,
                        char ip_list_global[256][IP_LEN],
                        long ip_count_global[256], int *ip_num_global) {
    total->total_lines    += r->total_lines;
    total->count_debug    += r->count_debug;
    total->count_info     += r->count_info;
    total->count_warn     += r->count_warn;
    total->count_error    += r->count_error;
    total->count_critical += r->count_critical;
    total->count_4xx      += r->count_4xx;
    total->count_5xx      += r->count_5xx;

    for (int k = 0; k < 10; k++) {
        if (r->top_ips[k][0] == '\0' || r->top_ips_counts[k] <= 0) continue;
        int found = -1;
        for (int i = 0; i < *ip_num_global; i++) {
            if (strcmp(ip_list_global[i], r->top_ips[k]) == 0) { found = i; break; }
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
                long tmp = ip_count_global[j];
                ip_count_global[j] = ip_count_global[j + 1];
                ip_count_global[j + 1] = tmp;
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
