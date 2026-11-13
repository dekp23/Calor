// Minimal test harness: CHECK records failures, main() reports and exits
// nonzero if any check failed. No external dependency needed.
#pragma once

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace calortest {

inline int& failures() { static int f = 0; return f; }
inline int& checks() { static int c = 0; return c; }

struct Case { const char* name; std::function<void()> fn; };
inline std::vector<Case>& cases() { static std::vector<Case> v; return v; }

struct Register {
    Register(const char* n, std::function<void()> f) { cases().push_back({n, std::move(f)}); }
};

inline bool near(double a, double b) {
    if (std::isinf(a) || std::isinf(b)) return a == b;
    return std::fabs(a - b) <= 1e-6 * (1.0 + std::fabs(a) + std::fabs(b));
}

}  // namespace calortest

#define CALOR_CAT2(a, b) a##b
#define CALOR_CAT(a, b) CALOR_CAT2(a, b)
#define TEST(name)                                                              \
    static void name();                                                         \
    static calortest::Register CALOR_CAT(reg_, name)(#name, name);              \
    static void name()

#define CHECK(cond)                                                             \
    do {                                                                        \
        ++calortest::checks();                                                  \
        if (!(cond)) {                                                          \
            ++calortest::failures();                                            \
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                       \
    } while (0)

#define CHECK_NEAR(a, b) CHECK(calortest::near((a), (b)))

#define CALOR_TEST_MAIN                                                         \
    int main() {                                                                \
        for (auto& c : calortest::cases()) {                                    \
            const int before = calortest::failures();                           \
            c.fn();                                                             \
            std::printf("%s %s\n", calortest::failures() == before ? "ok  " : "FAIL", c.name); \
        }                                                                       \
        std::printf("%d checks, %d failures\n", calortest::checks(), calortest::failures()); \
        return calortest::failures() == 0 ? 0 : 1;                              \
    }
