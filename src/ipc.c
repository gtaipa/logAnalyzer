#include "ipc.h"
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h> 

/* =========================================================
 * connect_to_server - Liga-se ao servidor (pai) via Unix Domain Socket
 *
 * Utilizado pelo filho para estabelecer comunicação com o pai.
 * Tenta várias vezes para aguardar que o pai esteja pronto.
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
