#include "eawr/core/json_text.hpp"
#include "eawr/core/text.hpp"
#include "eawr/presentation/ui/font_extract.hpp"

#include "eawr/core/sha256.hpp"
#include "eawr/data/ui/sfnt.hpp"
#include "eawr/presentation/ui/fonts.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <fstream>
#include <map>
#include <random>
#include <system_error>
#include <utility>

namespace eawr::presentation::ui {
namespace {

namespace fs = std::filesystem;

constexpr std::size_t max_tables = 128U;
constexpr std::size_t max_face_bytes = 4U * 1024U * 1024U;
constexpr std::string_view manifest_name = "fonts.json";
constexpr std::string_view manifest_schema = "eawr-font-cache/1";

// SHA-256 of the executable -> label. Only these builds extract without the opt-in.
constexpr std::array<KnownFontBuild, 1> known_builds{{
    {"07daeeaec9d1e751383b1db3f5847b6c66f68785440a6c52c87b85f723d018a4",
     "FoC Steam x64 (corruption/StarWarsG.exe, 12,428,800 bytes, file version 1.0.0.0)"},
}};

// Face -> (bytes, SHA-256) as extracted from the pinned build.
constexpr std::array<PinnedFontFace, 4> pinned_faces{{
    {"EmpireAtWar-Bold", 65684U, "3df279cf0063098dcd54156878bc7dc0a3c1010a30c790ffc55695cd799c5aec"},
    {"EmpireAtWar-Light", 67148U, "6ea18fea12c6536f7ce151ea42bda0a03a03451919fa239af2bb8a31012ce0aa"},
    {"EmpireAtWar-Medium", 66580U, "24a5c0bad551c136f2d0ef045ae319104cee0bc9df411acb088ad3c25459ae3c"},
    {"EmpireAtWar-Stencil", 67632U, "3086766b3daee145a7d5c3f9f39ec9dbd49ac66bfe739201424af919553251fc"},
}};

const FontExtractPolicy default_policy{known_builds, pinned_faces};

struct Face final {
    std::string full_name;
    std::string family;
    std::string subfamily;
    std::string postscript_name;
    std::string data;
    std::string sha256;
};

struct Scan final {
    std::vector<Face> faces;
    std::size_t rejected{};
};

std::string lower(std::string_view text) {
    return core::to_lower(text);
}

std::uint32_t u16(const std::string_view bytes, const std::size_t at) {
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at])) << 8U)
           | static_cast<unsigned char>(bytes[at + 1U]);
}

std::uint32_t u32(const std::string_view bytes, const std::size_t at) {
    return (u16(bytes, at) << 16U) | u16(bytes, at + 2U);
}

std::string hash_hex(const std::string_view bytes) {
    return core::sha256_hex(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(bytes.data()),
                                                          bytes.size()));
}

bool read_file(const fs::path& path, std::string& out) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return false;
    stream.seekg(0, std::ios::end);
    const auto size = stream.tellg();
    if (size < 0) return false;
    stream.seekg(0, std::ios::beg);
    out.resize(static_cast<std::size_t>(size));
    if (size > 0) stream.read(out.data(), size);
    return static_cast<bool>(stream) || stream.gcount() == size;
}

// The end of the font whose table directory starts at `start`, or 0 when that is not a
// well-formed table directory (a cheap prefilter before full validation).
std::size_t directory_end(const std::string_view buffer, const std::size_t start) {
    if (start + 12U > buffer.size()) return 0U;
    const std::size_t count = u16(buffer, start + 4U);
    if (count < 1U || count > max_tables) return 0U;
    std::size_t selector = 0U;
    while ((std::size_t{2} << selector) <= count) ++selector;
    const std::size_t search_range = std::size_t{16} << selector;
    if (u16(buffer, start + 6U) != search_range || u16(buffer, start + 8U) != selector
        || u16(buffer, start + 10U) != count * 16U - search_range) {
        return 0U;
    }
    if (start + 12U + 16U * count > buffer.size()) return 0U;
    std::string previous;
    std::size_t end = 12U + 16U * count;
    for (std::size_t index = 0U; index < count; ++index) {
        const std::size_t at = start + 12U + 16U * index;
        std::string tag(buffer.substr(at, 4U));
        for (const char c : tag) {
            if (static_cast<unsigned char>(c) < 0x20U || static_cast<unsigned char>(c) > 0x7EU) return 0U;
        }
        if (!previous.empty() && previous >= tag) return 0U;
        previous = std::move(tag);
        end = std::max(end, static_cast<std::size_t>(u32(buffer, at + 8U)) + u32(buffer, at + 12U));
    }
    return end;
}

