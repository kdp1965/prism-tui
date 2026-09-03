/* Minimal <sys/ioctl.h> for the ported terminal stack (newlib has none).
 * ioctl() itself always fails on bare metal (tqv_termio.c), which makes
 * termcurses fall back to the VT100 escape queries - exactly what we
 * want over a UART.
 */

#pragma once

#include <stdint.h>

struct winsize
{
  uint16_t ws_row;
  uint16_t ws_col;
  uint16_t ws_xpixel;
  uint16_t ws_ypixel;
};

#define TIOCGWINSZ 0x5413

#ifdef __cplusplus
extern "C" {
#endif

int ioctl(int fd, int req, unsigned long arg);

#ifdef __cplusplus
}
#endif
