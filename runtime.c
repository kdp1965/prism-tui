// Much of this borrowed from the Pico SDK
// Copyright (c) 2020 Raspberry Pi (Trading) Ltd.

#include <sys/time.h>
#include <stdio.h>
#include <sys/stat.h>
#include "uart.h"
#include "gpio.h"

extern char __HeapLimit; /* Set by linker.  */
extern char __HeapStart; /* Set by linker.  */
char* __tinyqv_heap_end = &__HeapStart;

extern uint32_t __bss_start__;
extern uint32_t __bss_end__;
extern uint32_t __data_start__;
extern uint32_t __data_end__;
extern uint32_t __data_lma;
extern uint32_t __peri_data_start__;
extern uint32_t __peri_data_end__;
extern uint32_t __peri_lma;

//int __errno;

void __attribute__((section(".early_text"))) __runtime_init(void) {
    for (uint32_t* ptr = &__bss_start__; ptr < &__bss_end__; ) //*ptr++ = 0;
        asm("sw4n x0, (%[p])\n\t"
            "addi %[p], %[p], 16\n\t" :
            [p] "+r" (ptr));
    
    uint32_t* load_ptr = &__peri_lma;
    for (uint32_t* ptr = &__peri_data_start__; ptr < &__peri_data_end__; ) *ptr++ = *load_ptr++;

    load_ptr = &__data_lma;
    for (uint32_t* ptr = &__data_start__; ptr < &__data_end__; ) //*ptr++ = *load_ptr++;
        asm("lw4 a0, (%[lp])\n\t"
            "sw4 a0, (%[p])\n\t"
            "addi %[lp], %[lp], 16\n\t"
            "addi %[p], %[p], 16\n\t" :
            [p] "+r" (ptr), [lp] "+r" (load_ptr) :
            :
            "a0", "a1", "a2", "a3"
            );
    
    // Change UART CTS pin to out5 instead of out1 to match ETR demo board
    set_gpio_func(5, 2);
    set_gpio_func(1, 1);

    // C++ global constructors (.init_array, empty for pure C builds).
    // Runs after .data/.bss so constructors see initialized globals.
    {
        typedef void (*init_fn)(void);
        extern init_fn __init_array_start[];
        extern init_fn __init_array_end[];
        for (init_fn *f = __init_array_start; f != __init_array_end; ++f)
            (*f)();
    }
}

void *_sbrk(int incr) {
    char *prev_heap_end;

    prev_heap_end = __tinyqv_heap_end;
    char *next_heap_end = __tinyqv_heap_end + incr;

    //uart_printf("SBRK: %p -> %p\r\n", __tinyqv_heap_end, next_heap_end);

    if (next_heap_end > (&__HeapLimit)) {
        return (char *) -1;
    }

    __tinyqv_heap_end = next_heap_end;
    return (void *) prev_heap_end;
}

int _gettimeofday (struct timeval *__restrict tv, void *__restrict tz) {
    return 0;
}

pid_t _getpid(void) {
    return 0;
}

int _kill(__unused pid_t pid, __unused int sig) {
    return -1;
}

void __attribute__((noreturn)) _exit(__unused int status) {
    while (1) {
        //__breakpoint();
    }
}

#define STDIO_HANDLE_STDIN  0
#define STDIO_HANDLE_STDOUT 1
#define STDIO_HANDLE_STDERR 2
#define STDIO_HANDLE_FILE   3   /* first fd served by the host filesystem */

/* Optional host filesystem (tqv_fs.c, served by tqv.py's console).  Weak
   so apps that never link it keep the old "no files here" behavior; when
   it is linked its strong definitions take over and fopen/fgets/fprintf
   start working on descriptors 3 and up. */
int __attribute__((weak)) __tinyqv_fs_open(__unused const char *path,
                                           __unused int flags) {
    return -1;
}

int __attribute__((weak)) __tinyqv_fs_close(__unused int fd) {
    return -1;
}

int __attribute__((weak)) __tinyqv_fs_read(__unused int fd,
                                           __unused char *buffer,
                                           __unused int length) {
    return -1;
}

int __attribute__((weak)) __tinyqv_fs_write(__unused int fd,
                                            __unused const char *buffer,
                                            __unused int length) {
    return -1;
}

long __attribute__((weak)) __tinyqv_fs_lseek(__unused int fd,
                                             __unused long offset,
                                             __unused int whence) {
    return -1;
}

int _read(int handle, char *buffer, int length) {
    if (handle == STDIO_HANDLE_STDIN) {
        for (int i = 0; i < length; ++i) {
            while (!uart_is_char_available())
                ;
            *buffer++ = uart_getc();
        }
        return length;
    }
    else if (handle >= STDIO_HANDLE_FILE) {
        return __tinyqv_fs_read(handle, buffer, length);
    }
    return -1;
}

// Optional stdout redirection: when set, printf() output goes through
// the hook instead of the UART.  A full-screen UI can install one
// to pre-process output so output lands in its UI management code
// instead of corrupting the display.
int (*__tinyqv_stdout_hook)(const char *buffer, int length) = NULL;

int _write(int handle, char *buffer, int length) {
    if (handle == STDIO_HANDLE_STDOUT) {
        if (__tinyqv_stdout_hook)
            return __tinyqv_stdout_hook(buffer, length);
        uart_put_buffer(buffer, length);
        return length;
    }
    else if (handle == STDIO_HANDLE_STDERR) {
        debug_uart_put_buffer(buffer, length);
        return length;
    }
    else if (handle >= STDIO_HANDLE_FILE) {
        return __tinyqv_fs_write(handle, buffer, length);
    }
    return -1;
}

int _open(const char *fn, int oflag, ...) {
    return __tinyqv_fs_open(fn, oflag);
}

int _close(int fd) {
    if (fd >= STDIO_HANDLE_FILE)
        return __tinyqv_fs_close(fd);
    return -1;
}

off_t _lseek(int fd, off_t pos, int whence) {
    if (fd >= STDIO_HANDLE_FILE)
        return (off_t)__tinyqv_fs_lseek(fd, (long)pos, whence);
    return -1;
}

int __attribute__((weak)) _fstat(__unused int fd, __unused struct stat *buf) {
    return -1;
}

int __attribute__((weak)) _isatty(int fd) {
    return fd == STDIO_HANDLE_STDIN || fd == STDIO_HANDLE_STDOUT || fd == STDIO_HANDLE_STDERR;
}
