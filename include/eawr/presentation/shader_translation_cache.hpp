#pragma once

#include "eawr/assets/effect.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace eawr::presentation::shader_cache {

// STC-01: caller freshly resolves effective VFS selection/includes on activation.
// Tokens retain order and duplicates; none is a host filesystem path resolver.
struct Token final {
    std::string name, value;
    bool operator==(const Token&) const = default;
};
struct Include final {
    std::string parent, logical, content_sha256;
};
struct Stage final {
    std::string stage, entry, profile;
    std::vector<Token> options;
    // Present for per-stage lookup; whole-source activation may instead key
    // the freshly resolved source/include/macro/compiler input closure.
    std::optional<std::string> input_sha256;
};
enum class Form { compiled, source };
struct Identity final {
    std::string logical, root_sha256;
    Form form{Form::compiled};
    std::vector<Include> includes;
    std::vector<Token> macros, compiler_options;
    std::vector<Stage> stages;
    // Full VERIFIED transitive package/lock digest, not a reported DLL version.
    std::string package_sha256, compiler_abi, capability_manifest, options_manifest_sha256;
    std::uint32_t descriptor_version{assets::effect_descriptor_version};
    std::uint32_t result_schema_version{1}, options_manifest_version{1};
    std::string selection_policy, resolver_policy, translator, emitter;
    std::string binding_policy, state_policy, technique_policy, admission_policy;
    std::string godot_engine, shader_abi, language, renderer, backend, platform, architecture;
    // Producer's finite numeric IDs in its canonical order; no ad hoc textual
    // capabilities. Producer validates unknown/conflicting token groups.
    std::vector<std::uint16_t> capabilities, variants;
};
struct Key final { std::string sha256; };
[[nodiscard]] std::optional<Key> make_key(const Identity& identity);

struct Limits final {
    // Injected resource policy, unrelated to retail content sizes. File budgets
    // include the versioned envelope; scan budget counts every directory entry.
    std::uint64_t total_bytes{}, entry_bytes{};
    std::size_t entries{}, scan_entries{};
};
enum class Status { hit, miss, stored, invalidated, opted_out, invalid_key, invalid_payload, corrupt, io_error, busy, budget };
struct Result final {
    Status status{Status::miss};
    std::vector<std::uint8_t> payload;
};
// STC-02: decode and validate the owned serialized payload, including P1 spans
// reconstructed in activation-owned storage. Required on BOTH lookup and store.
// No descriptor, borrowed span, DLL allocation or renderer object is persisted.
using Validate = std::function<bool(std::span<const std::uint8_t>)>;

class Cache final {
public:
    // STC-03: integration supplies a dedicated directory in player user data,
    // never a mod, game or release directory. Construction does no I/O.
    Cache(std::filesystem::path user_directory, Limits limits);
    [[nodiscard]] Result lookup(const Key& key, bool translation_allowed, const Validate& validate) const;
    [[nodiscard]] Status store(const Key& key, bool translation_allowed,
        std::span<const std::uint8_t> payload, const Validate& validate) const;
    [[nodiscard]] Status invalidate(const Key& key, bool translation_allowed) const;
private:
    std::filesystem::path directory_;
    Limits limits_;
};

} // namespace eawr::presentation::shader_cache
