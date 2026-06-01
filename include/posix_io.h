/**
 * @file posix_io.h
 * @brief Utilitário de escrita formatada em file descriptors POSIX.
 *
 * Fornece posix_writef(), uma função análoga a fprintf() mas que opera
 * diretamente sobre file descriptors (int fd) em vez de streams FILE*,
 * sendo por isso adequada para uso em código que utilize apenas chamadas
 * de sistema POSIX (open/write/close) sem buffering de stdio.
 */

#ifndef POSIX_IO_H
#define POSIX_IO_H

#include <sys/types.h>

/**
 * @brief Escreve uma string formatada de forma segura num file descriptor POSIX.
 *
 * Formata a string de acordo com @p fmt (semelhante a printf) e escreve
 * o resultado no file descriptor @p fd usando write(). A função lida
 * internamente com escritas parciais, repetindo a operação até que todos
 * os bytes tenham sido escritos ou ocorra um erro irrecuperável.
 *
 * @param fd  File descriptor de destino (ex.: STDOUT_FILENO, um pipe ou socket).
 * @param fmt String de formato (sintaxe printf); seguida de argumentos variádicos.
 * @param ... Argumentos variádicos correspondentes aos especificadores em @p fmt.
 * @return Número total de bytes escritos em caso de sucesso; -1 em caso de erro.
 */
ssize_t posix_writef(int fd, const char *fmt, ...);

#endif /* POSIX_IO_H */
