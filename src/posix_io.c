#include "posix_io.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>

/* Escreve todos os bytes de forma segura em um file descriptor (trata write parcial) */
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

/* Escreve uma string formatada de forma segura (printf-like) para um fd */
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
