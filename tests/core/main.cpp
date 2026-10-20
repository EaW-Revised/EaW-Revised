#include <string_view>
#include "../support/check.hpp"
#include "eawr/core/diagnostic.hpp"
#include "eawr/core/json_text.hpp"
#include "eawr/core/result.hpp"

#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

eawr::testing::Checker local_checker;
int& failures = local_checker.failures;

void expect(const bool condition, const char* const message) {
    local_checker.check(condition, message, std::cerr);
}

bool core_support_self_test() {
    eawr::testing::Checker checker;
    std::ostringstream diagnostic;
    checker.check(false, "counting self-test", diagnostic);
    if (checker.finish() != 1 || checker.failures != 1
        || diagnostic.str() != "FAIL: counting self-test\n") return false;
    try {
        eawr::testing::require(false, "throwing self-test");
    } catch (const std::runtime_error& error) {
        return std::string(error.what()) == "throwing self-test";
    }
    return false;
}

} // namespace

int main() {
    if (!core_support_self_test()) {
        std::cerr << "FAIL: shared check/require self-test\n";
        return 1;
    }
    using eawr::core::Diagnostic;
    using eawr::core::Result;
    using eawr::core::Severity;

    using eawr::core::json_quote;
    expect(json_quote("") == "\"\"", "JSON quotes an empty string");
    expect(json_quote("quote\" and slash\\") == "\"quote\\\" and slash\\\\\"",
           "JSON escapes quotes and backslashes");
    expect(json_quote(std::string_view("a\0b", 3)) == "\"a\\u0000b\"",
           "JSON preserves embedded nulls");
    std::string controls;
    for (unsigned byte = 0; byte < 32; ++byte) controls += static_cast<char>(byte);
    const std::string expected_controls =
        "\\u0000\\u0001\\u0002\\u0003\\u0004\\u0005\\u0006\\u0007"
        "\\u0008\\t\\n\\u000b\\u000c\\r\\u000e\\u000f"
        "\\u0010\\u0011\\u0012\\u0013\\u0014\\u0015\\u0016\\u0017"
        "\\u0018\\u0019\\u001a\\u001b\\u001c\\u001d\\u001e\\u001f";
    expect(json_quote(controls) == '"' + expected_controls + '"', "JSON escapes every control byte");
    std::string high_bytes;
    for (unsigned byte = 127; byte < 256; ++byte) high_bytes += static_cast<char>(byte);
    expect(json_quote(high_bytes) == '"' + high_bytes + '"', "JSON preserves every non-ASCII byte");
    expect(json_quote("\xc3\xa9") == "\"\xc3\xa9\"", "JSON preserves UTF-8 bytes");
    expect(eawr::core::json_quote_short_controls("\b\f\n\r\t\x01") == "\"\\b\\f\\n\\r\\t\\u0001\"",
           "Short-control JSON keeps backspace and form-feed escapes");
    expect(eawr::core::json_quote_ascii("\n\t\x7f\xc3\xa9") == "\"\\u000a\\u0009\\u007f\\u00c3\\u00a9\"",
           "ASCII JSON escapes controls and high bytes individually");
    expect(eawr::core::json_quote_ascii_short_controls("\n\t\b\x7f\xc3\xa9") == "\"\\n\\t\\u0008\\u007f\\u00c3\\u00a9\"",
           "ASCII short-control JSON keeps the original report bytes");
    expect(eawr::core::json_quote_space_controls(controls) == '"' + std::string(32, ' ') + '"',
           "Space-control JSON replaces every control byte");
    expect(eawr::core::json_quote_drop_controls(controls) == "\"\"", "Drop-control JSON omits every control byte");
    expect(eawr::core::json_quote_drop_controls("a\n\"\\\xc3\xa9") == "\"a\\\"\\\\\xc3\xa9\"",
           "Drop-control JSON still quotes punctuation and preserves high bytes");

    expect(eawr::core::json_quote_verbatim_controls(controls) == '"' + controls + '"',
           "Verbatim-control quoting preserves the existing UI report bytes");

    const Diagnostic diagnostic{
        .code = "EAWR-TEST-0001",
        .severity = Severity::warning,
        .message = "synthetic diagnostic",
        .logical_path = "fixture/input.xml",
        .line = 7,
        .column = 11,
        .source_id = "fixture",
    };
    expect(
        eawr::core::format_diagnostic(diagnostic) ==
            "EAWR-TEST-0001 [warning] fixture/input.xml:7:11 (source fixture): synthetic diagnostic",
        "Diagnostic fields must retain stable formatting and 1-based locations"
    );

    auto success = Result<std::string>::success("value");
    expect(success.has_value(), "successful Result must have a value");
    expect(success.value() == "value", "successful Result must preserve its value");

    auto failure = Result<std::string>::failure(diagnostic);
    expect(!failure.has_value(), "failed Result must not have a value");
    expect(failure.error().code == "EAWR-TEST-0001", "failed Result must preserve its diagnostic");

    if (failures == 0) {
        std::cout << "core contracts passed\n";
    }
    return failures == 0 ? 0 : 1;
}
