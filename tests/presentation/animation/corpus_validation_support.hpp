#pragma once

#include "corpus_associations.hpp"
#include "corpus_diagnostics.hpp"

#include "eawr/assets/assets.hpp"
#include "eawr/presentation/animation/animation.hpp"
#include "eawr/sim/replay.hpp"
#include "eawr/vfs/vfs.hpp"

#if __has_include("corpus_tool_identity.hpp")
#include "corpus_tool_identity.hpp"
#define EAWR_CORPUS_TOOL_IDENTITY 1
#endif

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace eawr_validation {
namespace corpus = eawr::tests::animation_corpus;

struct Failure final {
    std::string path;
    std::string sha256;
    std::string model_path;
    std::string stage;
    std::string code;
    std::string cause;
    std::string follow_up;
};

struct ModelEntry final {
    std::optional<eawr::vfs::AssetRecord> record;
    std::string sha256;
    std::optional<eawr::assets::Model> model;
    std::optional<eawr::core::Diagnostic> error;
};

struct AnimationFacts final {
    unsigned version{};
    std::uint32_t stored_frames{};
    std::uint32_t playable_frames{};
    float frames_per_second{};
    float duration_seconds{};
    std::size_t tracks{};
};

// One receipt row per effective ALA, in effective enumeration order.
struct Row final {
    eawr::vfs::AssetRecord record;
    std::string sha256;
    std::string stage{"passed"};
    std::string code;
    std::string cause;
    std::optional<std::size_t> baseline_failure_index;
    std::optional<AnimationFacts> animation;
    std::vector<std::string> qualifying_models;
    std::string model_path;
    std::optional<corpus::BindingDiagnosis> binding;
    std::optional<bool> binding_consistent;
    std::size_t samples_passed{};
};

struct AuditInputs final {
    std::filesystem::path ledger_path;
    std::filesystem::path metadata_path;
    std::filesystem::path output_path;
    std::string ledger_sha256;
    std::uint64_t ledger_bytes{};
    corpus::associations::FrozenMetadata metadata;
};

[[nodiscard]] std::string json(std::string_view value);
[[nodiscard]] std::string hash(std::span<const std::byte> bytes);
[[nodiscard]] std::filesystem::path data_root(const std::filesystem::path& root);
[[nodiscard]] std::string compiler_identity();
void write_diagnostics(std::ostream& output, const std::vector<Row>& rows,
    const std::vector<eawr::vfs::ManifestResolution>& resolutions, std::size_t passed,
    std::size_t failures, const std::unordered_map<std::string, ModelEntry>& models,
    std::size_t inconsistencies);
[[nodiscard]] std::optional<std::string> read_file(const std::filesystem::path& path);
[[nodiscard]] std::optional<std::string> bytes_sha256(const std::optional<std::string>& bytes);
[[nodiscard]] std::optional<std::string> file_sha256(const std::filesystem::path& path);
[[nodiscard]] corpus::associations::AssetIdentity asset_identity(const eawr::vfs::AssetRecord& record, std::string sha256);
[[nodiscard]] corpus::associations::AssetIdentity missing_identity(const std::string& path);
int run_validation(int argc, char** argv);
} // namespace eawr_validation
