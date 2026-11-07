#include "eawr/mods/file_index.hpp"
#include "eawr/core/sha256.hpp"
#include "eawr/mods/steam/text_formats.hpp"
#include "eawr/vfs/vfs.hpp"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string_view>
#include <sstream>
#include <stdexcept>

namespace {
namespace fs = std::filesystem;
using namespace eawr::mods;
#define REQUIRE(value) do { if (!(value)) throw std::runtime_error("file index line " + std::to_string(__LINE__) + ": " #value); } while (false)

struct Fixture {
    fs::path root = fs::temp_directory_path() / ("eawr-file-index-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Fixture() { fs::create_directories(root); }
    ~Fixture() {
        std::error_code ec;
        for (fs::recursive_directory_iterator it(root, ec), end; it != end && !ec; it.increment(ec))
            fs::permissions(it->path(), fs::perms::owner_all, fs::perm_options::add, ec);
        fs::remove_all(root, ec);
    }
    void write(const fs::path& name, const std::string& bytes) const {
        fs::create_directories((root / name).parent_path());
        std::ofstream(root / name, std::ios::binary) << bytes;
    }
};

void integer(std::string& bytes, std::uint32_t value, unsigned count) {
    for (unsigned i = 0; i < count; ++i) bytes.push_back(static_cast<char>((value >> (8 * i)) & 255u));
}
std::string archive_header(const std::string& path, const std::uint32_t size) {
    std::string bytes;
    integer(bytes, 1, 4); integer(bytes, 1, 4);
    integer(bytes, static_cast<std::uint32_t>(path.size()), 2); bytes += path;
    integer(bytes, 0x12345678u, 4); integer(bytes, 0, 4);
    integer(bytes, size, 4);
    integer(bytes, static_cast<std::uint32_t>(8 + 2 + path.size() + 20), 4);
    integer(bytes, 0, 4);
    return bytes;
}
std::string archive(const std::string& path, const std::string& payload) {
    return archive_header(path, static_cast<std::uint32_t>(payload.size())) + payload;
}
const FileRow& row(const FileIndex& index, const std::string_view path) {
    const auto found = std::find_if(index.rows.begin(), index.rows.end(), [&](const auto& value) { return value.path == path; });
    REQUIRE(found != index.rows.end());
    return *found;
}

// Build large fixtures a small piece at a time, so neither fixture construction
// nor the VFS chunk seam needs storage proportional to the candidate size.
void streaming_contract() {
    Fixture fixture;
    constexpr std::uint32_t size = 8 * 1024 * 1024 + 123;
    constexpr std::size_t chunk_size = 16381; // Deliberately unaligned to SHA blocks.
    const std::string piece(chunk_size, 'a');
    fs::create_directories(fixture.root / "leaf/art");
    fs::create_directories(fixture.root / "parent");
    const auto write_large = [&](const fs::path& path, const std::string& header) {
        std::ofstream output(path, std::ios::binary);
        output << header;
        for (std::uint32_t at = 0; at < size;) {
            const auto count = std::min<std::size_t>(piece.size(), size - at);
            output.write(piece.data(), static_cast<std::streamsize>(count));
            at += static_cast<std::uint32_t>(count);
        }
        REQUIRE(output.good());
    };
    write_large(fixture.root / "leaf/art/large.bin", "");
    write_large(fixture.root / "large.meg", archive_header("Data/Art/large.bin", size));
    fixture.write("leaf/empty.bin", "");
    fixture.write("parent/empty.bin", "");
    const std::vector<eawr::vfs::MountSpec> mounts{
        {"leaf", fixture.root / "leaf", "data", {}},
        {"parent", fixture.root / "parent", "data", {{fixture.root / "large.meg", "large.meg", ""}}}};
    auto filesystem = eawr::vfs::Vfs::mount(mounts);
    REQUIRE(filesystem);
    const std::vector<std::string> order{"leaf", "parent"};
    const auto built = build_file_index(filesystem.value(), order, "streamed");
    REQUIRE(built);
    const auto& large = row(built.value(), "data/art/large.bin");
    // Independent Python hashlib SHA-256 of exactly size repetitions of ASCII a.
    constexpr std::string_view expected = "2d0a98bdb6952e9a9584d64b61e45569c96af91dd8512bc87d4f5959ac0eed8c";
    REQUIRE(large.candidates.size() == 2 && large.identical());
    REQUIRE(large.candidates[0].origin == "loose" && large.candidates[1].origin == "archive");
    REQUIRE(*large.candidates[0].sha256 == expected && *large.candidates[1].sha256 == expected);
    REQUIRE(built.value().stats.candidates_hashed == 4 && built.value().stats.bytes_hashed == 2ULL * size);
    auto candidates = filesystem.value().candidates("data/art/large.bin");
    REQUIRE(candidates);
    std::vector<std::byte> buffer(chunk_size);
    for (const auto& candidate : candidates.value()) {
        const auto whole = filesystem.value().open(candidate);
        REQUIRE(whole);
        const auto old_hash = eawr::core::sha256_hex(std::span(
            reinterpret_cast<const std::uint8_t*>(whole.value().data()), whole.value().size()));
        std::uint64_t total = 0;
        std::size_t callbacks = 0;
        eawr::core::Sha256Hasher hash;
        const auto read = filesystem.value().read_chunks(candidate, buffer, [&](std::span<const std::byte> bytes) {
            REQUIRE(!bytes.empty() && bytes.size() <= buffer.size() && bytes.data() == buffer.data());
            total += bytes.size();
            ++callbacks;
            hash.update(std::span(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()));
        });
        REQUIRE(read && total == size && callbacks == (size + chunk_size - 1) / chunk_size);
        REQUIRE(hash.hex_digest() == expected && hash.hex_digest() == old_hash);
    }
    const auto& empty_row = row(built.value(), "data/empty.bin");
    REQUIRE(empty_row.candidates.size() == 2 && empty_row.identical());
    REQUIRE(*empty_row.candidates[0].sha256 == eawr::core::sha256_hex({}));
    bool called = false;
    const auto consume = [&](std::span<const std::byte>) { called = true; };
    const auto empty = filesystem.value().stat("data/empty.bin");
    REQUIRE(empty && filesystem.value().read_chunks(empty.value(), buffer, consume) && !called);
    REQUIRE(!filesystem.value().read_chunks(candidates.value()[0], {}, consume));
    REQUIRE(!filesystem.value().read_chunks(candidates.value()[0], buffer, {}));
    auto forged = candidates.value()[0];
    ++forged.size;
    REQUIRE(!filesystem.value().read_chunks(forged, buffer, consume) && !called);
    // A source truncated after mounting must fail, including after partial delivery.
    fs::resize_file(fixture.root / "large.meg", size / 2);
    REQUIRE(!filesystem.value().read_chunks(candidates.value()[1], buffer, consume) && called);
    REQUIRE(!build_file_index(filesystem.value(), order, "truncated"));
}

void model_contract() {
    Fixture fixture;
    fixture.write("leaf/xml/same.xml", "same");
    fixture.write("parent/xml/same.xml", "same");
    fixture.write("leaf/xml/changed.xml", "leaf");
    fixture.write("parent/xml/changed.xml", "parent");
    fixture.write("parent/xml/from_archive.xml", "lower loose");
    fixture.write("leaf/art/large_only.bin", std::string(1024 * 1024, 'u'));
    fixture.write("leaf.meg", archive("Data/XML/from_archive.xml", "higher archive"));
    fixture.write("shadow.meg", archive("Data/XML/changed.xml", "same-layer archive"));
    for (const auto& entry : fs::recursive_directory_iterator(fixture.root)) {
        if (entry.is_regular_file()) fs::permissions(entry.path(), fs::perms::owner_read | fs::perms::group_read | fs::perms::others_read);
    }
    const std::vector<eawr::vfs::MountSpec> mounts{
        {"z-leaf", fixture.root / "leaf", "data", {{fixture.root / "leaf.meg", "leaf.meg", ""}, {fixture.root / "shadow.meg", "shadow.meg", ""}}},
        {"a-parent", fixture.root / "parent", "data", {}}};
    auto filesystem = eawr::vfs::Vfs::mount(mounts);
    REQUIRE(filesystem);
    const std::vector<std::string> order{"z-leaf", "a-parent"};
    auto built = build_file_index(filesystem.value(), order, std::string(64, 'a'));
    REQUIRE(built);
    const auto& index = built.value();
    REQUIRE(index.stats.enumerations == 1);
    REQUIRE(index.stats.candidates_hashed == 7); // same:2, changed:3, archive:2; unique megabyte is not opened.
    REQUIRE(index.stats.bytes_hashed == 4 + 4 + 4 + 18 + 6 + 14 + 11);
    REQUIRE(index.layers.size() == 2 && index.layers.front().key == "z-leaf");
    REQUIRE(row(index, "data/xml/same.xml").identical());
    const auto changed = row(index, "data/xml/changed.xml");
    REQUIRE(changed.candidates.size() == 3);
    REQUIRE(changed.candidates[0].layer == "z-leaf" && changed.candidates[0].origin == "loose");
    REQUIRE(changed.candidates[1].layer == "z-leaf" && changed.candidates[1].origin == "archive");
    REQUIRE(changed.candidates[2].layer == "a-parent");
    REQUIRE(!changed.identical() && changed.candidates[0].sha256 != changed.candidates[1].sha256);
    REQUIRE(row(index, "data/xml/from_archive.xml").candidates[0].origin == "archive");
    REQUIRE(!row(index, "data/art/large_only.bin").candidates.front().sha256);
    REQUIRE(index.layers[0].files == 4 && index.layers[0].wins == 3 && index.layers[0].only == 1 && index.layers[0].loses == 0);
    REQUIRE(index.layers[1].files == 3 && index.layers[1].loses == 3 && index.layers[1].wins == 0);
    FileFilter filter;
    filter.layer = "a-parent"; filter.role = "loser"; filter.type = "XML"; filter.path = "CHANGED";
    REQUIRE(matches_file_filter(changed, filter));
    filter.role = "winner";
    REQUIRE(!matches_file_filter(changed, filter));
    filter = {}; filter.hide_identical = true;
    REQUIRE(!matches_file_filter(row(index, "data/xml/same.xml"), filter));
    filter = {}; filter.conflicts_only = true;
    REQUIRE(!matches_file_filter(row(index, "data/art/large_only.bin"), filter));
    const auto encoded = steam::parse_json(file_row_json(changed));
    REQUIRE(encoded && encoded->member("winner") && encoded->member("candidates")->array.size() == 3);
    std::stringstream cache(std::ios::in | std::ios::out | std::ios::binary);
    REQUIRE(write_file_index(cache, index));
    const auto bytes = cache.str();
    const auto loaded = read_file_index(cache, index.chain_fingerprint);
    REQUIRE(loaded && *loaded == index);
    // A writer can fail after emitting its header; callers must discard partial output.
    auto oversize = index;
    oversize.rows.front().candidates.front().source = std::string(1024 * 1024 + 1, 'x');
    std::ostringstream partial;
    REQUIRE(!write_file_index(partial, oversize));
    REQUIRE(!partial.str().empty());
    std::istringstream incomplete(partial.str());
    REQUIRE(!read_file_index(incomplete, index.chain_fingerprint));
    std::istringstream wrong_key(bytes);
    REQUIRE(!read_file_index(wrong_key, std::string(64, 'b')));
    auto wrong_version = bytes;
    wrong_version[11] = 2; // magic is eleven bytes, followed by the little-endian version.
    std::istringstream wrong(wrong_version);
    REQUIRE(!read_file_index(wrong, index.chain_fingerprint));
    const auto corrupt_integer = [&](std::size_t offset, std::uint64_t value) {
        auto corrupt = bytes;
        for (unsigned i = 0; i < 8; ++i) corrupt[offset + i] = static_cast<char>((value >> (8 * i)) & 255u);
        std::istringstream input(corrupt);
        REQUIRE(!read_file_index(input, index.chain_fingerprint));
    };
    corrupt_integer(19, 1024 * 1024 + 1); // Oversized fingerprint frame, before allocation.
    constexpr std::size_t layer_count_offset = 19 + 8 + 64;
    corrupt_integer(layer_count_offset, 0); // Existing layer frames cannot be row frames.
    corrupt_integer(layer_count_offset, UINT64_MAX); // EOF, without trusting the count for allocation.
    const auto row_count_offset = layer_count_offset + 8 + 8 + order[0].size() + 8 + order[1].size();
    corrupt_integer(row_count_offset, 0); // Existing rows become forbidden trailing bytes.
    corrupt_integer(row_count_offset, UINT64_MAX);
    corrupt_integer(row_count_offset + 8, 1024 * 1024 + 1); // Oversized row frame.
    for (const auto length : {std::size_t(0), std::size_t(18), bytes.size() - 1}) {
        std::istringstream truncated(bytes.substr(0, length));
        REQUIRE(!read_file_index(truncated, index.chain_fingerprint));
    }
    std::istringstream trailing(bytes + "x");
    REQUIRE(!read_file_index(trailing, index.chain_fingerprint));
    REQUIRE(fs::file_size(fixture.root / "leaf/art/large_only.bin") == 1024 * 1024);
    REQUIRE(fs::file_size(fixture.root / "parent/xml/same.xml") == 4);
    REQUIRE(std::distance(fs::recursive_directory_iterator(fixture.root), fs::recursive_directory_iterator{}) == 13);
}
}

int main() {
    try { model_contract(); streaming_contract(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return 0;
}
