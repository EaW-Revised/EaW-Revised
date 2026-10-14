#include "eawr/platform/replay_store.hpp"
#include "eawr/presentation/ui/load_game.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <fstream>
#include <system_error>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace eawr::platform {
using presentation::ui::LoadGameEntry;
using presentation::ui::DeleteResult;
using presentation::ui::make_replay_entry;
using presentation::ui::replay_file_extension;
using presentation::ui::replay_header_read_limit;
namespace fs = std::filesystem;

namespace {
// BEP-05: Windows filename components commonly allow 255 UTF-16 units, including
// the suffix. Decode before constructing a native path so malformed UTF-8 is refused.
[[nodiscard]] std::optional<std::size_t> utf16_units(const std::string_view value) {
    std::size_t units = 0;
    for (std::size_t offset = 0; offset < value.size();) {
        const auto lead = static_cast<unsigned char>(value[offset++]);
        std::uint32_t point = lead;
        unsigned tail = 0;
        std::uint32_t minimum = 0;
        if (lead < 0x80U) {}
        else if (lead >= 0xC2U && lead <= 0xDFU) { point = lead & 0x1FU; tail = 1; minimum = 0x80; }
        else if (lead >= 0xE0U && lead <= 0xEFU) { point = lead & 0x0FU; tail = 2; minimum = 0x800; }
        else if (lead >= 0xF0U && lead <= 0xF4U) { point = lead & 0x07U; tail = 3; minimum = 0x10000; }
        else return std::nullopt;
        if (value.size() - offset < tail) return std::nullopt;
        for (unsigned index = 0; index < tail; ++index) {
            const auto byte = static_cast<unsigned char>(value[offset++]);
            if ((byte & 0xC0U) != 0x80U) return std::nullopt;
            point = (point << 6U) | (byte & 0x3FU);
        }
        if (point < minimum || point > 0x10FFFFU || (point >= 0xD800U && point <= 0xDFFFU)) return std::nullopt;
        units += point >= 0x10000U ? 2U : 1U;
    }
    return units;
}
} // namespace

std::string replay_name_key(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

// A plain file name: no directory part, no control characters (a NUL ends a Win32 name early), not
// hidden, the replay extension, and something before it.
bool plain_replay_name(std::string_view name) {
    if (name.empty() || name.front() == '.') return false;
    if (std::any_of(name.begin(), name.end(), [](const char c) {
            return static_cast<unsigned char>(c) < 0x20U || static_cast<unsigned char>(c) == 0x7FU;
        })) {
        return false;
    }
    if (name.find_first_of("/\\:") != std::string_view::npos) return false;
    if (name.size() <= replay_file_extension.size()) return false;
    return replay_name_key(name.substr(name.size() - replay_file_extension.size())) == replay_file_extension;
}

std::string replay_name_problem(const std::string_view name) {
    if (name.empty()) return "Enter a recording name.";
    const auto length = utf16_units(name);
    if (!length) return "The recording name is not valid UTF-8.";
    if (*length + replay_file_extension.size() > 255U) return "The recording name is too long.";
#if !defined(_WIN32)
    if (name.size() + replay_file_extension.size() > 255U) return "The recording name is too long.";
#endif
    if (name.size() >= replay_file_extension.size()
        && replay_name_key(name.substr(name.size() - replay_file_extension.size())) == replay_file_extension)
        return "Enter the recording name without the replay suffix.";
    if (name.front() == '.' || name.front() == ' ' || name.back() == '.' || name.back() == ' ')
        return "The name cannot begin with a dot or space, or end with a dot or space.";
    if (name.find_first_of("/\\:<>\"|?*") != std::string_view::npos
        || std::any_of(name.begin(), name.end(), [](const unsigned char c) { return c < 32U || c == 127U; }))
        return "The name contains a character that cannot be used in a file name.";
    const std::string stem = replay_name_key(name.substr(0, name.find('.')));
    if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul"
        || (stem.size() == 4 && (stem.starts_with("com") || stem.starts_with("lpt"))
            && stem[3] >= '0' && stem[3] <= '9')
        || ((stem.starts_with("com") || stem.starts_with("lpt"))
            && (stem.substr(3) == "\xC2\xB9" || stem.substr(3) == "\xC2\xB2" || stem.substr(3) == "\xC2\xB3")))
        return "That name is reserved by the filesystem.";
    return {};
}

