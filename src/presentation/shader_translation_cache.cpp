#include "eawr/presentation/shader_translation_cache.hpp"
#include "eawr/core/sha256.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <mutex>
#include <string_view>
#include <system_error>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace eawr::presentation::shader_cache {
namespace {
namespace fs = std::filesystem;
using Bytes = std::vector<std::uint8_t>;
constexpr std::size_t header_size = 8 + 4 + 8 + 64 + 32;
constexpr std::string_view magic = "EAWRSTC1";
std::mutex writer_mutex;

bool digest(const std::string_view value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](const char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
void word(Bytes& out, const std::uint64_t value, const unsigned count = 8) {
    for (unsigned i = 0; i < count; ++i) out.push_back(static_cast<std::uint8_t>(value >> (i * 8)));
}
void text(Bytes& out, const std::string_view value) {
    word(out, value.size());
    out.insert(out.end(), value.begin(), value.end());
}
void tokens(Bytes& out, const std::vector<Token>& values) {
    word(out, values.size());
    for (const auto& value : values) { text(out, value.name); text(out, value.value); }
}
bool valid(const Validate& validate, const std::span<const std::uint8_t> payload) {
    if (!validate) return false;
    try { return validate(payload); } catch (...) { return false; }
}
bool directory(const fs::path& path, const bool create) {
    std::error_code error;
    if (create) fs::create_directories(path, error);
    if (error) return false;
    const auto status = fs::symlink_status(path, error);
    return !error && fs::is_directory(status) && !fs::is_symlink(status);
}
struct Lease final {
#ifdef _WIN32
    HANDLE handle{INVALID_HANDLE_VALUE};
#else
    int handle{-1};
#endif
    bool held{};
    bool invalid{};
    explicit Lease(const fs::path& root) {
        const auto path = root / ".writer";
        std::error_code error;
        const auto status = fs::symlink_status(path, error);
        if (!error && status.type() != fs::file_type::not_found &&
            (!fs::is_regular_file(status) || fs::is_symlink(status))) {
            invalid = true;
            return;
        }
#ifdef _WIN32
        handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (handle == INVALID_HANDLE_VALUE) return;
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(handle, &info)
            || (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
            invalid = true;
            CloseHandle(handle); handle = INVALID_HANDLE_VALUE; return;
        }
        held = true;
#else
        handle = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, S_IRUSR | S_IWUSR);
        if (handle < 0) return;
        struct stat info{};
        if (::fstat(handle, &info) != 0 || !S_ISREG(info.st_mode)) {
            invalid = true;
            ::close(handle); handle = -1; return;
        }
        if (::flock(handle, LOCK_EX | LOCK_NB) != 0) {
            ::close(handle); handle = -1; return;
        }
        held = true;
#endif
    }
    Lease(const Lease&) = delete;
    Lease& operator=(const Lease&) = delete;
    ~Lease() {
#ifdef _WIN32
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
#else
        if (handle >= 0) ::close(handle);
#endif
    }
};
fs::path filename(const fs::path& root, const Key& key) { return root / (key.sha256 + ".eawrshader"); }
Result read(const fs::path& path, const Key& key, const Limits& limits, const Validate& validate) {
    std::error_code error;
    const auto status = fs::symlink_status(path, error);
    if (status.type() == fs::file_type::not_found) return {Status::miss, {}};
    if (error || !fs::is_regular_file(status) || fs::is_symlink(status)) return {Status::io_error, {}};
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) return {Status::io_error, {}};
    const auto end = input.tellg();
    if (end < 0) return {Status::io_error, {}};
    const auto size = static_cast<std::uint64_t>(end);
    if (size > limits.entry_bytes || size > static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max())
        || size > std::numeric_limits<std::size_t>::max()) return {Status::budget, {}};
    if (size < header_size) return {Status::corrupt, {}};
    Bytes bytes(static_cast<std::size_t>(size));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!input || input.peek() != std::char_traits<char>::eof()) return {Status::corrupt, {}};
    const auto number = [&bytes](const std::size_t at, const unsigned count) {
        std::uint64_t result{};
        for (unsigned i = 0; i < count; ++i) result |= std::uint64_t{bytes[at + i]} << (8 * i);
        return result;
    };
    if (!std::equal(magic.begin(), magic.end(), bytes.begin()) || number(8, 4) != 1
        || number(12, 8) != size - header_size
        || !std::equal(key.sha256.begin(), key.sha256.end(), bytes.begin() + 20)) return {Status::corrupt, {}};
    const std::span<const std::uint8_t> payload(bytes.data() + header_size, bytes.size() - header_size);
    const auto hash = core::sha256(payload);
    if (!std::equal(hash.begin(), hash.end(), bytes.begin() + 84) || !valid(validate, payload)) return {Status::corrupt, {}};
    return {Status::hit, Bytes(payload.begin(), payload.end())};
}
struct Entry final {
    fs::path path;
    std::uint64_t bytes{};
    fs::file_time_type recency;
    std::string name;
};
Status reserve(const fs::path& root, const Limits& limits, const std::uint64_t incoming) {
    std::error_code error;
    fs::directory_iterator at(root, error), end;
    if (error) return Status::io_error;
    std::size_t scanned{};
    std::uint64_t total{};
    std::vector<Entry> entries;
    for (; at != end; at.increment(error)) {
        if (error) return Status::io_error;
        if (++scanned > limits.scan_entries) return Status::budget;
        const auto native_name = at->path().filename().native();
        if (native_name.size() != 75) continue;
        std::string name;
        bool ascii = true;
        for (const auto c : native_name) {
            if (static_cast<std::uint32_t>(c) > 127) { ascii = false; break; }
            name.push_back(static_cast<char>(c));
        }
        if (!ascii || !name.ends_with(".eawrshader") || !digest(std::string_view(name).substr(0, 64))) continue;
        const auto status = at->symlink_status(error);
        if (error || !fs::is_regular_file(status) || fs::is_symlink(status)) return Status::io_error;
        const auto bytes = at->file_size(error);
        if (error || bytes > std::numeric_limits<std::uint64_t>::max() - total) return Status::io_error;
        const auto recency = at->last_write_time(error);
        if (error) return Status::io_error;
        total += bytes;
        entries.push_back({at->path(), bytes, recency, name});
    }
    if (error) return Status::io_error;
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        return a.recency == b.recency ? a.name < b.name : a.recency < b.recency;
    });
    std::size_t count = entries.size();
    for (const auto& entry : entries) {
        if (count < limits.entries && total <= limits.total_bytes - incoming) break;
        if (!fs::remove(entry.path, error) || error) return Status::io_error;
        total -= entry.bytes;
        --count;
    }
    return count < limits.entries && total <= limits.total_bytes - incoming ? Status::stored : Status::budget;
}
} // namespace