Scan scan_faces(const std::string_view buffer) {
    Scan scan;
    for (const std::string_view signature : {std::string_view("\x00\x01\x00\x00", 4U), std::string_view("true", 4U)}) {
        std::size_t position = buffer.find(signature);
        while (position != std::string_view::npos) {
            if (const std::size_t end = directory_end(buffer, position); end != 0U) {
                if (end > max_face_bytes || position + end > buffer.size()) {
                    ++scan.rejected;
                } else {
                    // sfnt tables are 4-byte aligned; the file carries zero padding.
                    std::string font(buffer.substr(position, end));
                    font.resize((font.size() + 3U) / 4U * 4U, '\0');
                    const auto names = data::ui::read_sfnt_face(std::span<const std::byte>(
                        reinterpret_cast<const std::byte*>(font.data()), font.size()));
                    if (names) {
                        Face face{names.value().full_name, names.value().family, names.value().subfamily,
                                  names.value().postscript_name, std::move(font), {}};
                        face.sha256 = hash_hex(face.data);
                        scan.faces.push_back(std::move(face));
                    } else {
                        ++scan.rejected;
                    }
                }
            }
            position = buffer.find(signature, position + 1U);
        }
    }
    return scan;
}

bool is_pe(const std::string_view data) {
    if (data.size() < 0x40U || data[0] != 'M' || data[1] != 'Z') return false;
    // The PE header offset is little-endian, unlike everything in a font.
    const std::size_t header = static_cast<unsigned char>(data[0x3C]) | (static_cast<std::size_t>(static_cast<unsigned char>(data[0x3D])) << 8U)
                               | (static_cast<std::size_t>(static_cast<unsigned char>(data[0x3E])) << 16U)
                               | (static_cast<std::size_t>(static_cast<unsigned char>(data[0x3F])) << 24U);
    return header + 4U <= data.size() && data.substr(header, 4U) == std::string_view("PE\0\0", 4U);
}

fs::path child(const fs::path& directory, const std::string_view name) {
    std::error_code error;
    const fs::path exact = directory / std::string(name);
    if (fs::exists(exact, error)) return exact;
    if (fs::is_directory(directory, error)) {
        for (const auto& entry : fs::directory_iterator(directory, error)) {
            if (lower(entry.path().filename().string()) == lower(name)) return entry.path();
        }
    }
    return {};
}

FontExtractResult fail(FontExtractFailure failure, std::string message) {
    FontExtractResult result;
    result.status = FontExtractStatus::failed;
    result.failure = failure;
    result.message = std::move(message);
    return result;
}

const PinnedFontFace* pinned_for(const FontExtractPolicy& policy, const std::string_view face) {
    for (const PinnedFontFace& pinned : policy.faces) {
        if (pinned.face == face) return &pinned;
    }
    return nullptr;
}

std::string json_string(const std::string_view text) {
    return eawr::core::json_quote_space_controls(text);
}

// The expected faces by name, plus warnings (an unknown build whose faces differ from the pins).
struct Selection final {
    std::map<std::string, const Face*> faces;
    std::vector<std::string> warnings;
    std::string error;
};

Selection select_faces(const Scan& scan, const bool pinned_build, const FontExtractPolicy& policy) {
    Selection selection;
    for (const std::string_view wanted : embedded_faces) {
        const Face* chosen = nullptr;
        std::size_t distinct = 0U;
        for (const Face& face : scan.faces) {
            const std::string name = lower(wanted);
            if (lower(face.full_name) != name && lower(face.postscript_name) != name) continue;
            if (chosen != nullptr && chosen->data == face.data) continue;
            ++distinct;
            if (chosen == nullptr) chosen = &face;
        }
        if (distinct > 1U) {
            selection.error = std::to_string(distinct) + " different fonts are named " + std::string(wanted);
            return selection;
        }
        if (chosen != nullptr) selection.faces.emplace(std::string(wanted), chosen);
    }
    std::string missing;
    for (const std::string_view wanted : embedded_faces) {
        if (!selection.faces.contains(std::string(wanted))) missing += (missing.empty() ? "" : ", ") + std::string(wanted);
    }
    if (!missing.empty()) {
        std::string names;
        for (const Face& face : scan.faces) names += (names.empty() ? "" : ", ") + face.full_name;
        selection.error = "missing faces: " + missing + " (valid fonts found: " + (names.empty() ? "none" : names)
                          + (scan.rejected != 0U ? "; " + std::to_string(scan.rejected)
                                                       + " other table directories failed validation"
                                                 : std::string())
                          + ")";
        return selection;
    }
    for (const auto& [name, face] : selection.faces) {
        const PinnedFontFace* pinned = pinned_for(policy, name);
        if (pinned != nullptr && pinned->bytes == face->data.size() && pinned->sha256 == face->sha256) continue;
        const std::string message = name + " (" + std::to_string(face->data.size()) + " bytes, sha256 " + face->sha256
                                    + ") differs from the pinned face";
        if (pinned_build) {
            selection.error = message;
            return selection;
        }
        selection.warnings.push_back(message);
    }
    return selection;
}