ReplayWriteResult write_replay_store(const fs::path& store, const std::string_view name,
    const std::span<const std::uint8_t> bytes, const bool overwrite, std::string& error) {
    error = replay_name_problem(name);
    if (!error.empty()) return ReplayWriteResult::invalid_name;
    if (store.empty() || bytes.empty()) { error = "There is no recording to save."; return ReplayWriteResult::failed; }
    std::error_code code;
    const fs::path target = store / path_from_utf8(std::string(name) + std::string(replay_file_extension));
    const auto status = fs::symlink_status(target, code);
    if (fs::exists(status)) {
        if (!fs::is_regular_file(status)) { error = "The recording name is not a regular file."; return ReplayWriteResult::failed; }
        if (!overwrite) return ReplayWriteResult::exists;
    } else if (code && code != std::errc::no_such_file_or_directory) {
        error = "Cannot inspect the recording: " + code.message(); return ReplayWriteResult::failed;
    }
    code.clear();
    fs::create_directories(store, code);
    if (code) { error = "Cannot create the recording folder: " + code.message(); return ReplayWriteResult::failed; }
    // An exclusively created sibling directory keeps temporary writes away from other writers
    // and pre-existing links. Install only after the complete bytes have been flushed and closed.
    static std::atomic<std::uint64_t> sequence{};
    fs::path staging;
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        staging = store / (".replay-write-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
            + "-" + std::to_string(sequence.fetch_add(1)));
        if (fs::create_directory(staging, code)) break;
        if (code) { error = "Cannot stage the recording: " + code.message(); return ReplayWriteResult::failed; }
        staging.clear();
    }
    if (staging.empty()) { error = "Cannot stage the recording."; return ReplayWriteResult::failed; }
    const fs::path temporary = staging / "recording";
    ReplayWriteResult result = ReplayWriteResult::failed;
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        output.flush();
        output.close();
        if (output) {
#if defined(_WIN32)
            const DWORD flags = overwrite ? MOVEFILE_REPLACE_EXISTING : 0;
            if (MoveFileExW(temporary.c_str(), target.c_str(), flags)) result = ReplayWriteResult::saved;
            else {
                const DWORD failure = GetLastError();
                code = std::error_code(static_cast<int>(failure), std::system_category());
                if (!overwrite && (failure == ERROR_ALREADY_EXISTS || failure == ERROR_FILE_EXISTS)) result = ReplayWriteResult::exists;
            }
#else
            if (overwrite) fs::rename(temporary, target, code);
            else fs::create_hard_link(temporary, target, code);
            if (!code) result = ReplayWriteResult::saved;
            else if (!overwrite && code == std::errc::file_exists) result = ReplayWriteResult::exists;
#endif
        }
    }
    if (result == ReplayWriteResult::failed) error = "Cannot save the recording" + (code ? ": " + code.message() : ".");
    fs::remove(temporary, code);
    fs::remove(staging, code);
    return result;
}

std::vector<LoadGameEntry> scan_replay_store(const fs::path& store) {
    std::vector<LoadGameEntry> entries;
    std::error_code error;
    if (store.empty() || !fs::is_directory(store, error)) return entries;
    for (fs::directory_iterator it(store, error), end; !error && it != end; it.increment(error)) {
        const fs::path path = it->path();
        std::error_code entry_error;
        // Inspect the directory entry itself: is_regular_file() follows symlinks, which could
        // expose a recording outside the store that delete_replay() correctly refuses to erase.
        if (!fs::is_regular_file(it->symlink_status(entry_error)) || entry_error) continue;
        const std::string name = utf8_from_path(path.filename());
        if (!plain_replay_name(name)) continue;
        std::int64_t modified = 0;
        const auto time = fs::last_write_time(path, entry_error);
        if (!entry_error) {
            // file_clock to system_clock without C++20's clock_cast, which MSVC and libstdc++ share
            // only in recent versions: shift by the clocks' current offset.
            const auto system_now = std::chrono::system_clock::now();
            const auto file_now = fs::file_time_type::clock::now();
            const auto shifted = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                time - file_now + system_now);
            modified = std::chrono::duration_cast<std::chrono::seconds>(shifted.time_since_epoch()).count();
        }
        std::vector<std::uint8_t> head(replay_header_read_limit);
        std::ifstream input(path, std::ios::binary);
        input.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(head.size()));
        head.resize(static_cast<std::size_t>(std::max<std::streamsize>(input.gcount(), 0)));
        entries.push_back(make_replay_entry(name, modified, head));
    }
    return entries;
}

DeleteResult delete_replay(const fs::path& store, const std::string_view file_name) {
    if (store.empty() || !plain_replay_name(file_name)) return DeleteResult::outside_store;
    const fs::path path = store / path_from_utf8(file_name);
    std::error_code error;
    // The name is plain, so the parent must be the store itself; a link that leads out is not followed.
    if (fs::is_symlink(fs::symlink_status(path, error))) return DeleteResult::outside_store;
    if (!fs::is_regular_file(path, error)) return DeleteResult::missing;
    return fs::remove(path, error) && !error ? DeleteResult::deleted : DeleteResult::failed;
}

} // namespace eawr::platform