std::optional<Key> make_key(const Identity& i) {
    if (!digest(i.root_sha256) || !digest(i.package_sha256) || !digest(i.capability_manifest)
        || !digest(i.options_manifest_sha256) || i.descriptor_version == 0
        || i.result_schema_version == 0 || i.options_manifest_version == 0
        || (i.form != Form::compiled && i.form != Form::source)) return std::nullopt;
    const std::array required{i.logical, i.compiler_abi, i.capability_manifest, i.selection_policy, i.resolver_policy,
        i.translator, i.emitter, i.binding_policy, i.state_policy, i.technique_policy, i.admission_policy,
        i.godot_engine, i.shader_abi, i.language, i.renderer, i.backend, i.platform, i.architecture};
    if (std::any_of(required.begin(), required.end(), [](const auto& value) { return value.empty(); })) return std::nullopt;
    Bytes bytes;
    text(bytes, "eawr-shader-translation-cache-key-v1");
    text(bytes, i.logical); text(bytes, i.root_sha256); word(bytes, i.form == Form::source ? 1 : 0);
    word(bytes, i.includes.size());
    for (const auto& include : i.includes) {
        if (include.logical.empty() || include.parent.empty() || !digest(include.content_sha256)) return std::nullopt;
        text(bytes, include.parent); text(bytes, include.logical); text(bytes, include.content_sha256);
    }
    tokens(bytes, i.macros); tokens(bytes, i.compiler_options);
    word(bytes, i.stages.size());
    for (const auto& stage : i.stages) {
        if (stage.stage.empty() || stage.entry.empty() || stage.profile.empty()) return std::nullopt;
        text(bytes, stage.stage); text(bytes, stage.entry); text(bytes, stage.profile); tokens(bytes, stage.options);
        word(bytes, stage.input_sha256.has_value() ? 1 : 0);
        if (stage.input_sha256) {
            if (!digest(*stage.input_sha256)) return std::nullopt;
            text(bytes, *stage.input_sha256);
        }
    }
    text(bytes, i.package_sha256); word(bytes, i.descriptor_version);
    text(bytes, i.options_manifest_sha256); word(bytes, i.result_schema_version); word(bytes, i.options_manifest_version);
    for (const auto& value : required) text(bytes, value);
    word(bytes, i.capabilities.size());
    for (const auto token : i.capabilities) word(bytes, token, 2);
    word(bytes, i.variants.size());
    for (const auto token : i.variants) word(bytes, token, 2);
    return Key{core::sha256_hex(bytes)};
}

Cache::Cache(fs::path user_directory, const Limits limits) : directory_(std::move(user_directory)), limits_(limits) {}

