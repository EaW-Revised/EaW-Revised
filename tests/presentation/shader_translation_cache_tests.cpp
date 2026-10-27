#include "eawr/presentation/shader_translation_cache.hpp"
#include "../support/check.hpp"

#include <algorithm>
#include <array>
#include <barrier>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <system_error>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winioctl.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {
namespace cache = eawr::presentation::shader_cache;
namespace fs = std::filesystem;
using Bytes = std::vector<std::uint8_t>;
eawr::testing::Checker checks;
void check(const bool condition, const std::string_view message) { checks.check(condition, message, std::cerr); }

cache::Identity identity() {
    cache::Identity i;
    i.logical = "data/art/shaders/invented.fxo";
    i.root_sha256 = std::string(64, 'a'); i.package_sha256 = std::string(64, 'b');
    i.compiler_abi = "owned-c-abi-1"; i.capability_manifest = std::string(64, 'e');
    i.options_manifest_sha256 = std::string(64, 'f');
    i.selection_policy = "selection-1"; i.resolver_policy = "vfs-1";
    i.translator = "translator-1"; i.emitter = "emitter-1";
    i.binding_policy = "bindings-1"; i.state_policy = "states-1";
    i.technique_policy = "techniques-1"; i.admission_policy = "admission-1";
    i.godot_engine = "engine-content-identity"; i.shader_abi = "shader-abi-1";
    i.language = "spatial"; i.renderer = "forward_plus"; i.backend = "vulkan";
    i.platform = "invented-platform"; i.architecture = "invented-architecture";
    i.includes = {{"root", "common.fxh", std::string(64, 'c')}, {"common.fxh", "leaf.fxh", std::string(64, 'd')}};
    i.macros = {{"A", "1"}, {"A", "2"}};
    i.compiler_options = {{"optimization", "bounded"}, {"matrix", "column"}};
    i.stages = {{"vertex", "invented_vertex", "vs_1_1", {{"strict", "yes"}}, std::string(64, 'a')},
        {"pixel", "invented_pixel", "ps_2_0", {}, std::string(64, 'c')}};
    i.capabilities = {1, 3}; i.variants = {1, 10, 20, 30, 40};
    return i;
}
cache::Key key(const cache::Identity& i) {
    const auto result = cache::make_key(i);
    eawr::testing::require(result.has_value(), "invented cache key");
    return *result;
}
struct Directory final {
    fs::path path;
    Directory() {
        const auto base = fs::temp_directory_path();
        for (unsigned n = 0; n < 100; ++n) {
            path = base / ("eawr-shader-cache-tests-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(n));
            if (fs::create_directory(path)) return;
        }
        throw std::runtime_error("cannot create private test directory");
    }
    ~Directory() { std::error_code error; fs::remove_all(path, error); }
};
void write(const fs::path& path, const Bytes& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    eawr::testing::require(static_cast<bool>(out), "write invented record");
}
Bytes read(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return Bytes(std::istreambuf_iterator<char>(in), {});
}
fs::path file(const fs::path& root, const cache::Key& k) { return root / (k.sha256 + ".eawrshader"); }
const Bytes program{0x01, 0x01, 0xfe, 0xff, 0xff, 0xff, 0, 0};
bool p1(const std::span<const std::uint8_t> bytes) {
    return eawr::assets::read_shader_program(std::as_bytes(bytes)).has_value();
}

void key_contracts() {
    const auto initial = identity();
    const auto baseline = key(initial).sha256;
    check(baseline == key(initial).sha256, "same identity is stable");
    const std::vector<std::function<void(cache::Identity&)>> changes{
        [](auto& i) { i.logical += "x"; }, [](auto& i) { i.root_sha256[0] = 'b'; },
        [](auto& i) { i.form = cache::Form::source; }, [](auto& i) { i.includes[0].parent += "x"; },
        [](auto& i) { i.includes[0].logical += "x"; }, [](auto& i) { i.includes[0].content_sha256[0] = 'e'; },
        [](auto& i) { std::reverse(i.includes.begin(), i.includes.end()); },
        [](auto& i) { std::reverse(i.macros.begin(), i.macros.end()); },
        [](auto& i) { std::reverse(i.compiler_options.begin(), i.compiler_options.end()); },
        [](auto& i) { i.stages[0].stage = "pixel"; }, [](auto& i) { i.stages[0].entry += "x"; },
        [](auto& i) { i.stages[0].profile += "x"; }, [](auto& i) { i.stages[0].options[0].value += "x"; },
        [](auto& i) { std::reverse(i.stages.begin(), i.stages.end()); },
        [](auto& i) { i.package_sha256[0] = 'e'; }, [](auto& i) { i.compiler_abi += "x"; },
        [](auto& i) { i.capability_manifest[0] = 'f'; }, [](auto& i) { ++i.descriptor_version; },
        [](auto& i) { i.options_manifest_sha256[0] = 'a'; }, [](auto& i) { ++i.result_schema_version; },
        [](auto& i) { ++i.options_manifest_version; }, [](auto& i) { (*i.stages[0].input_sha256)[0] = 'b'; },
        [](auto& i) { i.selection_policy += "x"; }, [](auto& i) { i.resolver_policy += "x"; },
        [](auto& i) { i.translator += "x"; }, [](auto& i) { i.emitter += "x"; },
        [](auto& i) { i.binding_policy += "x"; }, [](auto& i) { i.state_policy += "x"; },
        [](auto& i) { i.technique_policy += "x"; }, [](auto& i) { i.admission_policy += "x"; },
        [](auto& i) { i.godot_engine += "x"; }, [](auto& i) { i.shader_abi += "x"; },
        [](auto& i) { i.language += "x"; }, [](auto& i) { i.renderer += "x"; },
        [](auto& i) { i.backend += "x"; }, [](auto& i) { i.platform += "x"; },
        [](auto& i) { i.architecture += "x"; }, [](auto& i) { ++i.capabilities[0]; },
        [](auto& i) { std::reverse(i.variants.begin(), i.variants.end()); },
    };
    for (const auto& change : changes) {
        auto changed = initial; change(changed);
        check(key(changed).sha256 != baseline, "every generation input changes the key");
    }
    auto left = initial, right = initial;
    left.macros = {{"ab", "c"}}; right.macros = {{"a", "bc"}};
    check(key(left).sha256 != key(right).sha256, "key framing prevents concatenation ambiguity");
    right.package_sha256 = "version-string-only";
    check(!cache::make_key(right), "version string is not package identity");
    right = initial; right.includes[0].content_sha256 = "not-a-digest";
    check(!cache::make_key(right), "unverified include digest refused");
    right = initial; right.shader_abi.clear();
    check(!cache::make_key(right), "incomplete identity refused");
}

void storage_contracts() {
    Directory temporary;
    const auto root = temporary.path / "player-cache";
    const cache::Limits limits{8192, 1024, 8, 32};
    cache::Cache disk(root, limits);
    const auto k = key(identity());
    unsigned callbacks{};
    const auto validate = [&callbacks](const auto bytes) { ++callbacks; return p1(bytes); };
    check(disk.lookup(k, false, validate).status == cache::Status::opted_out, "cold opt-out precedes read");
    check(disk.store(k, false, program, validate) == cache::Status::opted_out, "cold opt-out precedes write");
    check(disk.invalidate(k, false) == cache::Status::opted_out, "cold opt-out precedes invalidation");
    check(callbacks == 0 && !fs::exists(root), "opt-out has no I/O or decoder call");
    check(disk.lookup(k, true, validate).status == cache::Status::miss, "cold lookup misses");
    check(disk.store(k, true, program, validate) == cache::Status::stored, "valid P1 stage stored");
    auto hit = disk.lookup(k, true, validate);
    check(hit.status == cache::Status::hit && hit.payload == program, "warm record validated and owned");
    const auto descriptor = eawr::assets::read_shader_program(std::as_bytes(std::span(hit.payload)));
    check(descriptor && descriptor.value().bytes.data() == reinterpret_cast<const std::byte*>(hit.payload.data()),
        "P1 descriptor borrows activation-owned cache payload");
    const auto before = callbacks;
    check(disk.lookup(k, false, validate).status == cache::Status::opted_out && callbacks == before,
        "warm opt-out bypasses payload and decoder");
    const auto warm_bytes = read(file(root, k));
    const auto warm_time = fs::last_write_time(file(root, k));
    check(disk.invalidate(k, false) == cache::Status::opted_out, "warm opt-out bypasses invalidation");
    check(read(file(root, k)) == warm_bytes && fs::last_write_time(file(root, k)) == warm_time,
        "opt-out invalidation leaves the warm record untouched");
    check(disk.lookup(k, true, {}).status == cache::Status::invalid_payload, "decoder mandatory on lookup");
    check(disk.store(k, true, program, {}) == cache::Status::invalid_payload, "decoder mandatory on store");
    check(disk.store(k, true, Bytes{1, 2, 3}, validate) == cache::Status::invalid_payload, "invalid P1 stage not stored");
    check(disk.lookup({"../escape"}, true, validate).status == cache::Status::invalid_key, "path key rejected");
    const auto original = read(file(root, k));
    for (const std::size_t offset : {std::size_t{0}, std::size_t{8}, std::size_t{12}, std::size_t{20}, std::size_t{84}, original.size() - 1}) {
        auto damaged = original; damaged[offset] ^= 1; write(file(root, k), damaged);
        const auto count = callbacks;
        check(disk.lookup(k, true, validate).status == cache::Status::corrupt, "bad magic/version/length/key/hash/payload misses");
        check(callbacks == count, "envelope checks precede decoder");
        check(disk.store(k, true, program, validate) == cache::Status::stored, "corruption regenerates on activation");
    }
    write(file(root, k), Bytes(original.begin(), original.end() - 1));
    check(disk.lookup(k, true, validate).status == cache::Status::corrupt, "truncation misses");
    check(disk.store(k, true, program, validate) == cache::Status::stored, "truncation recovers");
    check(disk.lookup(k, true, [](const auto) { return false; }).status == cache::Status::corrupt, "payload decode refusal misses");
    check(disk.lookup(k, true, [](const auto) -> bool { throw std::runtime_error("owned decoder refusal"); }).status == cache::Status::corrupt,
        "decoder failure preserves uncached path");
    check(disk.invalidate(k, true) == cache::Status::invalidated, "explicit invalidation removes disk entry");
    check(hit.payload == program && descriptor && descriptor.value().bytes.size() == program.size(), "in-flight bytes and P1 spans survive invalidation");
    check(disk.lookup(k, true, validate).status == cache::Status::miss, "invalidated entry misses");
    check(!fs::exists(root / ".pending"), "publication retains no temporary record");
    fs::create_directory(root / ".pending");
    check(disk.store(k, true, program, validate) == cache::Status::io_error, "blocked publication remains an optional cache failure");
    check(disk.lookup(k, true, validate).status == cache::Status::miss, "failed publication never admits temporary data");
    fs::remove(root / ".pending");
    check(disk.store(k, true, program, validate) == cache::Status::stored, "subsequent activation recovers failed publication");
}

void budgets_and_eviction() {
    Directory temporary;
    const auto root = temporary.path / "cache";
    cache::Cache disk(root, {248, 124, 2, 16}); // 116-byte envelope + 8-byte invented program.
    auto first = identity(), second = identity(), third = identity();
    second.logical = "second.fxo"; third.logical = "third.fxo";
    const auto a = key(first), b = key(second), c = key(third);
    check(disk.store(a, true, program, p1) == cache::Status::stored, "first bounded entry stored");
    check(disk.store(b, true, program, p1) == cache::Status::stored, "second bounded entry stored");
    auto held = disk.lookup(a, true, p1);
    const auto now = fs::file_time_type::clock::now();
    fs::last_write_time(file(root, a), now - std::chrono::hours(2));
    fs::last_write_time(file(root, b), now - std::chrono::hours(1));
    check(disk.store(c, true, program, p1) == cache::Status::stored, "bounded eviction permits next entry");
    check(disk.lookup(a, true, p1).status == cache::Status::miss, "least recent entry evicted");
    check(disk.lookup(b, true, p1).status == cache::Status::hit, "newer entry retained");
    check(held.payload == program, "eviction cannot invalidate active generation");
    fs::last_write_time(file(root, b), now); fs::last_write_time(file(root, c), now);
    check(disk.store(a, true, program, p1) == cache::Status::stored, "tied eviction stores replacement");
    const auto victim = b.sha256 < c.sha256 ? b : c;
    check(disk.lookup(victim, true, p1).status == cache::Status::miss, "recency ties use digest ordering");
    cache::Cache tiny(root, {248, 123, 2, 16});
    unsigned decoded{};
    check(tiny.store(a, true, program, [&decoded](const auto) { ++decoded; return true; }) == cache::Status::budget && decoded == 0,
        "entry budget includes header and precedes decoder work");
    check(tiny.lookup(a, true, [&decoded](const auto) { ++decoded; return true; }).status == cache::Status::budget && decoded == 0,
        "oversized disk record refused before allocation/decoder");
    cache::Cache bounded(root, {248, 124, 2, 1});
    check(bounded.store(victim, true, program, p1) == cache::Status::budget, "directory scan work bounded");
    fs::remove(root / ".writer");
    fs::create_directory(root / ".writer");
    check(disk.store(victim, true, program, p1) == cache::Status::io_error, "non-file writer lease is refused");
    check(disk.lookup(a, true, p1).status == cache::Status::hit, "writer lease does not block verified readers");
    fs::remove(root / ".writer");
    check(disk.store(victim, true, program, p1) == cache::Status::stored, "writer lease recovers without persistent directory state");
    write(temporary.path / "file-instead-of-directory", program);
    cache::Cache denied(temporary.path / "file-instead-of-directory", {248, 124, 2, 16});
    check(denied.store(a, true, program, p1) == cache::Status::io_error, "unwritable cache leaves activation usable");
    check(denied.lookup(a, true, p1).status == cache::Status::io_error, "unreadable directory diagnosed distinctly");
    cache::Cache relative("invented-relative-cache", {248, 124, 2, 16});
    check(relative.store(a, true, program, p1) == cache::Status::io_error, "relative root cannot write into a game working directory");
}

// Independent native holder: this does not use the cache's process mutex.
struct WriterHandle final {
#ifdef _WIN32
    HANDLE handle{INVALID_HANDLE_VALUE};
    explicit WriterHandle(const fs::path& path) {
        handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        eawr::testing::require(handle != INVALID_HANDLE_VALUE, "independent writer handle");
    }
    ~WriterHandle() { CloseHandle(handle); }
#else
    int handle{-1};
    explicit WriterHandle(const fs::path& path) {
        handle = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, S_IRUSR | S_IWUSR);
        eawr::testing::require(handle >= 0, "independent writer handle");
        if (::flock(handle, LOCK_EX | LOCK_NB) != 0) {
            ::close(handle); handle = -1;
            throw std::runtime_error("independent writer lock");
        }
    }
    ~WriterHandle() { ::close(handle); }
#endif
    WriterHandle(const WriterHandle&) = delete;
    WriterHandle& operator=(const WriterHandle&) = delete;
};

