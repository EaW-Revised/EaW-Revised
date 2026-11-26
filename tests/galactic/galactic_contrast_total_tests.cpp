#include "galactic_ai_selection.hpp"

#include <iostream>
#include <string>

// WGA-508: the total-force validity test of an attempted domain.
namespace gg = eawr::script::foc::ai::galactic_goals;
using eawr::script::foc::ai::real;
using eawr::script::foc::ai::Real;

namespace {
int failures = 0;
void check(bool ok, const std::string& what) {
    if (!ok) { std::cerr << "FAILED: " << what << '\n'; ++failures; }
}
} // namespace

int main() {
    const Real third = real(1) / real(3);
    check(gg::total_force_unmet(real(100), real(100), third), "nothing matched: unmet");
    check(gg::total_force_unmet(real(100), real(40), third), "40 percent left is above a one third limit");
    check(!gg::total_force_unmet(real(100), real(30), third), "30 percent left is within a one third limit");
    check(!gg::total_force_unmet(real(100), real(-14607), third), "an over-met target (negative remainder) is met");
    check(!gg::total_force_unmet(Real{}, real(50), third), "a target with no force is never unmet");
    check(!gg::total_force_unmet(real(100), Real{}, Real{}), "exactly met with a zero limit is met");
    check(gg::total_force_unmet(real(100), real(1), Real{}), "anything left with a zero limit is unmet");
    if (failures) return 1;
    std::cout << "galactic total-force contrast contracts passed\n";
    return 0;
}
