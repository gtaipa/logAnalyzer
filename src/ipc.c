#include "ipc.h"

#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/un.h>

/* =========================================================
 * readn / writen — garantem leitura/escrita total
 * ========================================================= */
ssize_t readn(int fd, void *buf, size_t count) {
    size_t total = 0;
    char *p = (char *)buf;

    while (total < count) {
        ssize_t n = read(fd, p + total, count - total);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) break;  /* EOF */
        total += n;
    }
    return (ssize_t)total;
}

ssize_t writen(int fd, const void *buf, size_t count) {
    size_t total = 0;
    const char *p = (const char *)buf;

    while (total < count) {
        ssize_t n = write(fd, p + total, count - total);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        total += n;
    }
    return (ssize_t)total;
}

/* =========================================================
 * create_server_socket
 *
 * Cria um socket AF_UNIX do tipo SOCK_STREAM, faz bind()
 * no path SOCKET_PATH e coloca-o em modo listen().
 * ========================================================= */
int create_server_socket(void) {

    /* Remover socket antigo se existir */
    unlink(SOCKET_PATH);

    /* Criar socket */
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    /* Configurar o endereço */
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    /* Bind — associar o socket ao path */
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }

    /* Listen — aceitar até 64 ligações pendentes */
    if (listen(fd, 64) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }

    return fd;
}

/* =========================================================
 * accept_client
 *
 * Aceita uma ligação de um processo filho.
 * ========================================================= */
int accept_client(int server_fd) {
    int client_fd = accept(server_fd, NULL, NULL);
    if (client_fd < 0) {
        perror("accept");
        return -1;
    }
    return client_fd;
}

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