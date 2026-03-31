#include "ipc.h"
#include <errno.h>
#include <unistd.h>

ssize_t readn(int fd, void *buf, size_t count) {
    size_t total = 0;
    char *p = (char *)buf;

    while (total < count) {
        ssize_t n = read(fd, p + total, count - total);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) {
            break;  /* EOF */
        }
        total += n;
    }
    return (ssize_t)total;
}

ssize_t writen(int fd, const void *buf, size_t count) {
    size_t total = 0;
    const char *p = (const char *)buf;

    while (total < count) {
        ssize_t n = write(fd, p + total, count - total);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        total += n;
    }
    return (ssize_t)total;
}
