// Minimal C++ heap runtime (no libstdc++ is linked).
//
// operator new forwards to newlib's malloc (backed by _sbrk over the
// ram_a heap span in the SDK memmap) and returns NULL on failure instead
// of throwing - we build with -fno-exceptions.  CXXFLAGS adds -fcheck-new
// so a NULL result skips the constructor rather than running it on a
// null object; callers keep the classic embedded style of checking the
// pointer (the ported TUI code already does).

#include <stddef.h>

extern "C" {
#include "uart.h"
// g++'s freestanding <stdlib.h> shadows newlib's and omits the heap
void *malloc(size_t size);
void free(void *p);
}

void *operator new(size_t size)          { return malloc(size); }
void *operator new[](size_t size)        { return malloc(size); }
void operator delete(void *p)            { free(p); }
void operator delete[](void *p)          { free(p); }

// Sized deallocation (C++14): the compiler emits these for delete of
// complete types with known size.
void operator delete(void *p, size_t)    { free(p); }
void operator delete[](void *p, size_t)  { free(p); }

// Reached through a vtable slot if a pure virtual is called during
// construction/destruction.  Report and park rather than wander off
// through a garbage pointer.
extern "C" void __cxa_pure_virtual(void)
{
    uart_printf("FATAL: pure virtual call\r\n");
    for (;;)
        ;
}
