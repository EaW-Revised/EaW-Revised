#include "support_variants_self_test.hpp"
#include "../support/check.hpp"

#include <sstream>
#include <stdexcept>
#include <string>

bool galactic_support_self_test() {
    eawr::testing::Checker checker;
    std::ostringstream diagnostic;
    checker.check(false, "counting self-test", diagnostic);
    if (checker.finish() == 0 || checker.failures != 1
        || diagnostic.str() != "FAIL: counting self-test\n") return false;
    try {
        eawr::testing::require(false, "throwing self-test");
    } catch (const std::runtime_error& error) {
        return std::string(error.what()) == "throwing self-test"
            && galactic_support_test_detail::variants();
    }
    return false;
}
