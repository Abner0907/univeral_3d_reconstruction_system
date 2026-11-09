#pragma once

// Minimal test harness (the engine has no test-framework dependency).
//   TEST(name) { CHECK(cond); CHECK_NEAR(a, b, tol); }
// Run: u3d_tests [substring-filter]

#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace u3dtest {

struct TestCase {
    const char *name;
    void (*fn)();
};

inline std::vector<TestCase> &registry()
{
    static std::vector<TestCase> tests;
    return tests;
}

struct Registrar {
    Registrar(const char *name, void (*fn)()) { registry().push_back({name, fn}); }
};

struct Failure : std::runtime_error {
    using std::runtime_error::runtime_error;
};

inline std::string where(const char *file, int line)
{
    return std::string(file) + ":" + std::to_string(line) + ": ";
}

} // namespace u3dtest

#define TEST(name)                                                        \
    static void name();                                                   \
    static ::u3dtest::Registrar registrar_##name(#name, &name);           \
    static void name()

#define CHECK(cond)                                                                              \
    do {                                                                                         \
        if (!(cond))                                                                             \
            throw ::u3dtest::Failure(::u3dtest::where(__FILE__, __LINE__) + "CHECK(" #cond ")"); \
    } while (0)

#define CHECK_NEAR(a, b, tol)                                                                    \
    do {                                                                                         \
        const double va_ = double(a), vb_ = double(b);                                           \
        if (!(std::fabs(va_ - vb_) <= double(tol)))                                              \
            throw ::u3dtest::Failure(::u3dtest::where(__FILE__, __LINE__) + #a " = "             \
                                     + std::to_string(va_) + ", " #b " = " + std::to_string(vb_) \
                                     + ", tolerance " + std::to_string(double(tol)));            \
    } while (0)

#define LOG(...)                     \
    do {                             \
        std::printf("    ");         \
        std::printf(__VA_ARGS__);    \
        std::printf("\n");           \
        std::fflush(stdout);         \
    } while (0)
