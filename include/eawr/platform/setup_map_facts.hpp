#pragma once

// The skirmish setup map list's persistent facts (skirmish::SetupMapFactsStore) kept in a platform::DerivedStore, the same
// folder, epoch and trust model as the unit-table model facts (docs/model-facts-store.md): a record is served only when the
// map file's stamp and the list's context fingerprint equal the live ones.

#include "eawr/platform/derived_store.hpp"
#include "eawr/skirmish/setup_facts.hpp"

#include <cstdint>
#include <memory>

namespace eawr::platform {

// Record kind byte of the setup list's records in a DerivedStore (1 is the model facts).
inline constexpr std::uint8_t setup_map_facts_kind = 2;

[[nodiscard]] std::shared_ptr<skirmish::SetupMapFactsStore> make_setup_map_facts_store(std::shared_ptr<DerivedStore> store);

// The store over the process-wide model facts folder (configure_model_facts_cache); nullptr while none is configured.
[[nodiscard]] std::shared_ptr<skirmish::SetupMapFactsStore> process_setup_map_facts_store();

} // namespace eawr::platform
