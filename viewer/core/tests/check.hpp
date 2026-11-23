// check.hpp: a tiny test framework, no dependencies.
//
//   TEST(name) { CHECK(cond); CHECK_EQ(a, b); CHECK_STREQ(a, b); }
//
// Tests register themselves at static-initialisation time; test_main.cpp
// runs them all and returns non-zero if any check failed.
#ifndef RV_CHECK_HPP
#define RV_CHECK_HPP

#include <cstring>
#include <iostream>

namespace rvtest {

using TestFn = void (*)();

struct Registrar {
    Registrar(const char *name, TestFn fn);
};

// Record one failed check (prints file, line and the expression).
void fail(const char *file, int line, const char *what);

// Count one passed check.
void pass();

} // namespace rvtest

#define RVTEST_CAT2(a, b) a##b
#define RVTEST_CAT(a, b) RVTEST_CAT2(a, b)

#define TEST(name)                                                         \
    static void RVTEST_CAT(rvtest_fn_, name)();                            \
    static const ::rvtest::Registrar RVTEST_CAT(rvtest_reg_, name)(        \
        #name, &RVTEST_CAT(rvtest_fn_, name));                             \
    static void RVTEST_CAT(rvtest_fn_, name)()

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (cond) ::rvtest::pass();                                        \
        else ::rvtest::fail(__FILE__, __LINE__, "CHECK(" #cond ")");       \
    } while (0)

#define CHECK_EQ(a, b)                                                     \
    do {                                                                   \
        const auto &rvtest_a = (a);                                        \
        const auto &rvtest_b = (b);                                        \
        if (rvtest_a == rvtest_b) {                                        \
            ::rvtest::pass();                                              \
        } else {                                                           \
            std::cerr << "    left:  " << rvtest_a << "\n"                 \
                      << "    right: " << rvtest_b << "\n";                \
            ::rvtest::fail(__FILE__, __LINE__, "CHECK_EQ(" #a ", " #b ")");\
        }                                                                  \
    } while (0)

#define CHECK_STREQ(a, b)                                                  \
    do {                                                                   \
        const char *rvtest_a = (a);                                        \
        const char *rvtest_b = (b);                                        \
        if (rvtest_a != nullptr && rvtest_b != nullptr &&                  \
            std::strcmp(rvtest_a, rvtest_b) == 0) {                        \
            ::rvtest::pass();                                              \
        } else {                                                           \
            std::cerr << "    left:  "                                     \
                      << (rvtest_a ? rvtest_a : "(null)") << "\n"          \
                      << "    right: "                                     \
                      << (rvtest_b ? rvtest_b : "(null)") << "\n";         \
            ::rvtest::fail(__FILE__, __LINE__,                             \
                           "CHECK_STREQ(" #a ", " #b ")");                 \
        }                                                                  \
    } while (0)

#endif // RV_CHECK_HPP
