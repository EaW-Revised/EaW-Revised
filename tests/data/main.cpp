#include "data_test_support.hpp"

#include "eawr/data/xml.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::tests::data_contracts {

int failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

} // namespace eawr::tests::data_contracts

using namespace eawr::tests::data_contracts;

int main() {
    run_contracts();
    tag_trace_contracts();
    if (failures == 0) std::cout << "data contracts passed\n";
    else std::cerr << failures << " data contract test(s) failed\n";
    return failures == 0 ? 0 : 1;
}