Result Cache::lookup(const Key& key, const bool translation_allowed, const Validate& validate) const {
    if (!translation_allowed) return {Status::opted_out, {}};
    if (!digest(key.sha256)) return {Status::invalid_key, {}};
    if (!directory_.is_absolute()) return {Status::io_error, {}};
    if (!validate) return {Status::invalid_payload, {}};
    if (!directory(directory_, false)) {
        std::error_code error;
        const auto status = fs::symlink_status(directory_, error);
        return {status.type() == fs::file_type::not_found ? Status::miss : Status::io_error, {}};
    }
    auto result = read(filename(directory_, key), key, limits_, validate);
    if (result.status == Status::hit) {
        std::error_code error;
        fs::last_write_time(filename(directory_, key), fs::file_time_type::clock::now(), error);
    }
    return result;
}

Status Cache::store(const Key& key, const bool translation_allowed, const std::span<const std::uint8_t> payload,
    const Validate& validate) const {
    if (!translation_allowed) return Status::opted_out;
    if (!digest(key.sha256)) return Status::invalid_key;
    if (!directory_.is_absolute()) return Status::io_error;
    if (limits_.entry_bytes < header_size || payload.size() > limits_.entry_bytes - header_size
        || limits_.total_bytes < header_size || payload.size() > limits_.total_bytes - header_size
        || limits_.entries == 0 || limits_.scan_entries == 0
        || payload.size() > static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max()) - header_size) return Status::budget;
    if (!valid(validate, payload)) return Status::invalid_payload;
    std::lock_guard guard(writer_mutex);
    if (!directory(directory_, true)) return Status::io_error;
    Lease lease(directory_);
    if (!lease.held) return lease.invalid ? Status::io_error : Status::busy;
    const auto destination = filename(directory_, key);
    // The new payload was decoded before taking the writer mutex. Do not invoke
    // client callbacks under it; the existing envelope can be reused if equal.
    const auto old = read(destination, key, limits_, [](const auto) { return true; });
    if (old.status == Status::hit && old.payload.size() == payload.size()
        && std::equal(old.payload.begin(), old.payload.end(), payload.begin())) return Status::stored;
    std::error_code error;
    const auto existing = fs::symlink_status(destination, error);
    if (existing.type() != fs::file_type::not_found) {
        if (error || !fs::is_regular_file(existing) || fs::is_symlink(existing)) return Status::io_error;
        fs::remove(destination, error);
        if (error) return Status::io_error;
    }
    const auto admitted = reserve(directory_, limits_, header_size + payload.size());
    if (admitted != Status::stored) return admitted;
    Bytes bytes(magic.begin(), magic.end());
    word(bytes, 1, 4); word(bytes, payload.size());
    bytes.insert(bytes.end(), key.sha256.begin(), key.sha256.end());
    const auto hash = core::sha256(payload);
    bytes.insert(bytes.end(), hash.begin(), hash.end());
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    const auto temporary = directory_ / ".pending";
    // Never follow a planted temporary symlink. Only this lease owns the name.
    const auto pending_status = fs::symlink_status(temporary, error);
    if (pending_status.type() != fs::file_type::not_found) {
        if (error || !fs::is_regular_file(pending_status) || fs::is_symlink(pending_status)) return Status::io_error;
        fs::remove(temporary, error);
        if (error) return Status::io_error;
    }
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) return Status::io_error;
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        output.flush();
        output.close();
        if (!output) { fs::remove(temporary, error); return Status::io_error; }
    }
    fs::rename(temporary, destination, error);
    if (error) { fs::remove(temporary, error); return Status::io_error; }
    return Status::stored;
}

Status Cache::invalidate(const Key& key, const bool translation_allowed) const {
    if (!translation_allowed) return Status::opted_out;
    if (!digest(key.sha256)) return Status::invalid_key;
    if (!directory_.is_absolute()) return Status::io_error;
    if (!directory(directory_, false)) {
        std::error_code error;
        const auto status = fs::symlink_status(directory_, error);
        return status.type() == fs::file_type::not_found ? Status::miss : Status::io_error;
    }
    std::lock_guard guard(writer_mutex);
    Lease lease(directory_);
    if (!lease.held) return lease.invalid ? Status::io_error : Status::busy;
    std::error_code error;
    const auto path = filename(directory_, key);
    const auto status = fs::symlink_status(path, error);
    if (status.type() == fs::file_type::not_found) return Status::miss;
    if (error || !fs::is_regular_file(status) || fs::is_symlink(status)) return Status::io_error;
    return fs::remove(path, error) && !error ? Status::invalidated : Status::io_error;
}
} // namespace eawr::presentation::shader_cache
