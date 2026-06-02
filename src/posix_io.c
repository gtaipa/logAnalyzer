/**
 * @file posix_io.c
 * @brief Primitivas de escrita segura sobre file descriptors POSIX.
 *
 * @details
 * Em POSIX, write(2) pode retornar menos bytes que os pedidos — situação normal
 * quando o buffer do kernel está cheio ou quando um sinal interrompe a syscall.
 * Este módulo fornece dois wrappers que tratam ambos os casos:
 *
 *  - **posix_write_all()** (estático) — loop de write(2) até o buffer estar
 *    totalmente consumido, com retry automático em EINTR.
 *
 *  - **posix_writef()** (público) — formata uma string com vsnprintf e delega
 *    a escrita a posix_write_all(), garantindo atomicidade lógica da mensagem.
 *
 * Estas funções são usadas pelos workers para comunicar com o dashboard sem
 * recorrer a stdio (que usa buffers internos incompatíveis com fork(2)).
 */

#include "posix_io.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>

/**
 * @brief Escreve exactamente @p len bytes para @p fd, repetindo em caso de escrita parcial.
 *
 * @details write(2) pode aceitar menos bytes que os pedidos quando o buffer do kernel
 * está quase cheio (short write). O loop garante que todos os bytes são enviados,
 * avançando o cursor de @p buffer a cada iteração pelo número real de bytes aceites.
 * EINTR ocorre quando um sinal interrompe a syscall antes de qualquer byte ser escrito;
 * nesse caso é seguro repetir imediatamente sem incrementar @p total.
 *
 * @param fd     File descriptor de destino.
 * @param buffer Ponteiro para o início dos dados a escrever.
 * @param len    Número de bytes a escrever.
 * @return Número total de bytes escritos (== @p len em caso de sucesso), ou -1 em erro real.
 */
static ssize_t posix_write_all(int fd, const char *buffer, size_t len) {
    size_t total = 0;

    while (total < len) {
        ssize_t written = write(fd, buffer + total, len - total);

        if (written == -1 && errno == EINTR) {
            /* Sinal interrompeu a syscall antes de qualquer byte escrito; repetir */
            continue;
        }
        if (written == -1) {
            return -1; /* erro real (EPIPE, EBADF, etc.) — propagar ao chamador */
        }
        total += (size_t)written;
    }

    return (ssize_t)total;
}

/**
 * @brief Formata uma string printf-like e escreve-a atomicamente num file descriptor.
 *
 * @details Usa vsnprintf para renderizar a string num buffer de 4 KiB na stack;
 * se o resultado exceder esse limite, é truncado ao máximo seguro antes de ser
 * enviado. Desta forma a função nunca aloca memória dinâmica e é segura para uso
 * em contextos pós-fork (antes de exec), onde malloc é desaconselhado.
 *
 * @param fd  File descriptor de destino (ex.: STDOUT_FILENO, STDERR_FILENO).
 * @param fmt String de formato no estilo printf (não pode ser NULL).
 * @param ... Argumentos variáveis correspondentes às directivas de formato.
 * @return Número de bytes escritos, ou -1 se vsnprintf falhar.
 */
ssize_t posix_writef(int fd, const char *fmt, ...) {
    char buffer[4096];
    va_list args;

    va_start(args, fmt);
    int len = vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    if (len < 0) {
        return -1; /* vsnprintf falhou (fmt inválido ou erro de codificação) */
    }

    /* Truncar silenciosamente se a mensagem exceder 4095 caracteres */
    if ((size_t)len >= sizeof(buffer)) {
        len = (int)sizeof(buffer) - 1;
    }

    return posix_write_all(fd, buffer, (size_t)len);
}
