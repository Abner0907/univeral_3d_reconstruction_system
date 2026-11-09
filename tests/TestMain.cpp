#include "Test.h"

#include <chrono>
#include <cstring>

int main(int argc, char **argv)
{
    const char *filter = argc > 1 ? argv[1] : "";
    int failed = 0, run = 0;
    for (const auto &test : u3dtest::registry()) {
        if (std::strstr(test.name, filter) == nullptr)
            continue;
        ++run;
        std::printf("[ RUN  ] %s\n", test.name);
        std::fflush(stdout);
        const auto start = std::chrono::steady_clock::now();
        try {
            test.fn();
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            std::printf("[  OK  ] %s (%.0f ms)\n", test.name, ms);
        } catch (const std::exception &e) {
            ++failed;
            std::printf("[ FAIL ] %s\n         %s\n", test.name, e.what());
        }
        std::fflush(stdout);
    }
    std::printf("%d test(s), %d failed\n", run, failed);
    return (failed == 0 && run > 0) ? 0 : 1;
}
