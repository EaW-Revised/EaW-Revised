#include "eawr/presentation/ui/load_game.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <limits>
#include <utility>

namespace eawr::presentation::ui {
namespace {

using Path = platform::Path;

constexpr std::array<std::uint8_t, 8> replay_magic{'E', 'A', 'W', 'R', 'P', 'L', 'Y', 0};
constexpr std::size_t fixed_header_size = 104; // versions 2 to 5 share these 104 bytes
constexpr std::uint16_t first_tactical_version = 2;
constexpr std::uint16_t last_tactical_version = 5;
constexpr std::uint16_t first_extension_version = 4;
constexpr std::uint16_t skirmish_setup_tag = 2; // SKSU
constexpr std::size_t max_string = 1024;

class Cursor final {
public:
    explicit Cursor(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}
    [[nodiscard]] std::size_t offset() const noexcept { return offset_; }
    [[nodiscard]] std::size_t remaining() const noexcept { return bytes_.size() - offset_; }
    void seek(std::size_t offset) noexcept { offset_ = offset; }
    [[nodiscard]] bool u16(std::uint16_t& value) {
        std::uint64_t wide{};
        if (!read(2, wide)) return false;
        value = static_cast<std::uint16_t>(wide);
        return true;
    }
    [[nodiscard]] bool u32(std::uint32_t& value) {
        std::uint64_t wide{};
        if (!read(4, wide)) return false;
        value = static_cast<std::uint32_t>(wide);
        return true;
    }
    [[nodiscard]] bool u64(std::uint64_t& value) { return read(8, value); }
    [[nodiscard]] bool text(std::string& value) {
        std::uint16_t length{};
        if (!u16(length) || length > max_string || remaining() < length) return false;
        value.assign(reinterpret_cast<const char*>(bytes_.data() + offset_), length);
        offset_ += length;
        return true;
    }

private:
    [[nodiscard]] bool read(std::size_t width, std::uint64_t& value) {
        if (remaining() < width) return false;
        value = 0;
        for (std::size_t index = 0; index < width; ++index) {
            value |= static_cast<std::uint64_t>(bytes_[offset_ + index]) << (8U * index);
        }
        offset_ += width;
        return true;
    }

