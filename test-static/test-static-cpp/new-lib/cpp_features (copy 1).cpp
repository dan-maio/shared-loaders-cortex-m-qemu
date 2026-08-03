#include "cpp_features.h"
#include <cstdint>
#include <cstddef>

// --- Local heap allocator (free-list) ---

struct AllocHeader {
    AllocHeader* next;
    uintptr_t size;
};

static char g_heap[2048];
static AllocHeader* g_head;

__attribute__((constructor))
static void heap_init() {
    g_head = reinterpret_cast<AllocHeader*>(g_heap);
    g_head->next = nullptr;
    g_head->size = sizeof(g_heap) - sizeof(AllocHeader);
}

extern "C" void* malloc(unsigned int n) {
    AllocHeader* prev = nullptr;
    AllocHeader* cur = g_head;
    uintptr_t needed = n + sizeof(AllocHeader);
    needed = (needed + 3u) & ~uintptr_t(3);

    while (cur) {
        if (cur->size >= needed) {
            if (cur->size >= needed + sizeof(AllocHeader) + 4) {
                auto* split = reinterpret_cast<AllocHeader*>(
                    reinterpret_cast<char*>(cur) + needed);
                split->next = cur->next;
                split->size = cur->size - needed;
                cur->next = split;
                cur->size = needed;
            }
            if (prev)
                prev->next = cur->next;
            else
                g_head = cur->next;
            return reinterpret_cast<char*>(cur) + sizeof(AllocHeader);
        }
        prev = cur;
        cur = cur->next;
    }
    return nullptr;
}

extern "C" void free(void* p) {
    if (!p) return;
    auto* hdr = reinterpret_cast<AllocHeader*>(
        static_cast<char*>(p) - sizeof(AllocHeader));
    hdr->next = g_head;
    g_head = hdr;
}

// --- C++ operator new/delete backed by local malloc/free ---

void* operator new(unsigned int n) {
    void* p = malloc(n);
    return p;
}

void* operator new[](unsigned int n) {
    void* p = malloc(n);
    return p;
}

void operator delete(void* p) noexcept {
    free(p);
}

void operator delete[](void* p) noexcept {
    free(p);
}

void operator delete(void* p, unsigned int) noexcept {
    free(p);
}

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
