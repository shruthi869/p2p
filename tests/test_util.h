// test_util.h - the tiny test harness shared by all test programs.
#pragma once

#include <cstdio>
#include <dirent.h>
#include <unistd.h>

inline int g_checks = 0;
inline int g_failures = 0;

#define CHECK(cond)                                                                \
    do {                                                                           \
        ++g_checks;                                                                \
        if (!(cond)) {                                                             \
            ++g_failures;                                                          \
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                          \
    } while (0)

// Number of open file descriptors (to prove no socket/file leaks).
inline int count_open_fds() {
    int n = 0;
    DIR* d = ::opendir("/proc/self/fd");
    if (!d) return -1;
    while (dirent* e = ::readdir(d))
        if (e->d_name[0] != '.') ++n;
    ::closedir(d);
    return n - 1;  // minus the fd opendir itself used
}

struct TestCase {
    const char* name;
    void (*fn)();
};

// Runs every test, then the fd-leak check. Returns the process exit code.
// alarm(): a deadlock becomes a failure (SIGALRM kills us) instead of a hang.
template <size_t N>
int run_tests(const TestCase (&tests)[N]) {
    ::alarm(120);
    const int fds_before = count_open_fds();
    for (const auto& t : tests) {
        const int before = g_failures;
        t.fn();
        std::printf("[%s] %s\n", g_failures == before ? "PASS" : "FAIL", t.name);
    }
    const int leaked = count_open_fds() - fds_before;
    CHECK(leaked == 0);
    std::printf("fd leak check: %d fds leaked\n", leaked);
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
