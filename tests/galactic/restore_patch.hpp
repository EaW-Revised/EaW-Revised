#pragma once

// Test helper: write a mutated copy of one service table into an otherwise valid direct checkpoint and report what
// the restore says. The table is located by its exact encoding, so a miss fails loudly instead of passing vacuously.
#include "../support/result.hpp"
#include "galactic_support.hpp"
#include "../../src/sim/galactic/checkpoint_fields_internal.hpp"
#include "eawr/sim/galactic/session.hpp"

#include <algorithm>
#include <string>
#include <utility>

namespace galactic_test {

// Returns "" when the patched checkpoint loads, else the restore's diagnostic message.
template <class Table, class Mutate>
std::string restore_message_after(const eawr::sim::galactic::GalacticSetup& config, const Table& table, Mutate&& mutate) {
    namespace gc = eawr::sim::galactic;
    auto source = eawr::testing::take(gc::GalacticSession::create(config));
    auto bytes = eawr::testing::take(source.save_checkpoint_state());
    gc::checkpoint_detail::Writer original; original(table);
    auto changed = table;
    if (!table) return "table absent";
    mutate(changed);
    gc::checkpoint_detail::Writer replacement; replacement(changed);
    const auto at = std::search(bytes.begin(), bytes.end(), original.bytes.begin(), original.bytes.end());
    expect(at != bytes.end(), "service table found in the checkpoint owner");
    if (at == bytes.end()) return "table not found";
    const auto offset = static_cast<std::size_t>(at - bytes.begin());
    bytes.erase(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
        bytes.begin() + static_cast<std::ptrdiff_t>(offset + original.bytes.size()));
    bytes.insert(bytes.begin() + static_cast<std::ptrdiff_t>(offset), replacement.bytes.begin(), replacement.bytes.end());
    auto restored = eawr::testing::take(gc::GalacticSession::create(config));
    const auto result = restored.load_checkpoint_state(bytes);
    return result ? std::string{} : result.error().message;
}

// The restore refuses with the expected cause (a distinctive phrase of the message), in the invalid_setup family.
inline void expect_refusal(const std::string& message, const std::string& phrase, const char* label) {
    expect(message.find(phrase) != std::string::npos, label);
}

} // namespace galactic_test
