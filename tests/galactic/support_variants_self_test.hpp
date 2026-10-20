#pragma once

#include "../support/check.hpp"

#include <ios>
#include <ostream>
#include <sstream>

namespace galactic_support_test_detail {

struct FailureOrderBuffer final : std::stringbuf {
    const eawr::testing::Checker& checker;
    int first_observed = -1;

    explicit FailureOrderBuffer(const eawr::testing::Checker& value) : checker(value) {}
    std::streamsize xsputn(const char* text, std::streamsize size) override {
        if (first_observed == -1) first_observed = checker.failures;
        return std::stringbuf::xsputn(text, size);
    }
    int_type overflow(int_type value) override {
        if (first_observed == -1) first_observed = checker.failures;
        return std::stringbuf::overflow(value);
    }
};

inline bool variants() {
    eawr::testing::Checker after;
    FailureOrderBuffer after_buffer(after);
    std::ostream after_output(&after_buffer);
    after.check(false, "late", after_output, "FAILED: ");
    if (after_buffer.str() != "FAILED: late\n" || after_buffer.first_observed != 0
        || after.failures != 1 || after.finish() != 1) return false;

    eawr::testing::Checker before;
    FailureOrderBuffer before_buffer(before);
    std::ostream before_output(&before_buffer);
    before.check_before_print(false, "early", before_output, "FAILED: ");
    if (before_buffer.str() != "FAILED: early\n" || before_buffer.first_observed != 1
        || before.failures != 1 || before.finish() != 1) return false;

    std::ostringstream raw;
    eawr::testing::Checker plain;
    plain.check(false, "plain", raw, "");
    return raw.str() == "plain\n" && plain.finish() == 1;
}

} // namespace galactic_support_test_detail
