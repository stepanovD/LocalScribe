#pragma once

#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace localscribe::test {

using TestFunction = void (*)();

struct TestCase {
    std::string name;
    TestFunction function{};
};

inline std::vector<TestCase> &registry()
{
    static std::vector<TestCase> tests;
    return tests;
}

class Registrar {
public:
    Registrar(std::string name, TestFunction function)
    {
        registry().push_back(TestCase{std::move(name), function});
    }
};

[[noreturn]] inline void fail(
    std::string_view expression,
    std::string_view file,
    int line,
    std::string detail = {})
{
    std::ostringstream message;
    message << file << ':' << line << ": check failed: " << expression;
    if (!detail.empty()) {
        message << " (" << detail << ')';
    }
    throw std::runtime_error(message.str());
}

template <typename Left, typename Right>
void checkEqual(
    const Left &left,
    const Right &right,
    std::string_view expression,
    std::string_view file,
    int line)
{
    if (!(left == right)) {
        fail(expression, file, line);
    }
}

} // namespace localscribe::test

#define LS_TEST(name)                                                       \
    static void name();                                                     \
    static ::localscribe::test::Registrar name##_registrar(#name, &name);  \
    static void name()

#define LS_CHECK(expression)                                                \
    do {                                                                    \
        if (!(expression)) {                                                \
            ::localscribe::test::fail(                                      \
                #expression,                                                \
                __FILE__,                                                   \
                __LINE__);                                                  \
        }                                                                   \
    } while (false)

#define LS_CHECK_EQ(left, right)                                            \
    ::localscribe::test::checkEqual(                                        \
        (left),                                                             \
        (right),                                                            \
        #left " == " #right,                                                \
        __FILE__,                                                           \
        __LINE__)
