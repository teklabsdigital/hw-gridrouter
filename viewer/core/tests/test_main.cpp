// test_main.cpp: runs every TEST registered through check.hpp.
//
//   rv_tests            run all tests
//   rv_tests NAME...    run only the named tests
//
// Prints one line per test and a summary; exits 1 if any check failed.

#include "check.hpp"

#include <cstring>
#include <iostream>
#include <vector>

namespace rvtest {
namespace {

struct Entry {
    const char *name;
    TestFn fn;
};

std::vector<Entry> &registry() {
    static std::vector<Entry> tests;
    return tests;
}

int g_passed = 0;
int g_failed = 0;

} // namespace

Registrar::Registrar(const char *name, TestFn fn) {
    registry().push_back(Entry{name, fn});
}

void fail(const char *file, int line, const char *what) {
    ++g_failed;
    std::cerr << "    FAIL " << file << ":" << line << ": " << what << "\n";
}

void pass() {
    ++g_passed;
}

} // namespace rvtest

int main(int argc, char **argv) {
    using namespace rvtest;
    int ran = 0;
    int failed_tests = 0;
    for (const Entry &t : registry()) {
        if (argc > 1) {
            bool wanted = false;
            for (int i = 1; i < argc; ++i)
                if (std::strcmp(argv[i], t.name) == 0) wanted = true;
            if (!wanted) continue;
        }
        const int before = g_failed;
        t.fn();
        ++ran;
        const bool ok = g_failed == before;
        if (!ok) ++failed_tests;
        std::cout << (ok ? "  ok   " : "  FAIL ") << t.name << "\n";
    }
    std::cout << ran << " tests (" << failed_tests << " failed), " << g_passed
              << " checks passed, " << g_failed << " checks failed\n";
    if (ran == 0) {
        std::cerr << "no tests ran\n";
        return 1;
    }
    return g_failed == 0 ? 0 : 1;
}
