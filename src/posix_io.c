/**
 * @file posix_io.c
 * @brief Escrita formatada segura para file descriptors POSIX.
 *
 * @details
 * Este ficheiro implementa `posix_writef()`, um equivalente seguro de
 * `fprintf()` para file descriptors POSIX brutos (não `FILE*`).
 *
 * ## Por que não usar printf() / fprintf() directamente?
 *
 * `printf()` e `fprintf()` escrevem para streams `FILE*` com buffer gerido
 * pela libc. Em cenários de IPC com pipes e sockets, o código usa file
 * descriptors inteiros (retornados por `open()`, `socket()`, `pipe()`,
 * `accept()`, etc.) — não streams `FILE*`. Não é possível passar um fd
 * directamente para `fprintf()` sem criar um wrapper com `fdopen()`, que
 * introduz buffering e pode causar problemas em ambientes multi-processo
 * (o buffer da libc não é partilhado entre processos após `fork()`).
 *
 * ## Arquitectura de posix_writef()
 *
 * ```
 *  posix_writef(fd, fmt, ...)
 *      │
 *      ├─ vsnprintf(buffer, 4096, fmt, args)
 *      │   └── formata a string em memória (stack), sem I/O
 *      │
 *      └─ posix_write_all(fd, buffer, len)
 *          └── ciclo write() até todos os bytes serem enviados
 *                (trata escrita parcial e EINTR)
 * ```
 *
 * ## Tratamento de escrita parcial e EINTR
 *
 * `write()` pode escrever menos bytes do que os pedidos (escrita parcial)
 * por várias razões (ver ipc.c para explicação detalhada). A função interna
 * `posix_write_all()` usa um ciclo para garantir que todos os bytes da
 * string formatada chegam ao file descriptor de destino.
 *
 * `EINTR` (sinal entregue durante syscall bloqueante) é tratado com
 * `continue` — repete a chamada sem perder posição no buffer.
 */

#include "posix_io.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>

/**
 * @brief Escreve todos os bytes de um buffer num file descriptor, tratando
 *        escrita parcial e interrupções por sinal (EINTR).
 *
 * @details Esta função interna (static) é o núcleo de `posix_writef()`.
 * Usa um ciclo com `write()` porque em pipes e sockets uma única chamada
 * pode não enviar todos os bytes pedidos:
 *
 *  - Se `write()` retornar -1 com `errno == EINTR`: um sinal foi entregue
 *    durante a syscall bloqueante; não é erro, repetir imediatamente.
 *  - Se `write()` retornar -1 com outro errno: erro real (EPIPE, EBADF...).
 *  - Se `write()` retornar N > 0: avançar `N` bytes no buffer e continuar.
 *
 * O ponteiro `buffer + total` garante que cada iteração começa exactamente
 * onde a anterior terminou, sem repetir bytes já enviados.
 *
 * @param fd     File descriptor de destino.
 * @param buffer Buffer com os bytes a escrever.
 * @param len    Número de bytes a escrever.
 * @return Total de bytes escritos em caso de sucesso; -1 em caso de erro.
 */
static ssize_t posix_write_all(int fd, const char *buffer, size_t len) {
    size_t total = 0; /* bytes já escritos com sucesso */

    /* Ciclo até todos os bytes terem sido entregues ao kernel */
    while (total < len) {
        /* Tentar escrever os bytes restantes (len - total) a partir da
         * posição actual no buffer (buffer + total) */
        ssize_t written = write(fd, buffer + total, len - total);

        /* EINTR: sinal entregue durante o write() bloqueante.
         * Nenhum byte foi escrito; repetir sem alterar 'total'. */
        if (written == -1 && errno == EINTR) {
            continue;
        }

        /* Erro real (EPIPE, EBADF, EIO, etc.): falhar imediatamente */
        if (written == -1) {
            return -1;
        }

        /* Acumular os bytes escritos e avançar para a próxima posição */
        total += (size_t)written;
    }

    return (ssize_t)total;
}

/**
 * @brief Escreve uma string formatada (estilo printf) num file descriptor POSIX.
 *
 * @details Equivalente seguro de `dprintf()` (disponível em POSIX.1-2008)
 * para situações em que se quer controlo explícito sobre o tratamento de
 * escrita parcial e EINTR.
 *
 * Funcionamento em dois passos:
 *  1. **Formatação em memória**: `vsnprintf()` escreve a string formatada
 *     num buffer de 4096 bytes no stack — sem qualquer I/O.
 *  2. **Escrita segura**: `posix_write_all()` garante que todos os bytes
 *     do buffer chegam ao file descriptor, mesmo que sejam necessárias
 *     várias chamadas a `write()`.
 *
 * Limitações:
 *  - O buffer interno tem 4096 bytes. Mensagens mais longas são truncadas
 *    silenciosamente (o excesso detectado via `len >= sizeof(buffer)` é
 *    cortado ao máximo disponível).
 *  - Não usa o heap — adequado para uso em signal handlers simples.
 *
 * @param fd  File descriptor de destino (pipe, socket, stdout=1, stderr=2, etc.).
 * @param fmt String de formato estilo printf (pode ser NULL → erro).
 * @param ... Argumentos variáveis correspondentes ao formato.
 * @return Número de bytes escritos em caso de sucesso; -1 em caso de erro
 *         de formatação ou de escrita.
 */
ssize_t posix_writef(int fd, const char *fmt, ...) {
    /* Buffer local no stack — evita alocação dinâmica (malloc) */
    char buffer[4096];
    va_list args;

    /* Passo 1: Formatar a string em memória sem qualquer I/O.
     * vsnprintf garante que buffer fica sempre terminado em '\0'. */
    va_start(args, fmt);
    int len = vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    /* vsnprintf retorna -1 em caso de erro de codificação */
    if (len < 0) {
        return -1;
    }

    /* Se a string formatada não cabe no buffer (len >= sizeof(buffer)),
     * truncar ao máximo que o buffer permite (sem o '\0' de terminação) */
    if ((size_t)len >= sizeof(buffer)) {
        len = (int)sizeof(buffer) - 1;
    }

    /* Passo 2: Escrever os bytes formatados no file descriptor,
     * garantindo escrita completa mesmo com escrita parcial ou EINTR */
    return posix_write_all(fd, buffer, (size_t)len);
}