void concurrent_writers() {
    Directory temporary;
    const auto root = temporary.path / "cache";
    const cache::Limits limits{8192, 1024, 8, 32};
    cache::Cache first(root, limits), second(root, limits);
    auto changed = identity(); changed.logical = "second.fxo";
    const auto a = key(identity()), b = key(changed);
    check(first.store(a, true, program, p1) == cache::Status::stored, "seed before independent lease");
    {
        WriterHandle holder(root / ".writer");
        check(second.store(b, true, program, p1) == cache::Status::busy, "second native holder cannot acquire lease");
        check(second.invalidate(a, true) == cache::Status::busy, "invalidation observes independent native lease");
        const auto hit = second.lookup(a, true, p1);
        check(hit.status == cache::Status::hit && hit.payload == program, "complete reader survives live writer lease");
    }
    check(second.store(b, true, program, p1) == cache::Status::stored, "native lease release restores writer");
    check(first.invalidate(a, true) == cache::Status::invalidated, "clear first threaded key");
    check(second.invalidate(b, true) == cache::Status::invalidated, "clear second threaded key");
    std::array<cache::Status, 64> left{}, right{};
    std::barrier start(2);
    const auto run = [&](const cache::Cache& disk, const cache::Key& k, auto& outcomes) {
        start.arrive_and_wait();
        for (auto& result : outcomes) {
            result = disk.store(k, true, program, p1);
            std::this_thread::yield();
        }
    };
    std::thread one([&] { run(first, a, left); });
    std::thread two([&] { run(second, b, right); });
    one.join(); two.join();
    for (const auto& outcomes : {left, right}) {
        check(std::all_of(outcomes.begin(), outcomes.end(), [](const auto value) {
            return value == cache::Status::stored || value == cache::Status::busy;
        }), "concurrent writes only publish or report lease contention");
        check(std::find(outcomes.begin(), outcomes.end(), cache::Status::stored) != outcomes.end(),
            "each concurrent writer publishes its own key");
    }
    for (const auto& k : {a, b}) {
        const auto hit = first.lookup(k, true, p1);
        check(hit.status == cache::Status::hit && hit.payload == program, "each concurrent record validates after join");
    }
    check(!fs::exists(root / ".pending"), "concurrent writers leave no partial publication");
}

