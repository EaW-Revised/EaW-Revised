#pragma once

#include "check.hpp"

#include <sstream>
#include <stdexcept>
#include <string>

namespace eawr::testing {

inline bool support_contract_self_test() {
    Checker counter;
    std::ostringstream diagnostic;
    counter.check(false, "counting self-test", diagnostic);
    if (counter.failures != 1 || counter.finish() == 0
        || diagnostic.str() != "FAIL: counting self-test\n") return false;
    Checker before;
    std::ostringstream before_diagnostic;
    before.check_before_print(false, "before-print self-test", before_diagnostic);
    if (before.failures != 1 || before.finish() != 1
        || before_diagnostic.str() != "FAIL: before-print self-test\n") return false;
    try {
        eawr::testing::require(false, "throwing self-test");
    } catch (const std::runtime_error& error) {
        return std::string(error.what()) == "throwing self-test";
    }
    return false;
}

} // namespace eawr::testing
