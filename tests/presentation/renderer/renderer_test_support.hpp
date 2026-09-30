#pragma once

// Private support for the renderer contract runner (main.cpp), which keeps the
// helper definitions; the cases are grouped into renderer_{resource,submission,
// surface}_tests.cpp.

#include "eawr/presentation/renderer.hpp"
#include "diagnostic_buffer.hpp"
#include "instance_reconciliation.hpp"
#include "missing_asset_waits.hpp"
#include "pass_submission.hpp"
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

namespace eawr_renderer_test {

extern int failures;

void check(const bool condition, const std::string_view message);
eawr::sim::math::Fixed fixed(const std::int64_t integer);
eawr::core::Diagnostic diagnostic(std::string message);
std::vector<std::byte> byte_vector(const std::initializer_list<unsigned> values);

// renderer_resource_tests.cpp
void particle_texture_contracts();
void upload_identity_contracts();
void instance_resource_contracts();

// renderer_submission_tests.cpp
void missing_asset_wait_contracts();
void pass_order_contracts();
void snapshot_adapter_contracts();

// renderer_surface_tests.cpp
void derived_fog_variant_contracts();
void surface_material_contracts();

} // namespace eawr_renderer_test
