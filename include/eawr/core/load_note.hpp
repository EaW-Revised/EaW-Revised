#pragma once

#include "eawr/core/diagnostic.hpp"
#include "eawr/core/result.hpp"

#include <algorithm>
#include <compare>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace eawr::core {

enum class LoadCategory : std::uint8_t { refused, retail_accepts, unknown_degrade };
enum class LoadPhase : std::uint8_t { load, runtime };
enum class LoadAction : std::uint8_t {
    refused, accepted, default_used, clamped, skipped_object, skipped_placement, runtime_degraded
};
struct LoadLayer {
    std::string mod_id{"stock"};
    std::optional<std::uint64_t> chain_position;
    auto operator<=>(const LoadLayer&) const = default;
};
struct LoadNote {
    std::string code;
    LoadCategory classification{LoadCategory::unknown_degrade};
    LoadLayer layer;
    std::string file;
    std::optional<std::uint64_t> line;
    std::string object;
    std::string tag;
    std::string authored_value;
    LoadAction action{LoadAction::accepted};
    std::string effective_value;
    std::string retail{"unknown"};
    LoadPhase phase{LoadPhase::load};
    std::string diagnostic_code;
    std::string message;
};
struct LoadNoteHeader {
    std::string chain_fingerprint;
    std::optional<std::string> profile_id;
    std::string game_version;
    std::string written_at;
};
inline constexpr std::size_t load_note_file_cap = 1024;
inline constexpr std::size_t load_note_byte_cap = 512 * 1024;

namespace load_note_codes {
inline constexpr std::string_view missing_include = "EAWR-LOAD-0001";
} // namespace load_note_codes

[[nodiscard]] std::string_view to_string(LoadCategory value) noexcept;
[[nodiscard]] std::string_view to_string(LoadAction value) noexcept;
[[nodiscard]] Diagnostic load_note_diagnostic(const LoadNote& note);
[[nodiscard]] std::string load_notes_json(std::span<const LoadNote> notes, const LoadNoteHeader& header = {});
[[nodiscard]] std::string load_notes_text(std::span<const LoadNote> notes);

// Each partition owns its buffer. No shared state, locks, I/O or callbacks in append.
// The owner must join workers before merging; a buffer cannot have two writers.
class LoadNoteBuffer {
public:
    void append(LoadNote note);
    void runtime(LoadNote note);
private:
    friend class LoadNoteCollector;
    struct Entry { LoadNote note; bool runtime{}; };
    std::vector<Entry> entries_;
};

// One collector per match/load. Its owner alone calls merge/snapshot/reset after
// a partition phase joins. Never put this diagnostic state in replay/checkpoints.
class LoadNoteCollector {
public:
    void merge(LoadNoteBuffer& buffer);
    void append(LoadNote note);
    [[nodiscard]] std::vector<LoadNote> snapshot() const;
    void reset();
private:
    std::vector<LoadNoteBuffer::Entry> entries_;
};

// Resolve source layer IDs against the mounted chain before publishing. Stock
// IDs are explicit inputs, so a mod named "base" is not mistaken for stock.
void bind_load_note_layers(std::span<LoadNote> notes, std::span<const std::string> ordered_mods,
                           std::span<const std::string> stock_layers);

// Shared family seam: normalizers must be deterministic and idempotent. Always
// normalize the owned copy before checking invariants; callers retain input data.
template<class Table, class Normalize, class Validate>
Result<Table> normalize_owned_load_table(const Table& source, LoadNoteBuffer& notes,
                                         Normalize normalize, Validate validate) {
    auto owned = source;
    normalize(owned, notes);
    if (auto valid = validate(owned); !valid) return Result<Table>::failure(valid.error());
    return Result<Table>::success(std::move(owned));
}

// Helpers emit only when a value actually changes. Values must be ordered and
// representable in T; saturate_note handles cross-width integer conversion.
template<class T>
T clamp_note(T value, T minimum, T maximum, LoadNote note, LoadNoteBuffer& notes) {
    const auto effective = std::clamp(value, minimum, maximum);
    if (effective != value) {
        note.classification = LoadCategory::unknown_degrade;
        note.action = LoadAction::clamped;
        note.effective_value = std::to_string(effective);
        notes.append(std::move(note));
    }
    return effective;
}

template<class To, class From>
To saturate_note(From value, LoadNote note, LoadNoteBuffer& notes);

template<class T>
T default_note(std::optional<T> value, T fallback, LoadNote note, LoadNoteBuffer& notes) {
    if (value) return *value;
    note.classification = LoadCategory::unknown_degrade;
    note.action = LoadAction::default_used;
    note.effective_value = std::to_string(fallback);
    notes.append(std::move(note));
    return fallback;
}

} // namespace eawr::core

namespace eawr::core {
template<class To, class From>
To saturate_note(From value, LoadNote note, LoadNoteBuffer& notes) {
    static_assert(std::is_integral_v<To> && std::is_integral_v<From>);
    To effective{};
    if (std::cmp_less(value, std::numeric_limits<To>::lowest())) effective = std::numeric_limits<To>::lowest();
    else if (std::cmp_greater(value, std::numeric_limits<To>::max())) effective = std::numeric_limits<To>::max();
    else return static_cast<To>(value);
    note.classification = LoadCategory::unknown_degrade;
    note.action = LoadAction::clamped;
    note.effective_value = std::to_string(effective);
    notes.append(std::move(note));
    return effective;
}
} // namespace eawr::core
