#ifndef POSIX_IO_H
#define POSIX_IO_H

#include <sys/types.h>

/* Escreve uma string formatada de forma segura em um file descriptor */
ssize_t posix_writef(int fd, const char *fmt, ...);

#endif /* POSIX_IO_H */