std::string build_manifest(const std::string_view digest, const std::size_t exe_bytes, const std::string_view build,
                           const Selection& selection, const FontExtractPolicy& policy) {
    std::string text = "{\n  \"schema\": " + json_string(manifest_schema) + ",\n  \"tool\": \"viewer font_extract\",\n"
                       "  \"source\": {\n    \"file\": \"StarWarsG.exe\",\n    \"sha256\": \""
                       + std::string(digest) + "\",\n    \"bytes\": " + std::to_string(exe_bytes) + ",\n    \"build\": "
                       + (build.empty() ? std::string("null") : json_string(build)) + "\n  },\n  \"faces\": [\n";
    bool first = true;
    for (const std::string_view name : embedded_faces) {
        const Face& face = *selection.faces.at(std::string(name));
        const PinnedFontFace* pinned = pinned_for(policy, name);
        const bool matches = pinned != nullptr && pinned->bytes == face.data.size() && pinned->sha256 == face.sha256;
        text += std::string(first ? "" : ",\n") + "    {\n      \"face\": " + json_string(name) + ",\n      \"file\": "
                + json_string(std::string(name) + ".ttf") + ",\n      \"family\": " + json_string(face.family)
                + ",\n      \"subfamily\": " + json_string(face.subfamily) + ",\n      \"bytes\": "
                + std::to_string(face.data.size()) + ",\n      \"sha256\": \"" + face.sha256 + "\",\n      \"pinned\": "
                + (matches ? "true" : "false") + "\n    }";
        first = false;
    }
    return text + "\n  ]\n}\n";
}

// Write through a new, uniquely named file and move it over the target.
bool publish(const fs::path& target, const std::string_view data) {
    std::random_device random;
    const fs::path temporary = target.parent_path() / ("." + target.filename().string() + "." + std::to_string(random())
                                                       + ".tmp");
    // "x": the create fails when the name exists, so a planted file or link is never followed.
#ifdef _WIN32
    std::FILE* file = nullptr;
    if (_wfopen_s(&file, temporary.c_str(), L"wbx") != 0) file = nullptr;
#else
    std::FILE* const file = std::fopen(temporary.c_str(), "wbx");
#endif
    if (file == nullptr) return false;
    const bool written = std::fwrite(data.data(), 1U, data.size(), file) == data.size() && std::fflush(file) == 0;
    const bool closed = std::fclose(file) == 0;
    if (!written || !closed) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        return false;
    }
    std::error_code error;
    fs::rename(temporary, target, error);
    if (error) {
        fs::remove(temporary, error);
        return false;
    }
    return true;
}

// The SHA-256 the cache's manifest names as its source, empty when there is no usable manifest.
std::string manifest_source_digest(const fs::path& cache) {
    std::string text;
    if (!read_file(cache / std::string(manifest_name), text)) return {};
    if (text.find("\"schema\": " + json_string(manifest_schema)) == std::string::npos) return {};
    const std::size_t source = text.find("\"source\"");
    if (source == std::string::npos) return {};
    constexpr std::string_view key = "\"sha256\": \"";
    const std::size_t at = text.find(key, source);
    if (at == std::string::npos || at + key.size() + 64U > text.size()) return {};
    return text.substr(at + key.size(), 64U);
}

// The SHA-256 the manifest records for one face, empty when it names none.
std::string manifest_face_digest(const std::string_view manifest, const std::string_view face) {
    const std::string entry = "\"face\": " + json_string(face);
    const std::size_t at = manifest.find(entry);
    if (at == std::string_view::npos) return {};
    constexpr std::string_view key = "\"sha256\": \"";
    const std::size_t digest = manifest.find(key, at);
    if (digest == std::string_view::npos || digest + key.size() + 64U > manifest.size()) return {};
    return std::string(manifest.substr(digest + key.size(), 64U));
}

