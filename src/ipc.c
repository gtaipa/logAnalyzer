/**
 * @file ipc.c
 * @brief Comunicação entre processos via Unix Domain Socket e pipes.
 *
 * @details
 * Este ficheiro implementa as primitivas de IPC (Inter-Process Communication)
 * usadas entre o processo pai (servidor) e os processos filho (workers).
 *
 * ## Por que read()/write() podem ser "parciais"?
 *
 * Em Linux, `read()` e `write()` sobre pipes e sockets **não garantem** que
 * todos os bytes pedidos sejam transferidos numa única chamada ao sistema.
 * Isto acontece porque:
 *
 *  - O **pipe** tem um buffer interno no kernel (tipicamente 64 KB em Linux).
 *    Se o buffer estiver quase cheio (escrita) ou quase vazio (leitura), a
 *    syscall retorna apenas os bytes disponíveis.
 *
 *  - O **socket** pode estar sujeito a controlo de fluxo TCP/socket: o kernel
 *    pode aceitar menos bytes do que os pedidos se o buffer de envio estiver
 *    cheio, ou devolver menos do que o pedido se os dados ainda não chegaram.
 *
 *  - Para **pipes**: `write()` é atómico apenas para escritas ≤ PIPE_BUF
 *    (4096 bytes em Linux). Para escritas maiores, pode ser partida.
 *
 * ## O que é EINTR e por que precisa de tratamento especial?
 *
 * `EINTR` (Interrupted System Call) ocorre quando uma **chamada de sistema
 * bloqueante** (como `read()` ou `write()`) é interrompida pela entrega de
 * um sinal Unix (ex.: SIGCHLD quando um filho termina, SIGALRM de um timer,
 * SIGUSR1 definido pela aplicação).
 *
 * Quando `EINTR` ocorre:
 *  - A syscall não fez progressso — **zero bytes** foram lidos/escritos.
 *  - `errno` fica com o valor `EINTR`.
 *  - A syscall deve ser **repetida** — não é um erro real.
 *
 * Se não tratarmos `EINTR`, a função retornaria -1 prematuramente, causando
 * perda de dados ou falha de comunicação sem motivo real.
 *
 * ## Como writen() garante escrita completa?
 *
 * `writen()` usa um ciclo que:
 *  1. Chama `write(fd, buf_ptr, nleft)` — escreve o que puder.
 *  2. Se retornar -1 com `errno == EINTR`, repete sem avançar.
 *  3. Se retornar -1 com outro erro, falha imediatamente.
 *  4. Se retornar 0, trata como erro para evitar loop infinito.
 *  5. Se retornar N > 0, avança `buf_ptr` em N bytes e desconta N de `nleft`.
 *  6. Repete até `nleft == 0` (todos os bytes foram escritos).
 *
 * Funções públicas:
 *  - `connect_to_server()` — liga ao servidor via Unix Domain Socket
 *  - `readn()`             — leitura garantida de N bytes
 *  - `writen()`            — escrita garantida de N bytes
 */

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

/**
 * @brief Liga ao processo servidor através de um Unix Domain Socket.
 *
 * @details Os Unix Domain Sockets (AF_UNIX / AF_LOCAL) são a forma mais
 * eficiente de IPC em Linux quando os processos estão na mesma máquina:
 * os dados passam directamente pelo kernel sem overhead de rede (sem TCP/IP).
 *
 * O socket é identificado por um caminho no sistema de ficheiros definido
 * pela constante `SOCKET_PATH`. O servidor (processo pai) deve ter chamado
 * `bind()` + `listen()` antes de esta função ser chamada.
 *
 * Para lidar com a condição de corrida em que o filho tenta ligar-se antes
 * de o pai ter feito `bind()`, a função tenta até 10 vezes com 50ms de
 * espera entre tentativas (total máximo: 500ms).
 *
 * @return File descriptor do socket ligado em caso de sucesso; -1 em caso
 *         de erro (socket não criado ou todas as tentativas falharam).
 */
