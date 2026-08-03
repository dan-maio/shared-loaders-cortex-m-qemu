#include "cpp_features.h"
#include <cstdint>
#include <cstddef>
#include <cstdlib>

// --- malloc/free provided by firmware via dynamic linking ---

void* operator new(size_t n) { return malloc(n); }
void* operator new[](size_t n) { return malloc(n); }
void operator delete(void* p) noexcept { free(p); }
void operator delete[](void* p) noexcept { free(p); }
void operator delete(void* p, size_t) noexcept { free(p); }
void operator delete[](void* p, size_t) noexcept { free(p); }

extern "C" void __cxa_pure_virtual() { while (1); }

static constexpr uint32_t MASK_A = 0xAA;
static constexpr uint32_t MASK_B = 0x55;

// 1. Class + 2. Constructor/Destructor + 8. Static member
class Widget {
public:
    Widget(uint32_t id) : id_(id) { ++alive_; }
    ~Widget() { --alive_; }
    uint32_t id() const { return id_; }   // 3. Method
    static uint32_t alive() { return alive_; }
private:
    uint32_t id_;
    static uint32_t alive_;
};
uint32_t Widget::alive_ = 0;

// 7. Abstract class (pure virtual)
class IProcessor {
public:
    virtual ~IProcessor() = default;
    virtual uint32_t process(uint32_t x) const = 0;  // 5. Virtual + pure
};

// 4. Inheritance (derives from IProcessor)
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

// 10. Template
template<uint32_t Mul>
class ShiftProc : public IProcessor {
public:
    uint32_t process(uint32_t x) const override { return (x << 1) + Mul; }
};

static uint32_t run_polymorphism(uint32_t mode, uint32_t x) {
    // 6. Polymorphism via base pointer
    IProcessor* p = nullptr;

    switch (mode) {
    case 0:
        p = new ScaleProc(3);
        break;
    case 1:
        p = new XorProc();
        break;
    case 2: {
        // 10. Template instantiation
        p = new ShiftProc<7>();
        break;
    }
    default:
        return 0;
    }

    uint32_t r = p->process(x);   // virtual dispatch
    delete p;                     // 9. delete
    return r;
}

extern "C" uint32_t cpp_features_run(uint32_t mode, uint32_t x) {
    // 1+2+3: class, ctor/dtor, method
    Widget w(42);
    uint32_t id = w.id();

    // 8: Static member access
    uint32_t alive = Widget::alive();

    // 9: new/delete on basic type
    auto* buf = new uint32_t(x);
    asm volatile("" :: "r"(buf) : "memory");
    uint32_t val = buf ? *buf : 0;
    delete buf;

    // 4+5+6+7+10: polymorphism through abstract base
    uint32_t pr = run_polymorphism(mode, x);

    // 9: new[]/delete[] (array form)
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
