#include "TestSupport.hpp"

int main()
{
    int failures = 0;
    for (const auto &test : localscribe::test::registry()) {
        try {
            test.function();
            std::cout << "[PASS] " << test.name << '\n';
        } catch (const std::exception &error) {
            ++failures;
            std::cerr << "[FAIL] " << test.name << ": " << error.what()
                      << '\n';
        } catch (...) {
            ++failures;
            std::cerr << "[FAIL] " << test.name << ": unknown exception\n";
        }
    }
    std::cout << localscribe::test::registry().size() - failures
              << " passed, " << failures << " failed\n";
    return failures == 0 ? 0 : 1;
}
