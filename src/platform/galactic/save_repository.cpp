#include "save_repository.hpp"
#include "live_save_transport.hpp"
#include "eawr/checkpoint/checkpoint.hpp"
#include "eawr/platform/publish_files.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <mutex>

namespace eawr::platform::save_detail {
namespace {
namespace fs = std::filesystem;

// All repository entry points share this lock, including session-free menu calls.
// Helpers suffixed _unlocked run only while an entry point holds it. No session
// locks or user callbacks are acquired or invoked within the repository.
std::mutex repository_mutex;

[[nodiscard]] fs::path native_path(const std::string_view value) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(value.data()), value.size()));
}
template<class T> core::Result<T> fail(std::string message) {
    core::Diagnostic error; error.code = "EAWR-SAVE-0101"; error.message = std::move(message);
    return core::Result<T>::failure(std::move(error));
}
fs::path slot_path(const std::string& directory, std::string_view name) {
    return native_path(directory) / (std::string(name) + ".eawrsave");
}
template<class FileClock = fs::file_time_type::clock>
auto system_file_time(fs::file_time_type modified) {
    // GCC's file clock supplies to_sys; MSVC supplies to_utc instead. Avoid
    // clock_cast, which is absent in older supported standard libraries.
    if constexpr (requires { FileClock::to_sys(modified); }) return FileClock::to_sys(modified);
    else {
        const auto file_now = FileClock::now();
        const auto system_now = std::chrono::system_clock::now();
        return system_now + std::chrono::duration_cast<std::chrono::system_clock::duration>(modified - file_now);
    }
}
}
std::filesystem::path slot_file(const std::string& directory, std::string_view name) {
    return slot_path(directory, name);
}
bool valid_name(std::string_view name) noexcept {
    return !name.empty() && name.size() <= 64 && name.front() != ' ' && name.back() != ' '
        && std::all_of(name.begin(), name.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                || (c >= '0' && c <= '9') || c == ' ' || c == '-' || c == '_';
        });
}
core::Result<void> write(const std::string& directory, std::string_view name,
    std::span<const std::uint8_t> bytes, bool overwrite) {
    const std::lock_guard lock(repository_mutex);
    if (directory.empty() || !valid_name(name)) return fail<void>("Choose a valid save name and directory.");
    std::error_code error; fs::create_directories(native_path(directory), error);
    if (error) return fail<void>("Cannot create save directory: " + error.message());
    const auto target = slot_path(directory, name);
    if (fs::is_symlink(fs::symlink_status(target, error))) return fail<void>("Save slot is a symbolic link.");
    error.clear();
    if (!overwrite && fs::exists(target, error)) return fail<void>("Save already exists; confirm overwrite.");
    if (error) return fail<void>("Cannot inspect save slot: " + error.message());
    const auto report = publish_path_files({{"save", target, std::string(
        reinterpret_cast<const char*>(bytes.data()), bytes.size())}});
    if (report.failure) return fail<void>(report.failure->message);
    return core::Result<void>::success();
}
namespace {
core::Result<std::vector<std::uint8_t>> read_unlocked(const std::string& directory, std::string_view name) {
    using Bytes = std::vector<std::uint8_t>;
    if (directory.empty() || !valid_name(name)) return fail<Bytes>("Invalid save name or directory.");
    const auto target = slot_path(directory, name);
    std::error_code error;
    if (fs::is_symlink(fs::symlink_status(target, error))) return fail<Bytes>("Save slot is a symbolic link.");
    error.clear(); const auto size = fs::file_size(target, error);
    if (error) return fail<Bytes>("Cannot read save: " + error.message());
    if (size > checkpoint::max_bytes) return fail<Bytes>("Save exceeds the checkpoint byte limit.");
    Bytes bytes(static_cast<std::size_t>(size)); std::ifstream input(target, std::ios::binary);
    if (!input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size)))
        return fail<Bytes>("Could not read the complete save.");
    // Refuse a file that grew between size and read as well as a short read.
    if (input.peek() != std::char_traits<char>::eof()) return fail<Bytes>("Save changed while reading.");
    return core::Result<Bytes>::success(std::move(bytes));
}
LiveSaveSlot describe_unlocked(const std::string& directory, std::string_view name) {
    LiveSaveSlot slot; slot.name = std::string(name);
    slot.description = slot.name == "Quick Save" ? "[QuickSave]"
        : slot.name == "Auto Save" ? "[AutoSave]" : slot.name;
    std::error_code error;
    const auto modified = fs::last_write_time(slot_path(directory, name), error);
    if (!error) slot.last_write_seconds = std::chrono::duration_cast<std::chrono::seconds>(
        system_file_time(modified).time_since_epoch()).count();
    auto bytes = read_unlocked(directory, slot.name);
    if (!bytes) { slot.diagnostic = bytes.error(); return slot; }
    slot.bytes = bytes.value().size(); auto parsed = checkpoint::parse_checkpoint(bytes.value());
    if (!parsed) { slot.diagnostic = parsed.error(); return slot; }
    slot.completed_tick = parsed.value().header.completed_tick;
    slot.galactic_frame = parsed.value().header.galactic_frame;
    slot.child = (parsed.value().header.flags & checkpoint::flag_child) != 0;
    auto campaign = decode_campaign(parsed.value());
    if (!campaign) slot.diagnostic = campaign.error();
    else slot.campaign = std::move(campaign).value();
    return slot;
}
} // namespace
core::Result<std::vector<std::uint8_t>> read(const std::string& directory, std::string_view name) {
    const std::lock_guard lock(repository_mutex);
    return read_unlocked(directory, name);
}
LiveSaveSlot describe(const std::string& directory, std::string_view name) {
    const std::lock_guard lock(repository_mutex);
    return describe_unlocked(directory, name);
}
core::Result<std::vector<LiveSaveSlot>> list(const std::string& directory) {
    const std::lock_guard lock(repository_mutex);
    using Slots = std::vector<LiveSaveSlot>;
    if (directory.empty()) return fail<Slots>("Save directory is unavailable.");
    std::error_code error;
    if (!fs::exists(native_path(directory), error) && !error) return core::Result<Slots>::success({});
    fs::directory_iterator at(native_path(directory), error), end;
    if (error) return fail<Slots>("Cannot list saves: " + error.message());
    Slots slots;
    for (; at != end; at.increment(error)) {
        if (error) return fail<Slots>("Cannot list saves: " + error.message());
        const auto path = at->path();
        const auto encoded_name = path.stem().generic_u8string();
        const std::string name(encoded_name.begin(), encoded_name.end());
        if (path.extension() != ".eawrsave" || !valid_name(name)) continue;
        slots.push_back(describe_unlocked(directory, name));
        if (slots.size() > 1024) return fail<Slots>("Save list exceeds the 1024-slot limit.");
    }
    if (error) return fail<Slots>("Cannot list saves: " + error.message());
    std::sort(slots.begin(), slots.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
    return core::Result<Slots>::success(std::move(slots));
}
core::Result<void> erase(const std::string& directory, std::string_view name) {
    const std::lock_guard lock(repository_mutex);
    if (directory.empty() || !valid_name(name)) return fail<void>("Invalid save name or directory.");
    std::error_code error; const auto target = slot_path(directory, name);
    if (fs::is_symlink(fs::symlink_status(target, error))) return fail<void>("Save slot is a symbolic link.");
    error.clear(); const bool removed = fs::remove(target, error);
    if (error || !removed) return fail<void>("Could not delete the save" + (error ? ": " + error.message() : "."));
    return core::Result<void>::success();
}
} // namespace eawr::platform::save_detail

namespace eawr::platform {
core::Result<LiveSaveSlot> read_save_slot(std::string_view directory, std::string_view name) {
    const std::lock_guard lock(save_detail::repository_mutex);
    const std::string root(directory);
    if (root.empty() || !save_detail::valid_name(name)) {
        core::Diagnostic error; error.code = "EAWR-SAVE-0101"; error.message = "Invalid save name or directory.";
        return core::Result<LiveSaveSlot>::failure(std::move(error));
    }
    std::error_code error;
    if (!std::filesystem::is_regular_file(save_detail::slot_file(root, name), error)) {
        core::Diagnostic missing; missing.code = "EAWR-SAVE-0101"; missing.message = "Save not found: " + std::string(name);
        return core::Result<LiveSaveSlot>::failure(std::move(missing));
    }
    return core::Result<LiveSaveSlot>::success(save_detail::describe_unlocked(root, name));
}
} // namespace eawr::platform
