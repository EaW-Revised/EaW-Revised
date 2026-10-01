#include "eawr/presentation/renderer.hpp"
#include "diagnostic_buffer.hpp"
#include "instance_reconciliation.hpp"
#include "missing_asset_waits.hpp"
#include "pass_submission.hpp"
#include "submission_plan.hpp"
#include "particle_texture.hpp"
#include "resource_lease_ledger.hpp"
#include "shader_adapter.hpp"
#include "upload_identity.hpp"
#include "upload_winding.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <initializer_list>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "renderer_test_support.hpp"

namespace eawr_renderer_test {

int failures{};

void check(const bool condition, const std::string_view message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

eawr::sim::math::Fixed fixed(const std::int64_t integer) {
    return eawr::sim::math::Fixed::from_raw(integer * eawr::sim::math::Fixed::scale);
}

eawr::core::Diagnostic diagnostic(std::string message) {
    return {
        .code = "EAWR-PRES-0002",
        .severity = eawr::core::Severity::error,
        .message = std::move(message),
        .logical_path = {},
        .line = {},
        .column = {},
        .source_id = std::string("presentation.godot"),
    };
}

std::vector<std::byte> byte_vector(const std::initializer_list<unsigned> values) {
    std::vector<std::byte> result;
    for (const unsigned value : values) result.push_back(static_cast<std::byte>(value));
    return result;
}

} // namespace eawr_renderer_test

int main() {
    using namespace eawr_renderer_test;
    particle_texture_contracts();
    derived_fog_variant_contracts();
    upload_identity_contracts();
    missing_asset_wait_contracts();
    pass_order_contracts();
    surface_material_contracts();
    snapshot_adapter_contracts();
    submission_plan_contracts();
    submission_presence_contracts();
    instance_resource_contracts();

    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
