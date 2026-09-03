/****************************************************************************
 * tui/platform/pdcnuttx.h
 *
 * TinyQV port of apps/graphics/pdcurs34/nuttx/pdcnuttx.h: the framebuffer
 * (NX graphics) half is gone - this build only supports the termcurses
 * path (CONFIG_SYSTEM_TERMCURSES), rendering through VT100 escapes over
 * the UART.  The termcurses-side structures are unchanged from the
 * original so the platform .c files port verbatim.
 *
 *   Copyright (C) 2017, 2019 Gregory Nutt. All rights reserved.
 *   Author: Gregory Nutt <gnutt@nuttx.org>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name NuttX nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

#ifndef __TUI_PLATFORM_PDCNUTTX_H
#define __TUI_PLATFORM_PDCNUTTX_H 1

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include "nuttx/config.h"

#include <stdint.h>

#include "curspriv.h"
#include <system/termcurses.h>

#ifndef CONFIG_SYSTEM_TERMCURSES
#  error This port only supports the termcurses path
#endif

/****************************************************************************
 * Private Types
 ****************************************************************************/

#if defined(__cplusplus)
extern "C"
{
#  define EXTERN extern "C"
#else
#  define EXTERN extern
#endif

/* Describes one color pair */

struct pdc_colorpair_s
{
  short fg;
  short bg;
};

/* Describes one RGB color */

struct pdc_rgbcolor_s
{
  uint8_t red;
  uint8_t green;
  uint8_t blue;
};

/* This structure provides the overall state of the termcurses device */

struct pdc_termstate_s
{
  /* Terminal fd numbers (typcially 0 and 1) */

  int    out_fd;
  int    in_fd;

  /* Colors */

  short  fg_red;
  short  fg_green;
  short  fg_blue;
  short  bg_red;
  short  bg_green;
  short  bg_blue;

#ifdef CONFIG_PDCURSES_CHTYPE_LONG
  long   attrib;
#else
  short  attrib;
#endif

  struct pdc_colorpair_s colorpair[PDC_COLOR_PAIRS];
  struct pdc_rgbcolor_s rgbcolor[16];

  FAR struct termcurses_s *tcurs;
};

/* This structure contains the termstate structure and is a cast
 * compatible with type SCREEN.
 */

struct pdc_termscreen_s
{
  SCREEN screen;
  struct pdc_termstate_s termstate;
};

/* Dead-code support: some platform functions branch between the
 * termcurses path (early return, graphic_screen is always false here)
 * and unguarded framebuffer code that still has to compile.  These
 * minimal definitions satisfy that dead code; nothing initializes or
 * dereferences them at runtime.
 */

typedef int16_t fb_coord_t;

struct pdc_fbstate_s
{
  int fbfd;
  FAR void *fbmem;
  fb_coord_t xres;
  fb_coord_t yres;
  fb_coord_t stride;
  uint8_t fheight;
  uint8_t fwidth;
};

struct pdc_fbscreen_s
{
  SCREEN screen;
  struct pdc_fbstate_s fbstate;
};

#ifndef DEBUGASSERT
#  define DEBUGASSERT(x) ((void)(x))
#endif

/* Input open/close (pdckbd.c): with TERMINPUT these are no-ops but the
 * prototypes are still referenced from pdcscrn.c.
 */

#ifdef CONFIG_PDCURSES_HAVE_INPUT
int PDC_input_open(FAR struct pdc_fbstate_s *fbstate);
void PDC_input_close(FAR struct pdc_fbstate_s *fbstate);
#endif

#undef EXTERN
#if defined(__cplusplus)
}
#endif

#endif /* __TUI_PLATFORM_PDCNUTTX_H */
