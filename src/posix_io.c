#include "posix_io.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>

/**
 * @brief Escreve exactamente @p len bytes em @p fd, repetindo write() se necessário.
 * @param fd     Descritor de destino.
 * @param buffer Dados a escrever.
 * @param len    Número de bytes a escrever.
 * @return Total de bytes escritos, ou -1 em erro.
 */
static ssize_t posix_write_all(int fd, const char *buffer, size_t len) {
    size_t total = 0;

    while (total < len) {
        ssize_t written = write(fd, buffer + total, len - total);
        if (written == -1 && errno == EINTR) {
            continue;
        }
        if (written == -1) {
            return -1;
        }
        total += (size_t)written;
    }

    return (ssize_t)total;
}

/**
 * @brief Formata uma string e escreve-a num descritor POSIX sem usar stdio.
 * @param fd  Descritor de ficheiro de destino.
 * @param fmt String de formato estilo printf.
 * @param ... Argumentos variáveis.
 * @return Número de bytes escritos, ou -1 em erro de formatação ou escrita.
 */
ssize_t posix_writef(int fd, const char *fmt, ...) {
    char buffer[4096];
    va_list args;

    va_start(args, fmt);
    int len = vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    if (len < 0) {
        return -1;
    }

    if ((size_t)len >= sizeof(buffer)) {
        len = (int)sizeof(buffer) - 1;
    }

    return posix_write_all(fd, buffer, (size_t)len);
}
