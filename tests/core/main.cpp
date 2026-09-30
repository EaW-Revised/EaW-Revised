#include "eawr/core/diagnostic.hpp"
#include "eawr/core/result.hpp"

#include <iostream>
#include <string>

namespace {

int failures = 0;

void expect(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

} // namespace

int main() {
    using eawr::core::Diagnostic;
    using eawr::core::Result;
    using eawr::core::Severity;

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
