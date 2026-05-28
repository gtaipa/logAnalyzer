#ifndef POSIX_IO_H
#define POSIX_IO_H

#include <sys/types.h>

/**
 * @brief Escreve uma string formatada num descritor POSIX (sem usar stdio).
 * @param fd  Descritor de ficheiro de destino (ex: STDOUT_FILENO).
 * @param fmt String de formato estilo printf.
 * @param ... Argumentos variáveis para o formato.
 * @return Número de bytes escritos, ou -1 em erro.
 */
ssize_t posix_writef(int fd, const char *fmt, ...);

#endif /* POSIX_IO_H */
