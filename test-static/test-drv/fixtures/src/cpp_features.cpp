#include "cpp_features.h"
#include <cstdint>
#include <cstddef>
#include <cstdlib>

extern "C" int printf(const char*, ...);
extern "C" void uart_puts(const char*);

void* operator new(size_t n) { return malloc(n); }
void* operator new[](size_t n) { return malloc(n); }
void operator delete(void* p) noexcept { free(p); }
void operator delete[](void* p) noexcept { free(p); }
void operator delete(void* p, size_t) noexcept { free(p); }
void operator delete[](void* p, size_t) noexcept { free(p); }

extern "C" void __cxa_pure_virtual() { while (1); }

static constexpr uint32_t MASK_A = 0xAA;
static constexpr uint32_t MASK_B = 0x55;

class Widget {
public:
    Widget(uint32_t id) : id_(id) { ++alive_; }
    ~Widget() { --alive_; }
    uint32_t id() const { return id_; }
    static uint32_t alive() { return alive_; }
private:
    uint32_t id_;
    static uint32_t alive_;
};
uint32_t Widget::alive_ = 0;

class IProcessor {
public:
    virtual ~IProcessor() = default;
    virtual uint32_t process(uint32_t x) const = 0;
};

class ScaleProc : public IProcessor {
public:
    explicit ScaleProc(uint32_t factor) : factor_(factor) {}
    uint32_t process(uint32_t x) const override { return x * factor_; }
private:
    uint32_t factor_;
};

class XorProc : public IProcessor {
public:
    uint32_t process(uint32_t x) const override { return x ^ MASK_B; }
};

template<uint32_t Mul>
class ShiftProc : public IProcessor {
public:
    uint32_t process(uint32_t x) const override { return (x << 1) + Mul; }
};

__attribute__((constructor)) void cpp_ctor() {
    printf("C++ fixture constructor ran\n");
    uart_puts("C++ fixture constructor ran\n");
}

static uint32_t run_polymorphism(uint32_t mode, uint32_t x) {
    IProcessor* p = nullptr;
    switch (mode) {
    case 0: p = new ScaleProc(3);    break;
    case 1: p = new XorProc();       break;
    case 2: p = new ShiftProc<7>();  break;
    default: return 0;
    }
    uint32_t r = p->process(x);
    delete p;
    return r;
}

extern "C" uint32_t cpp_features_run(uint32_t mode, uint32_t x) {
    Widget w(42);
    uint32_t id = w.id();
    uint32_t alive = Widget::alive();

    auto* buf = new uint32_t(x);
    asm volatile("" :: "r"(buf) : "memory");
    uint32_t val = buf ? *buf : 0;
    delete buf;

    uint32_t pr = run_polymorphism(mode, x);

    auto* arr = new uint32_t[4];
    asm volatile("" :: "r"(arr) : "memory");
    for (int i = 0; i < 4; ++i) arr[i] = (x + i) * (mode + 1);
    uint32_t sum = 0;
    for (int i = 0; i < 4; ++i) sum += arr[i];
    delete[] arr;

    return id + alive + val + pr + sum;
}

extern "C" uint32_t cpp_features_summary(void) {
    return (MASK_A << 8) | MASK_B;
}

extern "C" void cpp_features_print(void) {
    uint32_t r0 = cpp_features_run(0, 10);
    uint32_t r1 = cpp_features_run(1, 10);
    uint32_t r2 = cpp_features_run(2, 10);
    uint32_t s  = cpp_features_summary();
    printf("C++ features: Widget(id=%u) ScaleProc=%u XorProc=%u ShiftProc=%u summary=0x%04X\n",
           r0, r1, r2, s);
    char buf[128];
    extern int snprintf(char*, size_t, const char*, ...);
    snprintf(buf, sizeof(buf),
        "C++ features: run0=%lu run1=%lu run2=%lu sum=0x%04X (via UART)\n",
        (unsigned long)r0, (unsigned long)r1, (unsigned long)r2, (unsigned long)s);
    uart_puts(buf);
}
