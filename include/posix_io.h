#ifndef POSIX_IO_H
#define POSIX_IO_H

#include <sys/types.h>

ssize_t posix_writef(int fd, const char *fmt, ...);

#endif /* POSIX_IO_H */