#ifdef _WIN32
// Same native fixture pattern as the runtime contracts. A mount-point junction
// does not require the symbolic-link privilege of a Windows test account.
bool make_junction(const fs::path& path, const fs::path& target, std::error_code& error) {
    if (!CreateDirectoryW(path.c_str(), nullptr)) {
        error = std::error_code(static_cast<int>(GetLastError()), std::system_category());
        return false;
    }
    const auto handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        error = std::error_code(static_cast<int>(GetLastError()), std::system_category());
        RemoveDirectoryW(path.c_str());
        return false;
    }
    const auto substitute = L"\\??\\" + fs::absolute(target).wstring();
    const auto print = fs::absolute(target).wstring();
    struct Header {
        std::uint32_t tag;
        std::uint16_t length, reserved, substitute_offset, substitute_length, print_offset, print_length;
    };
    static_assert(sizeof(Header) == 16);
    const auto path_bytes = (substitute.size() + print.size() + 2) * sizeof(wchar_t);
    if (path_bytes > 16000) {
        CloseHandle(handle); RemoveDirectoryW(path.c_str());
        error = std::make_error_code(std::errc::filename_too_long);
        return false;
    }
    const Header header{IO_REPARSE_TAG_MOUNT_POINT, static_cast<std::uint16_t>(path_bytes + 8), 0, 0,
        static_cast<std::uint16_t>(substitute.size() * sizeof(wchar_t)),
        static_cast<std::uint16_t>((substitute.size() + 1) * sizeof(wchar_t)),
        static_cast<std::uint16_t>(print.size() * sizeof(wchar_t))};
    std::vector<std::byte> buffer(sizeof(header) + path_bytes);
    std::memcpy(buffer.data(), &header, sizeof(header));
    std::memcpy(buffer.data() + sizeof(header), substitute.data(), substitute.size() * sizeof(wchar_t));
    std::memcpy(buffer.data() + sizeof(header) + header.print_offset, print.data(), print.size() * sizeof(wchar_t));
    DWORD returned{};
    const auto result = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, buffer.data(), static_cast<DWORD>(buffer.size()),
        nullptr, 0, &returned, nullptr);
    if (!result) error = std::error_code(static_cast<int>(GetLastError()), std::system_category());
    CloseHandle(handle);
    if (!result) RemoveDirectoryW(path.c_str());
    return result != FALSE;
}
#endif