    std::span<const std::uint8_t> bytes_;
    std::size_t offset_{};
};

[[nodiscard]] ReplayHeaderSummary refuse(ReplayHeaderSummary summary, std::string problem) {
    summary.valid = false;
    summary.problem = std::move(problem);
    return summary;
}

[[nodiscard]] std::string sort_key(const LoadGameEntry& entry) { return platform::replay_name_key(entry.name) + '\n' + entry.file_name; }

} // namespace

std::uint64_t ReplayHeaderSummary::duration_seconds() const noexcept {
    if (tick_numerator == 0 || tick_denominator == 0) return 0;
    const auto whole = final_tick_count / tick_denominator;
    const auto fraction = (final_tick_count % tick_denominator) * tick_numerator / tick_denominator;
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    if (whole > (maximum - fraction) / tick_numerator) return maximum;
    return whole * tick_numerator + fraction;
}

ReplayHeaderSummary read_replay_header(const std::span<const std::uint8_t> bytes) {
    ReplayHeaderSummary summary;
    if (bytes.size() < replay_magic.size() + 4U
        || !std::equal(replay_magic.begin(), replay_magic.end(), bytes.begin())) {
        return refuse(summary, "not a replay file");
    }
    Cursor cursor(bytes);
    cursor.seek(8);
    std::uint16_t header_size{};
    if (!cursor.u16(summary.format_version) || !cursor.u16(header_size)) return refuse(summary, "truncated header");
    if (summary.format_version < first_tactical_version || summary.format_version > last_tactical_version) {
        return refuse(summary, "replay format version " + std::to_string(summary.format_version) + " cannot be played");
    }
    if (header_size < fixed_header_size || bytes.size() < fixed_header_size) return refuse(summary, "truncated header");
    cursor.seek(24);
    if (!cursor.u32(summary.tick_numerator) || !cursor.u32(summary.tick_denominator)) {
        return refuse(summary, "truncated header");
    }
    cursor.seek(40);
    if (!cursor.u64(summary.final_tick_count)) return refuse(summary, "truncated header");
    if (summary.tick_numerator == 0 || summary.tick_denominator == 0) return refuse(summary, "invalid tick rate");

    if (summary.format_version >= first_extension_version && header_size > fixed_header_size) {
        if (bytes.size() < header_size) return refuse(summary, "truncated header");
        cursor.seek(fixed_header_size);
        std::uint32_t records{};
        if (!cursor.u32(records)) return refuse(summary, "truncated header");
        for (std::uint32_t record = 0; record < records; ++record) {
            std::uint16_t tag{}, length{};
            if (!cursor.u16(tag) || !cursor.u16(length) || cursor.offset() + length > header_size) {
                return refuse(summary, "truncated header extension");
            }
            if (tag == skirmish_setup_tag) {
                // The map path is the body's first string.
                Cursor body(bytes.subspan(cursor.offset(), length));
                if (!body.text(summary.map)) return refuse(summary, "invalid setup record");
            }
            cursor.seek(cursor.offset() + length);
        }
    }
    summary.valid = true;
    return summary;
}

std::string format_unix_minutes(const std::int64_t unix_seconds) {
    // Civil date from days since the epoch (proleptic Gregorian, UTC).
    const std::int64_t seconds_per_day = 86400;
    std::int64_t days = unix_seconds / seconds_per_day;
    std::int64_t rest = unix_seconds % seconds_per_day;
    if (rest < 0) {
        rest += seconds_per_day;
        --days;
    }
    days += 719468;
    const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const std::int64_t day_of_era = days - era * 146097;
    const std::int64_t year_of_era =
        (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
    const std::int64_t day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    const std::int64_t month_index = (5 * day_of_year + 2) / 153;
    const std::int64_t day = day_of_year - (153 * month_index + 2) / 5 + 1;
    const std::int64_t month = month_index < 10 ? month_index + 3 : month_index - 9;
    const std::int64_t year = year_of_era + era * 400 + (month <= 2 ? 1 : 0);
    std::array<char, 96> text{};
    std::snprintf(text.data(), text.size(), "%04lld-%02lld-%02lld %02lld:%02lld", static_cast<long long>(year),
                  static_cast<long long>(month), static_cast<long long>(day),
                  static_cast<long long>(rest / 3600), static_cast<long long>(rest % 3600 / 60));
    return text.data();
}

std::string format_duration(const std::uint64_t seconds) {
    std::array<char, 96> text{};
    const auto hours = static_cast<unsigned long long>(seconds / 3600);
    const auto minutes = static_cast<unsigned long long>(seconds % 3600 / 60);
    const auto rest = static_cast<unsigned long long>(seconds % 60);
    if (hours > 0) std::snprintf(text.data(), text.size(), "%llu:%02llu:%02llu", hours, minutes, rest);
    else std::snprintf(text.data(), text.size(), "%llu:%02llu", minutes, rest);
    return text.data();
}

std::string map_label(const std::string_view logical_path) {
    if (logical_path.empty()) return {};
    const std::size_t slash = logical_path.find_last_of("/\\");
    std::string_view name = slash == std::string_view::npos ? logical_path : logical_path.substr(slash + 1);
    const std::size_t dot = name.find_last_of('.');
    if (dot != std::string_view::npos && dot > 0) name = name.substr(0, dot);
    return std::string(name);
}

LoadGameEntry make_replay_entry(const std::string_view file_name, const std::int64_t modified_unix,
                                const std::span<const std::uint8_t> header) {
    LoadGameEntry entry;
    entry.file_name = std::string(file_name);
    entry.name = entry.file_name;
    if (platform::plain_replay_name(file_name)) entry.name.resize(file_name.size() - replay_file_extension.size());
    entry.modified_unix = modified_unix;
    const ReplayHeaderSummary summary = read_replay_header(header);
    entry.playable = summary.valid;
    entry.problem = summary.problem;
    entry.map = summary.map;
    entry.duration_seconds = summary.duration_seconds();
    entry.text = entry.name + "  " + format_unix_minutes(modified_unix);
    if (summary.valid) {
        if (const std::string label = map_label(summary.map); !label.empty()) entry.text += "  " + label;
        entry.text += "  " + format_duration(entry.duration_seconds);
    }
    return entry;
}

LoadGameModel::LoadGameModel(Path store, std::shared_ptr<SavedGameStore> saves)
    : store_(std::move(store)), saves_(std::move(saves)) {}

void LoadGameModel::refresh() {
    const LoadGameEntry* before = selected_entry();
    const std::string kept = before != nullptr ? before->file_name : std::string{};
    set_recordings(scan_replay_store(store_));
    if (saves_ != nullptr) {
        SavedGameListing listing = saves_->list();
        saved_problem_ = std::move(listing.problem);
        set_saved_games(std::move(listing.entries));
    }
    if (kept.empty()) return;
    const auto& rows = visible();
    for (std::size_t index = 0; index < rows.size(); ++index) {
        if (rows[index].file_name == kept) selected_ = index;
    }
}

void LoadGameModel::set_recordings(std::vector<LoadGameEntry> entries) {
    std::sort(entries.begin(), entries.end(),
              [](const LoadGameEntry& left, const LoadGameEntry& right) { return sort_key(left) < sort_key(right); });
    recordings_ = std::move(entries);
    selected_.reset();
}

void LoadGameModel::set_saved_games(std::vector<LoadGameEntry> entries) {
    saved_ = std::move(entries);
    selected_.reset();
}

void LoadGameModel::set_saved_load_unavailable(std::string reason) { saved_load_unavailable_ = std::move(reason); }

void LoadGameModel::set_show_recordings(const bool show) {
    if (show == show_recordings_) return;
    show_recordings_ = show;
    selected_.reset();
}

const std::vector<LoadGameEntry>& LoadGameModel::visible() const noexcept { return show_recordings_ ? recordings_ : saved_; }

void LoadGameModel::select(const std::optional<std::size_t> index) {
    if (index && *index >= visible().size()) selected_.reset();
    else selected_ = index;
}

const LoadGameEntry* LoadGameModel::selected_entry() const noexcept {
    return selected_ && *selected_ < visible().size() ? &visible()[*selected_] : nullptr;
}

bool LoadGameModel::can_load() const noexcept {
    const LoadGameEntry* entry = selected_entry();
    if (entry == nullptr || !entry->playable) return false;
    return show_recordings_ || saved_load_unavailable_.empty();
}

std::string LoadGameModel::load_disabled_reason() const {
    const LoadGameEntry* entry = selected_entry();
    if (entry == nullptr) return show_recordings_ ? "no recording is selected" : "no saved game is selected";
    if (!entry->playable) return entry->problem;
    return show_recordings_ ? std::string{} : saved_load_unavailable_;
}

std::optional<PlayRequest> LoadGameModel::play_request() const {
    if (!show_recordings_ || !can_load()) return std::nullopt;
    const LoadGameEntry& entry = *selected_entry();
    return PlayRequest{store_ / path_from_utf8(entry.file_name), entry.map};
}

std::optional<SavedGameRequest> LoadGameModel::saved_request() const {
    if (show_recordings_ || !can_load() || saves_ == nullptr) return std::nullopt;
    return SavedGameRequest{saves_->directory(), selected_entry()->file_name};
}

bool LoadGameModel::can_delete() const noexcept {
    if (selected_entry() == nullptr) return false;
    return show_recordings_ || saves_ != nullptr;
}

MessageBoxRequest LoadGameModel::delete_confirmation() const {
    MessageBoxRequest request;
    request.kind = BoxKind::standard;
    request.caption = "TEXT_DELETE_CONFIRM";
    request.buttons = {BoxButton{"TEXT_BUTTON_YES", 1, false}, BoxButton{"TEXT_BUTTON_NO", 0, true}};
    return request;
}

DeleteResult LoadGameModel::confirm_delete(const std::string_view file_name) {
    std::vector<LoadGameEntry>& rows = show_recordings_ ? recordings_ : saved_;
    const auto found = std::find_if(rows.begin(), rows.end(),
                                    [&](const LoadGameEntry& entry) { return entry.file_name == file_name; });
    if (found == rows.end()) return DeleteResult::missing;
    DeleteResult result = DeleteResult::failed;
    if (show_recordings_) result = delete_replay(store_, file_name);
    else if (saves_ != nullptr) result = saves_->erase(file_name);
    if (result == DeleteResult::deleted || result == DeleteResult::missing) {
        // Keep the selection on the entry it was on, by name.
        const std::string kept = selected_entry() != nullptr && selected_entry()->file_name != file_name
            ? selected_entry()->file_name : std::string{};
        rows.erase(found);
        selected_.reset();
        for (std::size_t index = 0; index < rows.size() && !kept.empty(); ++index) {
            if (rows[index].file_name == kept) selected_ = index;
        }
    }
    return result;
}

} // namespace eawr::presentation::ui
