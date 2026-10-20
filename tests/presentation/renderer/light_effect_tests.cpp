// Light effect contracts (WSD-02, WSD-08): the self-destruct pulse and its cancel flash, from frame counts only.

#include "../../support/check.hpp"
#include "eawr/presentation/effects/light_effect.hpp"

#include <array>
#include <cmath>
#include <iostream>
#include <optional>
#include <string_view>

namespace {

namespace le = eawr::presentation::light_effect;

eawr::testing::Checker local_checker;
int& failures = local_checker.failures;
void expect(const bool condition, const std::string_view message) {
    if (!condition) local_checker.check(condition, message, std::cerr, "FAILED: ");
}
bool near(const float a, const float b) { return std::fabs(a - b) < 1e-4F; }

constexpr std::array<float, 3> base{1.0F, 1.0F, 1.0F};

void test_pulse() {
    // 105-frame countdown: one linear pulse over 210 frames, peak (3, 0, 0) at frame 105.
    const auto state = le::self_destruct_pulse(1000.0, 0U, 105U);
    expect(state.kind == le::Kind::linear_pulse && state.duration_frames == 210.0 && state.pulses == 1U, "pulse shape");
    auto at = [&](const double frame) { return le::apply(state, frame, base); };
    expect(at(999.0) == std::nullopt, "not before the start");
    expect(at(1000.0) && near((*at(1000.0))[0], 1.0F) && near((*at(1000.0))[1], 1.0F), "starts at the base light");
    expect(at(1052.5) && near((*at(1052.5))[0], 2.0F) && near((*at(1052.5))[1], 0.5F) && near((*at(1052.5))[2], 0.5F),
           "a quarter in: half way to red");
    expect(at(1105.0) && near((*at(1105.0))[0], 3.0F) && near((*at(1105.0))[1], 0.0F), "peak at the expiry frame");
    expect(at(1157.5) && near((*at(1157.5))[0], 2.0F), "falls back symmetrically");
    expect(at(1210.0) == std::nullopt, "ends after twice the expiration");
}

void test_late_sight() {
    // First seen 30 frames into the countdown: the pulse is placed as if it started at the activation.
    const auto state = le::self_destruct_pulse(1030.0, 30U, 105U);
    expect(state.start_frame == 1000.0, "start frame back-dated by the elapsed frames");
}

void test_cancel_flash() {
    const auto running = le::self_destruct_pulse(1000.0, 0U, 105U);
    const auto flash = le::self_destruct_cancel(running, 1040.0, 65U);
    expect(flash.kind == le::Kind::color_flash && near(flash.color[0], 3.0F) && flash.duration_frames == 65.0, "flash shape");
    const auto start = le::apply(flash, 1040.0, base);
    expect(start && near((*start)[0], 3.0F) && near((*start)[1], 0.0F), "the flash starts at its full colour");
    const auto middle = le::apply(flash, 1072.5, base);
    expect(middle && near((*middle)[0], 2.0F) && near((*middle)[1], 0.5F), "returns linearly to the base light");
    expect(le::apply(flash, 1105.0, base) == std::nullopt, "ends after the remaining share");
    expect(le::apply(le::self_destruct_cancel(running, 1040.0, 0U), 1040.0, base) == std::nullopt, "a zero share never runs");
}

} // namespace

int main() {
    test_pulse();
    test_late_sight();
    test_cancel_flash();
    return failures == 0 ? 0 : 1;
}