int connect_to_server(void) {

    /* Criar um socket do tipo SOCK_STREAM (orientado a ligação, como TCP)
     * mas no domínio AF_UNIX (comunicação local, sem rede) */
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    /* Preencher a estrutura de endereço com o caminho do socket no filesystem */
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

    /* Todas as tentativas falharam — reportar o último erro e limpar */
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

/**
 * @brief Lê exactamente @p nbytes bytes de um file descriptor.
 *
 * @details Esta função é necessária porque `read()` sobre um pipe ou socket
 * pode devolver menos bytes do que os pedidos — isto chama-se **leitura
 * parcial** e é um comportamento normal e esperado do kernel Linux.
 *
 * Causas de leitura parcial:
 *  - O buffer do pipe/socket no kernel tem menos dados disponíveis.
 *  - Um sinal foi entregue durante a syscall bloqueante (EINTR).
 *  - O escritor fez `write()` por partes.
 *
 * O ciclo garante que a leitura continua até um dos três critérios de paragem:
 *  1. Todos os `nbytes` foram lidos (sucesso completo).
 *  2. EOF — o escritor fechou o pipe/socket (retorna bytes lidos até agora).
 *  3. Erro real (não-EINTR) — retorna -1.
 *
 * @param fd     File descriptor a ler (pipe, socket, etc.).
 * @param ptr    Buffer de destino com capacidade mínima de @p nbytes bytes.
 * @param nbytes Número de bytes a ler.
 * @return Número de bytes efectivamente lidos (pode ser < @p nbytes em caso
 *         de EOF prematuro); -1 em caso de erro.
 */
ssize_t readn(int fd, void *ptr, size_t nbytes) {
    /* 1. Preparar o contador de bytes em falta e um ponteiro byte-a-byte
     *    para avançar no buffer do chamador. */
    size_t nleft = nbytes;
    char *buf_ptr = ptr;

    /* 2. O ciclo existe porque read() em pipes/sockets não garante entregar
     *    todos os nbytes numa única chamada. */
    while (nleft > 0) {
        ssize_t nread = read(fd, buf_ptr, nleft);

        /* 3. Se read() foi interrompido por um sinal, errno fica EINTR;
         *    não é erro lógico, repetimos a leitura sem alterar o estado.
         *    Exemplo: SIGCHLD chega enquanto esperamos dados do filho. */
        if (nread == -1 && errno == EINTR) {
            continue;
        }

        /* 4. Qualquer outro valor negativo representa erro real do sistema
         *    (ex.: EBADF, EIO), logo a função sinaliza falha com -1. */
        if (nread == -1) {
            return -1;
        }

        /* 5. read() devolver 0 significa EOF: o escritor fechou o pipe/socket
         *    antes de enviar todos os bytes. Saímos do ciclo e retornamos
         *    o que já foi lido (pode ser menos do que nbytes). */
        if (nread == 0) {
            break;
        }

        /* 6. Como a leitura pode ser parcial, descontamos os bytes recebidos
         *    e avançamos o ponteiro para a próxima posição do buffer. */
        nleft -= (size_t)nread;
        buf_ptr += nread;
    }

    /* 7. Retornar o total efectivamente lido permite ao chamador distinguir
     *    leitura completa (retorno == nbytes) de EOF prematuro (retorno < nbytes). */
    return (ssize_t)(nbytes - nleft);
}

/* =========================================================
 * writen - Escreve exatamente nbytes de forma segura em fd.
 *
 * Em pipes e sockets, write() tambem pode aceitar apenas parte
 * dos bytes pedidos. Esta funcao continua a escrever ate enviar
 * tudo, ou ate encontrar um erro real que impossibilite continuar.
 * ========================================================= */

/**
 * @brief Escreve exactamente @p nbytes bytes num file descriptor.
 *
 * @details Esta função garante a **escrita completa** mesmo quando `write()`
 * faz apenas escrita parcial — comportamento normal em pipes e sockets.
 *
 * Causas de escrita parcial:
 *  - O buffer de escrita do pipe/socket no kernel está quase cheio.
 *  - Para pipes, `write()` só é atómico para N ≤ PIPE_BUF (4096 bytes).
 *    Para mensagens maiores, o kernel pode partir a escrita.
 *  - Um sinal foi entregue durante a syscall bloqueante (EINTR).
 *
 * O ciclo avança no buffer a cada iteração, garantindo que todos os bytes
 * são eventualmente entregues ao kernel:
 *  - `buf_ptr` avança pelos bytes já escritos.
 *  - `nleft` conta os bytes que ainda faltam escrever.
 *  - O ciclo termina quando `nleft == 0` (todos escritos) ou erro.
 *
 * @param fd     File descriptor de destino (pipe, socket, etc.).
 * @param ptr    Buffer de origem com pelo menos @p nbytes bytes.
 * @param nbytes Número de bytes a escrever.
 * @return @p nbytes em caso de sucesso total; -1 em caso de erro.
 */
ssize_t writen(int fd, void *ptr, size_t nbytes) {
    /* 1. Preparar o contador de bytes por escrever e um ponteiro para a
     *    próxima posição a enviar no buffer. */
    size_t nleft = nbytes;
    char *buf_ptr = ptr;

    /* 2. O ciclo é obrigatório porque write() pode fazer uma escrita parcial
     *    em pipes/sockets — não podemos assumir que todos os bytes saíram. */
    while (nleft > 0) {
        ssize_t nwritten = write(fd, buf_ptr, nleft);

        /* 3. Se write() foi interrompido por um sinal, errno == EINTR;
         *    repetimos sem perder posição no buffer — nenhum byte saiu
         *    quando EINTR ocorre. */
        if (nwritten == -1 && errno == EINTR) {
            continue;
        }

        /* 4. Um erro negativo diferente de EINTR indica falha real:
         *    EPIPE (leitor fechou o pipe), EBADF (fd inválido), etc. */
        if (nwritten == -1) {
            return -1;
        }

        /* 5. write() devolver 0 não faz progresso; tratamos como falha
         *    para evitar um ciclo infinito. Sinalizar com EPIPE é adequado
         *    porque indica que o canal de comunicação está encerrado. */
        if (nwritten == 0) {
            errno = EPIPE;
            return -1;
        }

        /* 6. Como a escrita pode ser parcial, descontamos o que saiu
         *    e avançamos para o próximo byte a enviar. */
        nleft -= (size_t)nwritten;
        buf_ptr += nwritten;
    }

    /* 7. Se o ciclo terminou normalmente, todos os nbytes foram escritos
     *    com sucesso no descritor. */
    return (ssize_t)nbytes;
}
