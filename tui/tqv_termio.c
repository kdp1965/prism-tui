// POSIX terminal I/O shim for the ported termcurses/pdcurses stack.
//
// The originals (nuttx_riscv/apps) talk to the terminal through file
// descriptors; on TinyQV the "terminal" is the UART, full stop.  This
// file provides the five syscalls they use - read/write/select/fcntl/
// usleep (plus a failing ioctl) - over the SDK UART driver, so the
// library sources compile essentially verbatim.  Any fd maps to the
// UART; there are no files here.
//
// read() honors O_NONBLOCK set via fcntl (termcurses toggles it around
// the ESC[6n window-size query); select() only ever watches terminal
// input, so it just polls uart_is_char_available() against the µs
// timebase.

#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/ioctl.h>

#include "uart.h"
#include "csr.h"

static int tqv_fd_flags;        // O_NONBLOCK is the only bit honored

ssize_t write(int fd, const void *buf, size_t nbytes)
{
    (void)fd;
    uart_put_buffer((const char *)buf, (int)nbytes);
    return (ssize_t)nbytes;
}

ssize_t read(int fd, void *buf, size_t nbytes)
{
    char *p = (char *)buf;
    size_t got = 0;

    (void)fd;
    if (nbytes == 0)
        return 0;

    if (!uart_is_char_available()) {
        if (tqv_fd_flags & O_NONBLOCK) {
            errno = EAGAIN;
            return -1;
        }
        while (!uart_is_char_available())
            ;
    }

    // First byte is guaranteed now; drain whatever else has arrived
    do {
        p[got++] = (char)uart_getc();
    } while (got < nbytes && uart_is_char_available());

    return (ssize_t)got;
}

int select(int nfds, fd_set *readfds, fd_set *writefds,
           fd_set *exceptfds, struct timeval *timeout)
{
    uint32_t start = read_time();
    uint32_t limit_us = 0;

    (void)nfds;
    (void)writefds;
    (void)exceptfds;

    if (timeout != NULL)
        limit_us = (uint32_t)timeout->tv_sec * 1000000u
                 + (uint32_t)timeout->tv_usec;

    for (;;) {
        if (uart_is_char_available())
            return 1;                      // readfds already has the fd set
        if (timeout != NULL && (uint32_t)(read_time() - start) >= limit_us)
            break;
    }

    if (readfds != NULL)
        FD_ZERO(readfds);
    return 0;
}

int fcntl(int fd, int cmd, ...)
{
    __builtin_va_list ap;
    int arg;

    (void)fd;
    switch (cmd) {
    case F_GETFL:
        return tqv_fd_flags;
    case F_SETFL:
        __builtin_va_start(ap, cmd);
        arg = __builtin_va_arg(ap, int);
        __builtin_va_end(ap);
        tqv_fd_flags = arg;
        return 0;
    default:
        errno = EINVAL;
        return -1;
    }
}

int usleep(useconds_t usec)
{
    uint32_t start = read_time();

    while ((uint32_t)(read_time() - start) < (uint32_t)usec)
        ;
    return 0;
}

int ioctl(int fd, int req, unsigned long arg)
{
    // No tty layer: always fail so termcurses falls back to the VT100
    // escape query for the window size.
    (void)fd;
    (void)req;
    (void)arg;
    errno = ENOTTY;
    return -1;
}
