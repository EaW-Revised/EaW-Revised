#include "depth_prepass_option.hpp"

#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

using eawr::presentation::godot_backend::DepthPrepassOption;
using eawr::presentation::godot_backend::select_depth_prepass;

int failures = 0;

DepthPrepassOption select(const std::vector<std::string>& arguments) {
    return select_depth_prepass(arguments.size(), [&](const std::size_t index) { return arguments[index]; });
}

void expect(const bool ok, const char* what) {
    if (ok) return;
    std::cerr << "FAILED: " << what << '\n';
    ++failures;
}

bool clean(const DepthPrepassOption& option) { return option.error.empty(); }
bool rejected(const DepthPrepassOption& option) { return !option.value.has_value() && !option.error.empty(); }

} // namespace

int main() {
    expect(!select({}).value.has_value() && clean(select({})), "no arguments leave the project setting in charge");
    expect(!select({"--eawr-report", "x.json"}).value.has_value() && clean(select({"--eawr-report", "x.json"})),
           "unrelated options are ignored");
    expect(select({"--eawr-depth-prepass", "off"}).value == std::optional<bool>(false) && clean(select({"--eawr-depth-prepass", "off"})),
           "separate value off");
    expect(select({"--eawr-depth-prepass", "on"}).value == std::optional<bool>(true), "separate value on");
    expect(select({"--eawr-depth-prepass=off"}).value == std::optional<bool>(false), "equals form off");
    expect(select({"--eawr-depth-prepass=on"}).value == std::optional<bool>(true), "equals form on");
    expect(rejected(select({"--eawr-depth-prepass"})), "missing value is reported, nothing applied");
    expect(rejected(select({"--eawr-depth-prepass", "maybe"})), "malformed value is reported, nothing applied");
    expect(rejected(select({"--eawr-depth-prepass="})), "empty equals value is reported");
    expect(rejected(select({"--eawr-depth-prepass", "true"})) && rejected(select({"--eawr-depth-prepass", "1"})),
           "only on and off are accepted");
    expect(select({"--eawr-depth-prepass", "maybe"}).error.find("maybe") != std::string::npos, "the message names the bad value");
    expect(!select({"--eawr-depth-prepass-x", "off"}).value.has_value() && clean(select({"--eawr-depth-prepass-x", "off"})),
           "longer option names do not match");
    expect(select({"--eawr-depth-prepass", "off", "--eawr-depth-prepass", "on"}).value == std::optional<bool>(true), "last good occurrence wins");
    const auto mixed = select({"--eawr-depth-prepass", "off", "--eawr-depth-prepass", "bad"});
    expect(mixed.value == std::optional<bool>(false) && !mixed.error.empty(),
           "a later malformed occurrence keeps the earlier good value and is still reported");
    if (failures == 0) std::cout << "depth_prepass_option: ok\n";
    return failures == 0 ? 0 : 1;
}