// True when every face file is present, structurally valid, named for its face, byte-identical to
// the digest the manifest recorded when it was extracted and, for a pinned build, to the pins.
bool cache_is_valid(const fs::path& cache, const std::string_view digest, const bool pinned_build,
                    const FontExtractPolicy& policy) {
    if (manifest_source_digest(cache) != digest) return false;
    std::string manifest;
    if (!read_file(cache / std::string(manifest_name), manifest)) return false;
    for (const std::string_view name : embedded_faces) {
        std::string data;
        if (!read_file(cache / (std::string(name) + ".ttf"), data) || data.size() > max_face_bytes) return false;
        const auto names = data::ui::read_sfnt_face(
            std::span<const std::byte>(reinterpret_cast<const std::byte*>(data.data()), data.size()));
        if (!names || (lower(names.value().full_name) != lower(name) && lower(names.value().postscript_name) != lower(name))) {
            return false;
        }
        const std::string observed = hash_hex(data);
        if (observed != manifest_face_digest(manifest, name)) return false;
        if (pinned_build) {
            const PinnedFontFace* pinned = pinned_for(policy, name);
            if (pinned == nullptr || pinned->bytes != data.size() || pinned->sha256 != observed) return false;
        }
    }
    return true;
}

} // namespace

const FontExtractPolicy& default_font_extract_policy() noexcept {
    return default_policy;
}

fs::path locate_foc_executable(const fs::path& game_root) {
    try {
        const fs::path corruption = lower(game_root.filename().string()) == "corruption"
                                        ? game_root
                                        : child(game_root, "corruption");
        if (corruption.empty()) return {};
        const fs::path executable = child(corruption, "StarWarsG.exe");
        std::error_code error;
        return !executable.empty() && fs::is_regular_file(executable, error) ? executable : fs::path{};
    } catch (const std::exception&) {
        return {};
    }
}

FontExtractResult ensure_font_cache(const fs::path& game_root, const fs::path& cache_directory,
                                    const bool allow_unknown_build, const FontExtractPolicy& policy) try {
    const fs::path executable = locate_foc_executable(game_root);
    if (executable.empty()) {
        return fail(FontExtractFailure::unreadable,
                    "No corruption/StarWarsG.exe under " + game_root.string()
                        + ". EAWR supports Forces of Corruption only; choose the folder that holds GameData and corruption.");
    }
    std::string data;
    if (!read_file(executable, data)) {
        return fail(FontExtractFailure::unreadable,
                    "Cannot read " + executable.string() + ". Check read permissions for this account and verify or repair "
                    "the FoC installation.");
    }
    if (!is_pe(data)) {
        return fail(FontExtractFailure::unreadable, executable.string() + " is not a Windows executable.");
    }
    FontExtractResult result;
    result.executable_sha256 = hash_hex(data);
    std::string_view label;
    for (const KnownFontBuild& known : policy.builds) {
        if (known.sha256 == result.executable_sha256) label = known.label;
    }
    const bool pinned_build = !label.empty();
    result.build = std::string(label);
    if (!pinned_build && !allow_unknown_build) {
        std::string known;
        for (const KnownFontBuild& build : policy.builds) known += (known.empty() ? "" : "; ") + std::string(build.label);
        return fail(FontExtractFailure::unknown_build,
                    "This Forces of Corruption build is not one EAWR has verified (" + executable.string() + ", sha256 "
                        + result.executable_sha256 + ", " + std::to_string(data.size()) + " bytes). EAWR verifies: " + known
                        + ". Use the Steam edition of Forces of Corruption, or start with --eawr-allow-unknown-build to try "
                          "extracting the fonts from this build anyway.");
    }
    if (cache_is_valid(cache_directory, result.executable_sha256, pinned_build, policy)) {
        result.status = FontExtractStatus::cached;
        return result;
    }
    const Scan scan = scan_faces(data);
    Selection selection = select_faces(scan, pinned_build, policy);
    if (!selection.error.empty()) return fail(FontExtractFailure::faces, "Cannot extract the EmpireAtWar fonts: " + selection.error);
    result.warnings = std::move(selection.warnings);
    if (!pinned_build) {
        result.warnings.insert(result.warnings.begin(), "unknown executable (sha256 " + result.executable_sha256
                                                            + "); extracted because of --eawr-allow-unknown-build");
    }
    std::error_code error;
    fs::create_directories(cache_directory, error);
    if (error) {
        return fail(FontExtractFailure::output, "Could not create the font cache " + cache_directory.string() + ": "
                                                    + error.message());
    }
    for (const std::string_view name : embedded_faces) {
        if (!publish(cache_directory / (std::string(name) + ".ttf"), selection.faces.at(std::string(name))->data)) {
            return fail(FontExtractFailure::output, "Could not write the font cache in " + cache_directory.string());
        }
    }
    // The manifest goes last: a cache without it is never taken as valid.
    if (!publish(cache_directory / std::string(manifest_name),
                 build_manifest(result.executable_sha256, data.size(), label, selection, policy))) {
        return fail(FontExtractFailure::output, "Could not write the font cache in " + cache_directory.string());
    }
    result.status = FontExtractStatus::extracted;
    return result;
} catch (const std::exception& error) {
    return fail(FontExtractFailure::unreadable, std::string("Font extraction failed: ") + error.what());
}

} // namespace eawr::presentation::ui
