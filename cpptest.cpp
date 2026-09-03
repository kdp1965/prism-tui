// C++ smoke test for the TinyQV SDK ('cpptest' command).
//
// Exercises the embedded C++ subset the SDK now supports: global
// constructors (run from .init_array by __runtime_init), virtual
// dispatch, templates, local statics, namespaces, and heap allocation
// (operator new/delete over newlib malloc, see cxxrt.cpp; the heap
// lives in ram_a per the SDK memmap).  Exceptions and RTTI remain
// deliberately unavailable (see CXXFLAGS in the Makefile).

extern "C" {
#include <stdio.h>
#include <stdint.h>
#include <csr.h>
#include "prism_tui.h"
// g++'s freestanding <stdlib.h> shadows newlib's and omits the heap
void *malloc(size_t size);
void free(void *p);
}

extern char __HeapStart;   // SDK memmap: heap span inside ram_a

// --- global constructor: proves .init_array runs before main ----------
namespace {

class BootStamp {
public:
    BootStamp() : magic_(0xC0FFEEu), boot_us_(read_time()) { }
    unsigned magic() const { return magic_; }
    unsigned boot_us() const { return boot_us_; }
private:
    unsigned magic_;
    unsigned boot_us_;
};

BootStamp g_stamp;

// --- virtual dispatch --------------------------------------------------
class Shape {
public:
    virtual int area() const = 0;
    virtual const char *name() const = 0;
    virtual ~Shape() = default;     // heap test deletes through base
};

class Rect : public Shape {
public:
    Rect(int w, int h) : w_(w), h_(h) { }
    int area() const override { return w_ * h_; }
    const char *name() const override { return "rect"; }
private:
    int w_, h_;
};

class Tri : public Shape {
public:
    Tri(int b, int h) : b_(b), h_(h) { }
    int area() const override { return b_ * h_ / 2; }
    const char *name() const override { return "tri"; }
private:
    int b_, h_;
};

// --- templates ---------------------------------------------------------
template <typename T, int N>
class RingSum {
public:
    void push(T v)
    {
        sum_ += v - buf_[idx_];
        buf_[idx_] = v;
        idx_ = (idx_ + 1) % N;
    }
    T sum() const { return sum_; }
private:
    T buf_[N] = {};
    T sum_ = 0;
    int idx_ = 0;
};

int checks_run, checks_failed;

void expect(bool ok, const char *what)
{
    ++checks_run;
    if (!ok)
        ++checks_failed;
    printf("  %s: %s\n", what, ok ? "PASS" : "FAIL");
}

} // namespace

extern "C" void cpp_test_run(void)
{
    printf("\nC++ smoke test (g++ %d.%d, C++%ld)\n",
           __GNUC__, __GNUC_MINOR__, __cplusplus / 100 % 100);

    // global constructor ran before main, at boot time
    expect(g_stamp.magic() == 0xC0FFEEu, "global constructor ran");
    expect(g_stamp.boot_us() < 1000000u, "ctor ran at boot, not lazily");

    // virtual dispatch through a base pointer
    Rect r(6, 7);
    Tri t(10, 5);
    const Shape *shapes[] = { &r, &t };
    int total = 0;
    for (const Shape *s : shapes)
        total += s->area();
    expect(total == 42 + 25, "virtual dispatch (areas sum)");
    expect(shapes[1]->name()[0] == 't', "virtual dispatch (names)");

    // templates with non-type parameters
    RingSum<int, 4> ring;
    for (int i = 1; i <= 6; ++i)
        ring.push(i);          // window holds 3,4,5,6
    expect(ring.sum() == 18, "class template instantiation");

    // local static initialization (guards elided, single core)
    struct Counter {
        static int next()
        {
            static int n = 100;
            return ++n;
        }
    };
    Counter::next();
    expect(Counter::next() == 102, "local static state");

    // constexpr / compile-time evaluation
    constexpr int fib7 = [] {
        int a = 0, b = 1;
        for (int i = 0; i < 7; ++i) { int c = a + b; a = b; b = c; }
        return a;
    }();
    static_assert(fib7 == 13, "constexpr lambda");
    expect(fib7 == 13, "constexpr evaluation");

    // heap: operator new/delete over newlib malloc (cxxrt.cpp), heap
    // span placed in ram_a by the SDK memmap
    uintptr_t hs = (uintptr_t)&__HeapStart;
    expect(hs >= 0x1000000u && hs < 0x1800000u, "heap lives in ram_a");

    Rect *pr = new Rect(6, 7);
    expect(pr != nullptr && pr->area() == 42, "operator new + virtual");
    delete pr;

    int *arr = new int[1000];
    expect(arr != nullptr, "operator new[]");
    if (arr) {
        for (int i = 0; i < 1000; ++i)
            arr[i] = i;
        int sum = 0;
        for (int i = 0; i < 1000; ++i)
            sum += arr[i];
        expect(sum == 499500, "heap array read back");
        delete[] arr;
    }

    void *big = malloc(1u << 20);          // 1MB: fits in ram_a span
    expect(big != nullptr, "malloc(1MB)");
    free(big);
    void *absurd = malloc(16u << 20);      // 16MB: larger than ram_a
    expect(absurd == nullptr, "malloc(16MB) fails cleanly");
    free(absurd);

    printf("%d/%d checks passed - %s\n",
           checks_run - checks_failed, checks_run,
           checks_failed ? "*** FAIL ***" : "*** PASS ***");
    checks_run = checks_failed = 0;
}
