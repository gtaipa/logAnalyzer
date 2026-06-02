/**
 * @file ipc.c
 * @brief Primitivas de IPC (Inter-Process Communication) sobre Unix Domain Sockets.
 *
 * @details
 * Este módulo fornece três funções de baixo nível para comunicação entre processos:
 *
 *  - **connect_to_server()** — cria um socket Unix Domain e liga-se ao servidor pai,
 *    com retry automático caso o servidor ainda não esteja a escutar.
 *
 *  - **readn()** — lê exactamente N bytes de um file descriptor, repetindo read(2)
 *    até completar a leitura ou encontrar EOF/erro.
 *
 *  - **writen()** — escreve exactamente N bytes num file descriptor, repetindo write(2)
 *    até enviar tudo ou encontrar erro real.
 *
 * Tanto readn como writen tratam EINTR (interrupção por sinal) retomando a operação
 * sem incrementar o cursor — comportamento obrigatório em código POSIX robusto que
 * pode correr enquanto sinais como SIGCHLD são entregues assincronamente.
 */

#include "ipc.h"
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>

/**
 * @brief Conecta a um servidor Unix Domain Socket com retry automático.
 *
 * @details O servidor (processo pai) pode ainda não estar a escutar no momento
 * em que o filho chama esta função — race condition inerente ao fork(2) sem
 * sincronização explícita. O ciclo de até 10 tentativas com 50 ms de espera entre
 * elas resolve esse intervalo de arranque sem busy-wait agressivo. 50 ms × 10 = 500 ms
 * é suficiente para qualquer servidor local inicializar o socket.
 *
 * @return File descriptor do socket ligado (≥ 0) em sucesso; -1 em erro.
 */
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

    int tentativas = 10;
    while (tentativas-- > 0) {
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            return fd;
        }
        /* Servidor ainda não está a escutar; aguardar 50 ms antes de tentar de novo */
        usleep(50000);
    }

    perror("connect");
    close(fd);
    return -1;
}

/**
 * @brief Lê exactamente @p nbytes de um file descriptor com retry automático.
 *
 * @details Em pipes e sockets, read(2) pode devolver menos bytes que os pedidos
 * (short read) — comportamento normal quando os dados chegam em fragmentos. O loop
 * garante que todos os bytes são recebidos antes de retornar. EINTR é tratado como
 * caso não-fatal (repetir); qualquer outro errno negativo interrompe a leitura.
 *
 * @param fd     Descritor de ficheiro a ler.
 * @param ptr    Buffer de destino com pelo menos @p nbytes bytes.
 * @param nbytes Número de bytes a ler.
 * @return Número de bytes efectivamente lidos (pode ser < @p nbytes em EOF); -1 em erro.
 */
ssize_t readn(int fd, void *ptr, size_t nbytes) {
    size_t nleft   = nbytes;
    char  *buf_ptr = ptr;

    while (nleft > 0) {
        ssize_t nread = read(fd, buf_ptr, nleft);

        if (nread == -1 && errno == EINTR) {
            /* Sinal interrompeu read(2) antes de qualquer byte lido; repetir */
            continue;
        }
        if (nread == -1) {
            return -1; /* erro real — propagar ao chamador */
        }
        if (nread == 0) {
            break; /* EOF: pipe/socket fechado pelo emissor */
        }

        nleft   -= (size_t)nread;
        buf_ptr += nread;
    }

    return (ssize_t)(nbytes - nleft);
}

/**
 * @brief Escreve exactamente @p nbytes num file descriptor com retry automático.
 *
 * @details Em pipes e sockets, write(2) pode aceitar menos bytes que os pedidos
 * quando o buffer do kernel está quase cheio (short write). O loop repete a escrita
 * do segmento restante até enviar tudo. EINTR é tratado como caso não-fatal; write
 * que retorna 0 bytes é um caso patológico sem errno — tratado como EPIPE para
 * sinalizar que o receptor fechou a ligação.
 *
 * @param fd     Descritor de ficheiro para escrever.
 * @param ptr    Buffer de origem com pelo menos @p nbytes bytes.
 * @param nbytes Número de bytes a escrever.
 * @return Número de bytes escritos (== @p nbytes em sucesso); -1 em erro.
 */
ssize_t writen(int fd, void *ptr, size_t nbytes) {
    size_t nleft   = nbytes;
    char  *buf_ptr = ptr;

    while (nleft > 0) {
        ssize_t nwritten = write(fd, buf_ptr, nleft);

        if (nwritten == -1 && errno == EINTR) {
            /* Sinal interrompeu write(2) antes de qualquer byte escrito; repetir */
            continue;
        }
        if (nwritten == -1) {
            return -1; /* erro real (EPIPE, EBADF, etc.) */
        }
        if (nwritten == 0) {
            /* write(2) retornou 0 sem erro — receptor fechou a ligação */
            errno = EPIPE;
            return -1;
        }

        nleft   -= (size_t)nwritten;
        buf_ptr += nwritten;
    }

    return (ssize_t)nbytes;
}
