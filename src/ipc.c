#include "ipc.h"
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h> 

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

/* =========================================================
 * readn - Lê exatamente 'n' bytes do descritor 'fd'
 * ========================================================= */
ssize_t readn(int fd, void *ptr, size_t n) {
    size_t nleft = n;
    ssize_t nread;
    char *p = ptr;

    while (nleft > 0) {
        if ((nread = read(fd, p, nleft)) < 0) {
            if (errno == EINTR) {
                nread = 0;  /* Interrompido por um sinal, tenta de novo */
            } else {
                return -1;  /* Erro real */
            }
        } else if (nread == 0) {
            break;          /* Fim do ficheiro (EOF) - a ligação fechou */
        }

        nleft -= nread;
        p += nread;
    }
    return (n - nleft);     /* Retorna quantos bytes leu na realidade */
}

/* =========================================================
 * writen - Escreve exatamente 'n' bytes no descritor 'fd'
 * ========================================================= */
ssize_t writen(int fd, const void *ptr, size_t n) {
    size_t nleft = n;
    ssize_t nwritten;
    const char *p = ptr;

    while (nleft > 0) {
        if ((nwritten = write(fd, p, nleft)) <= 0) {
            if (nwritten < 0 && errno == EINTR) {
                nwritten = 0; /* Interrompido por um sinal, tenta de novo */
            } else {
                return -1;    /* Erro real */
            }
        }

        nleft -= nwritten;
        p += nwritten;
    }
    return n;
}