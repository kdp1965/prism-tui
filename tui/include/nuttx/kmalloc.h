/* Stub for NuttX <nuttx/kmalloc.h>: map the kernel allocators onto
 * newlib's heap (ram_a span, see the SDK memmap).
 */

#pragma once

#include <stdlib.h>

#ifndef zalloc
#  define zalloc(s)     calloc(1, (s))
#endif
#define kmm_malloc(s)   malloc(s)
#define kmm_zalloc(s)   calloc(1, (s))
#define kmm_free(p)     free(p)