bool is_link(const fs::path& path) {
#ifdef _WIN32
    const auto attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    return fs::is_symlink(fs::symlink_status(path));
#endif
}
void remove_link(const fs::path& path) {
#ifdef _WIN32
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 &&
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        eawr::testing::require(RemoveDirectoryW(path.c_str()) != FALSE, "remove private directory link");
        return;
    }
#endif
    eawr::testing::require(fs::remove(path), "remove private file link");
}

bool link(const fs::path& target, const fs::path& path, const bool is_directory, const std::string_view guard) {
    std::error_code error;
    if (is_directory) fs::create_directory_symlink(target, path, error);
    else fs::create_symlink(target, path, error);
    if (!error) {
        std::cout << "symlink guard " << guard << ": link created\n";
        return true;
    }
#ifdef _WIN32
    const auto symlink_error = error.message();
    // File positions use a junction to the sentinel's private directory. They
    // must refuse any reparse object there without traversing or removing it.
    if (make_junction(path, is_directory ? target : target.parent_path(), error)) {
        std::cout << "junction guard " << guard << ": created after symlink refusal: " << symlink_error << '\n';
        return true;
    }
#endif
    std::cout << "SKIP link guard " << guard << ": host refused link creation: " << error.message() << '\n';
    return false;
}

void symlink_refusals() {
    Directory temporary;
    const cache::Limits limits{8192, 1024, 8, 32};
    const auto k = key(identity());
    const auto target_directory = temporary.path / "target-directory";
    fs::create_directory(target_directory);
    const auto target = target_directory / "sentinel";
    write(target, program);
    const auto original = read(target);
    const auto original_time = fs::last_write_time(target);
    const auto untouched = [&] {
        return read(target) == original && fs::last_write_time(target) == original_time;
    };
    const auto root_link = temporary.path / "root-link";
    if (link(target_directory, root_link, true, "root")) {
        cache::Cache disk(root_link, limits);
        check(disk.lookup(k, true, p1).status == cache::Status::io_error, "root link lookup refused");
        check(disk.store(k, true, program, p1) == cache::Status::io_error, "root link store refused");
        check(disk.invalidate(k, true) == cache::Status::io_error, "root link invalidation refused");
        check(untouched() && !fs::exists(target_directory / ".writer") && !fs::exists(file(target_directory, k)),
            "root link target untouched");
        remove_link(root_link);
    }
    const auto root = temporary.path / "cache";
    fs::create_directory(root);
    cache::Cache disk(root, limits);
    if (link(target, file(root, k), false, "entry")) {
        check(disk.lookup(k, true, p1).status == cache::Status::io_error, "entry link lookup refused");
        check(disk.store(k, true, program, p1) == cache::Status::io_error, "entry link store refused");
        check(disk.invalidate(k, true) == cache::Status::io_error, "entry link invalidation refused");
        check(untouched() && is_link(file(root, k)), "entry link target untouched");
        remove_link(file(root, k));
    }
    if (link(target, root / ".pending", false, "pending")) {
        check(disk.store(k, true, program, p1) == cache::Status::io_error, "pending link publication refused");
        check(untouched() && is_link(root / ".pending"), "pending link target untouched");
        check(disk.lookup(k, true, p1).status == cache::Status::miss, "pending link never admitted");
        remove_link(root / ".pending");
    }
    check(disk.store(k, true, program, p1) == cache::Status::stored, "seed before writer link");
    fs::remove(root / ".writer");
    if (link(target, root / ".writer", false, "writer")) {
        check(disk.store(k, true, program, p1) == cache::Status::io_error, "writer link store refused");
        check(disk.invalidate(k, true) == cache::Status::io_error, "writer link invalidation refused");
        check(disk.lookup(k, true, p1).status == cache::Status::hit, "writer link does not hide complete records");
        check(untouched() && is_link(root / ".writer"), "writer link target untouched");
        remove_link(root / ".writer");
    }
}
} // namespace

int main() {
    try { key_contracts(); storage_contracts(); budgets_and_eviction(); concurrent_writers(); symlink_refusals(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return checks.finish();
}
