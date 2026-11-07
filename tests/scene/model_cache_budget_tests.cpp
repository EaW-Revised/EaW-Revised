#include "model_cache_budget.hpp"

#include <cstdint>
#include <iostream>
#include <limits>
#include <string_view>

namespace {

int failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

constexpr std::uint64_t mib = 1ULL << 20U;
constexpr std::uint64_t gib = 1ULL << 30U;

void formula_table() {
    using eawr::viewer::model_cache_load_budget;
    expect(model_cache_load_budget(4 * gib) == 256 * mib, "4 GB RAM takes the 256 MiB floor");
    expect(model_cache_load_budget(8 * gib) == 256 * mib, "8 GB RAM gives 256 MiB");
    expect(model_cache_load_budget(16 * gib) == 512 * mib, "16 GB RAM gives 512 MiB");
    expect(model_cache_load_budget(32 * gib) == 1 * gib, "32 GB RAM gives 1 GiB");
    expect(model_cache_load_budget(64 * gib) == 1 * gib, "64 GB RAM stays at the 1 GiB cap");
    expect(model_cache_load_budget(0) == 256 * mib, "an unknown RAM size takes the floor");
    expect(model_cache_load_budget(std::numeric_limits<std::uint64_t>::max()) == 1 * gib,
        "the largest possible RAM size stays at the cap");
    // Clamp edges: 8 GiB / 32 = 256 MiB exactly, 32 GiB / 32 = 1 GiB exactly; one byte either side clamps or scales.
    expect(model_cache_load_budget(8 * gib - 1) == 256 * mib, "just below 8 GiB stays at the floor");
    expect(model_cache_load_budget(8 * gib + 32) == 256 * mib + 1, "just above 8 GiB scales linearly");
    expect(model_cache_load_budget(32 * gib - 32) == 1 * gib - 1, "just below 32 GiB scales linearly");
    expect(model_cache_load_budget(32 * gib + 32) == 1 * gib, "just above 32 GiB stays at the cap");
}

void argument_parsing() {
    using eawr::viewer::parse_model_cache_load_budget;
    expect(parse_model_cache_load_budget("off") == std::numeric_limits<std::uint64_t>::max(),
        "off restores the unbounded load");
    expect(parse_model_cache_load_budget("512") == 512 * mib, "a MiB count converts to bytes");
    expect(parse_model_cache_load_budget("0") == 0, "zero MiB is a valid budget that retains nothing");
    expect(!parse_model_cache_load_budget("").has_value(), "an empty value is rejected");
    expect(!parse_model_cache_load_budget("on").has_value(), "on is not a budget");
    expect(!parse_model_cache_load_budget("12x").has_value(), "trailing text is rejected");
    expect(!parse_model_cache_load_budget("-1").has_value(), "a negative count is rejected");
    expect(!parse_model_cache_load_budget("1234567890").has_value(), "a count too long to convert safely is rejected");
}

} // namespace

int main() {
    formula_table();
    argument_parsing();
    if (failures != 0) {
        std::cerr << failures << " model cache budget contract(s) failed\n";
        return 1;
    }
    std::cout << "model cache budget contracts passed\n";
    return 0;
}
