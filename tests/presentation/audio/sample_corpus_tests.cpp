// #1708: how many SFX sample references resolve and decode. Synthetic trees cover each path form
// and WAVE format on a real VFS; with EAWR_EAW_GAME_ROOT (and EAWR_MOD_AUDIO_ROOTS) every
// sample of the installed FoC data and of each installed mod is resolved too. Nothing from the
// installed data enters the repository: only counts are printed.
//
// EAWR_MOD_AUDIO_ROOTS lists name=leaf[;parent] entries separated by '|'; a root is a mod folder
// or its Data folder. EAWR_AUDIO_CORPUS_LIST=1 also prints every unresolved name.
#include "eawr/data/xml.hpp"
#include "eawr/presentation/audio/sfx.hpp"
#include "eawr/vfs/vfs.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace audio = eawr::presentation::audio;
namespace data = eawr::data;
namespace vfs = eawr::vfs;

int failures{};

void expect(const bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

std::optional<std::string> environment(const char* name) {
#ifdef _MSC_VER
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) return std::nullopt;
    std::string result(value);
    std::free(value);
#else
    const char* value = std::getenv(name);
    if (value == nullptr) return std::nullopt;
    std::string result(value);
#endif
    if (result.empty()) return std::nullopt;
    return result;
}

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](const char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    });
    return text;
}

void write_bytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

// A generated WAVE: `fmt` body and `data` payload only.
std::vector<std::uint8_t> riff(const std::vector<std::uint8_t>& fmt, const std::vector<std::uint8_t>& data) {
    std::vector<std::uint8_t> bytes;
    const auto put = [&bytes](const char* text) { bytes.insert(bytes.end(), text, text + 4); };
    const auto u32 = [&bytes](const std::uint32_t value) {
        for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xffU));
    };
    put("RIFF");
    u32(0);
    put("WAVE");
    put("fmt ");
    u32(static_cast<std::uint32_t>(fmt.size()));
    bytes.insert(bytes.end(), fmt.begin(), fmt.end());
    put("data");
    u32(static_cast<std::uint32_t>(data.size()));
    bytes.insert(bytes.end(), data.begin(), data.end());
    if ((data.size() & 1U) != 0U) bytes.push_back(0);
    return bytes;
}

std::vector<std::uint8_t> fmt(const std::uint16_t format, const std::uint16_t block_align, const std::uint16_t bits,
                              const std::vector<std::uint8_t>& extra = {}) {
    std::vector<std::uint8_t> body;
    const auto u16 = [&body](const std::uint16_t value) {
        body.push_back(static_cast<std::uint8_t>(value & 0xffU));
        body.push_back(static_cast<std::uint8_t>(value >> 8U));
    };
    u16(format);
    u16(1);
    u16(22050);
    u16(0);
    u16(static_cast<std::uint16_t>(22050U * block_align));
    u16(0);
    u16(block_align);
    u16(bits);
    body.insert(body.end(), extra.begin(), extra.end());
    return body;
}

// One-entry original MEG fixture, containing generated WAVE bytes only.
std::vector<std::uint8_t> meg(const std::string& name, const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint8_t> bytes;
    const auto u32 = [&](std::uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8) bytes.push_back(static_cast<std::uint8_t>(value >> shift));
    };
    u32(1); u32(1);
    bytes.push_back(static_cast<std::uint8_t>(name.size()));
    bytes.push_back(static_cast<std::uint8_t>(name.size() >> 8));
    bytes.insert(bytes.end(), name.begin(), name.end());
    u32(0x12345678); u32(0); u32(static_cast<std::uint32_t>(payload.size()));
    u32(static_cast<std::uint32_t>(30 + name.size())); u32(0);
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    return bytes;
}

std::string temp_name() {
    return "eawr-audio-corpus-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
}

// Every path form a mod can use for a sample, resolved on a real two-layer VFS.
void synthetic_tree() {
    const auto root = std::filesystem::temp_directory_path() / temp_name();
    const std::vector<std::uint8_t> pcm16{1, 0, 2, 0, 3, 0, 4, 0};
    write_bytes(root / "parent/Audio/SFX/Bare_Sample.wav", riff(fmt(1, 2, 16), pcm16));
    write_bytes(root / "leaf/Audio/SFX/Bare_Sample.wav", riff(fmt(1, 2, 16), {9, 0, 9, 0}));  // the leaf wins
    write_bytes(root / "leaf/Audio/Voiceovers/Rebel/Rooted_Line.wav", riff(fmt(1, 1, 8), {0x80, 0x90, 0x70}));
    write_bytes(root / "leaf/Audio/SFX/Weapons/Deep/Sub_Path.wav", riff(fmt(1, 2, 16), pcm16));
    write_bytes(root / "leaf/Audio/SFX/Adpcm_Line.wav",
                riff(fmt(0x11, 8, 4, {2, 0, 9, 0}), {0xE8, 0x03, 10, 0, 0x21, 0x43, 0x65, 0x87}));
    write_bytes(root / "leaf/Audio/SFX/Float_Line.wav", riff(fmt(3, 4, 32), {0, 0, 0, 0}));
    write_bytes(root / "leaf/Audio/SFX/Not_A_Wave.wav", {'O', 'g', 'g', 'S', 0, 0, 0, 0});
    write_bytes(root / "bare.meg", meg("Bare_Sample.wav", riff(fmt(1, 2, 16), {7, 0, 7, 0})));
    write_bytes(root / "full.meg", meg("Data\\Audio\\SFX\\Archived.wav", riff(fmt(1, 2, 16), {5, 0})));
    write_bytes(root / "prefixed.meg", meg("Data/Audio/SFX/Prefixed.wav", riff(fmt(1, 2, 16), {6, 0})));
    const std::array mounts{vfs::MountSpec{"leaf", root / "leaf", "data", {
        {root / "bare.meg", "bare", "data/audio/sfx"}, {root / "full.meg", "full", ""}, {root / "prefixed.meg", "prefixed", "data/audio/sfx"}}}, vfs::MountSpec{"parent", root / "parent", "data", {}}};
    auto filesystem = vfs::Vfs::mount(mounts);
    expect(static_cast<bool>(filesystem), "the synthetic audio tree mounts");
    if (!filesystem) { std::error_code ignored; std::filesystem::remove_all(root, ignored); return; }
    const auto decode = [&](const std::string& name, std::string& error) {
        const auto bytes = audio::open_sfx_sample(filesystem.value(), name);
        if (!bytes) {
            error = "missing";
            return std::optional<std::vector<std::int16_t>>{};
        }
        const auto pcm = audio::parse_wav(bytes.value(), error);
        if (!pcm) return std::optional<std::vector<std::int16_t>>{};
        std::vector<std::int16_t> values;
        for (std::size_t at = 0; at + 1 < pcm->data.size(); at += 2) {
            values.push_back(static_cast<std::int16_t>(std::to_integer<std::uint16_t>(pcm->data[at])
                                                       | (std::to_integer<std::uint16_t>(pcm->data[at + 1]) << 8U)));
        }
        return std::optional(values);
    };
    std::string error;
    // Bare names search the original archive name, skipping a loose SFX-directory shadow.
    auto bare = decode("BARE_SAMPLE.WAV", error);
    expect(bare && *bare == std::vector<std::int16_t>({7, 7}), "a bare archive name skips the loose SFX-directory shadow: " + error);
    expect(decode("Data/Audio/SFX/Bare_Sample.wav", error) == std::optional(std::vector<std::int16_t>{9, 9}),
           "a rooted name explicitly selects the loose SFX file");
    expect(decode("Data/Audio/SFX/Archived.wav", error) == std::optional(std::vector<std::int16_t>{5}),
           "a rooted miss finds an archive entry with the full authored path");
    expect(decode("Data/Audio/SFX/Prefixed.wav", error) == std::optional(std::vector<std::int16_t>{6}),
           "a full-path entry also resolves inside a prefixed conventional archive");
    expect(!decode("Archived.wav", error), "a full-path archive entry does not satisfy a bare request");
    const auto records = filesystem.value().candidates("data/audio/sfx/bare_sample.wav");
    expect(records && records.value().size() == 3, "loose and archive candidates remain separately available");
    if (records) {
        auto changed = records.value().front();
        ++changed.size;
        expect(!filesystem.value().open(changed), "altered candidate metadata cannot select stored bytes");
    }
    // Every ML-04 spelling of a Data-rooted name reaches the same file, with no second prefix.
    for (const char* spelling : {"Data\\Audio\\Voiceovers\\Rebel\\Rooted_Line.wav", "data/audio/voiceovers/rebel/rooted_line.wav",
                                 ".\\DATA\\Audio\\Voiceovers\\Rebel\\Rooted_Line.wav", "./Data/Audio/Voiceovers/Rebel/Rooted_Line.wav"}) {
        const auto rooted = decode(spelling, error);
        expect(rooted && *rooted == std::vector<std::int16_t>({0, 4096, -4096}),
               std::string("a rooted 8-bit name resolves and decodes: ") + spelling + " " + error);
    }
    // Unrooted sub-paths do not search loose SFX folders either.
    expect(!decode("Sub_Path.wav", error) && error == "missing", "a bare name does not search sub-folders");
    expect(!decode("Weapons\\Deep\\Sub_Path.wav", error), "an unrooted sub-path does not probe loose SFX folders");
    expect(decode("Data\\Audio\\SFX\\Weapons\\Deep\\Sub_Path.wav", error).has_value(), "the same sub-path, Data-rooted: " + error);
    // Formats.
    const auto adpcm = decode("Data/Audio/SFX/Adpcm_Line.wav", error);
    expect(adpcm && adpcm->size() == 9 && (*adpcm)[8] == 1179, "an ADPCM sample decodes: " + error);
    expect(!decode("Data/Audio/SFX/Float_Line.wav", error) && error.find("format 3") != std::string::npos, "an unsupported format is counted undecodable: " + error);
    expect(!decode("Data/Audio/SFX/Not_A_Wave.wav", error) && error.find("RIFF") != std::string::npos, "a non-WAVE file is counted undecodable: " + error);
    expect(!decode("Absent.wav", error) && error == "missing", "an absent sample is counted missing");
    // A Data-rooted name that is absent is missing, never retried under the SFX folder.
    expect(!decode("Data\\Audio\\Bare_Sample.wav", error) && error == "missing", "a rooted name is not retried under the SFX folder");
    std::filesystem::remove(root / "leaf/Audio/SFX/Bare_Sample.wav");
    std::filesystem::remove(root / "parent/Audio/SFX/Bare_Sample.wav");
    auto archives_only = vfs::Vfs::mount(mounts);
    expect(archives_only && !audio::open_sfx_sample(archives_only.value(), "Data/Audio/SFX/Bare_Sample.wav"),
           "a rooted miss never falls through to a bare archive entry via its mount prefix");
    expect(archives_only && audio::open_sfx_sample(archives_only.value(), "Bare_Sample.wav"),
           "the same bare archive entry still serves a bare request");
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

struct Tally final {
    std::size_t references{}, unique{}, resolved{}, missing{}, undecodable{};
    std::map<std::string, std::size_t> causes;
    std::map<std::string, std::size_t> missing_names;
    std::uint64_t digest{14695981039346656037ULL};          // over the decoded frames of every resolved sample
    std::uint64_t legacy_digest{14695981039346656037ULL};   // the same, resolved only as data/audio/sfx/<name>, 16-bit PCM
    std::size_t legacy_resolved{};
};

void fold(std::uint64_t& digest, const std::string& name, const std::span<const std::byte> frames) {
    for (const char c : name) digest = (digest ^ static_cast<unsigned char>(c)) * 1099511628211ULL;
    for (const std::byte b : frames) digest = (digest ^ std::to_integer<unsigned>(b)) * 1099511628211ULL;
}

Tally tally(const vfs::Vfs& filesystem) {
    Tally result;
    std::map<std::string, std::size_t> names;  // lower-case name -> references
    const auto registry = data::load_document(filesystem, "data/xml/sfxeventfiles.xml");
    expect(static_cast<bool>(registry), "sfxeventfiles.xml loads");
    if (!registry) return result;
    for (const auto& entry : registry.value().root.children) {
        if (lower(entry.name) != "file") continue;
        std::string file = entry.raw_text;
        file.erase(0, file.find_first_not_of(" \t\r\n"));
        file.erase(file.find_last_not_of(" \t\r\n") + 1);
        const auto document = data::load_document(filesystem, data::registry_include_path("data/xml/sfxeventfiles.xml", file));
        if (!document) continue;
        for (const auto& event : document.value().root.children) {
            for (const auto& field : event.children) {
                const std::string tag = lower(field.name);
                if (tag != "samples" && tag != "pre_samples" && tag != "post_samples") continue;
                for (const auto& name : audio::split_list(field.raw_text)) {
                    ++result.references;
                    ++names[lower(name)];
                }
            }
        }
    }
    result.unique = names.size();
    for (const auto& [name, count] : names) {
        std::string error;
        // The old rule: one fixed prefix, 16-bit PCM only.
        const auto legacy = filesystem.open("data/audio/sfx/" + name);
        std::string legacy_error;
        if (legacy) {
            if (const auto old = audio::parse_wav(legacy.value(), legacy_error); old && old->converted.empty()) {
                ++result.legacy_resolved;
                fold(result.legacy_digest, name, old->data);
            }
        }
        const auto bytes = audio::open_sfx_sample(filesystem, name);
        if (!bytes) {
            ++result.missing;
            result.missing_names[name] = count;
            continue;
        }
        const auto pcm = audio::parse_wav(bytes.value(), error);
        if (!pcm) {
            ++result.undecodable;
            ++result.causes[error];
            continue;
        }
        ++result.resolved;
        fold(result.digest, name, pcm->data);

    }
    return result;
}

void report(const std::string& label, const Tally& counts, const bool list) {
    const double share = counts.unique == 0 ? 0.0 : 100.0 * static_cast<double>(counts.resolved) / static_cast<double>(counts.unique);
    const double legacy_share = counts.unique == 0 ? 0.0 : 100.0 * static_cast<double>(counts.legacy_resolved) / static_cast<double>(counts.unique);
    std::cout << label << ": references=" << counts.references << " unique=" << counts.unique << " resolved=" << counts.resolved
              << " (" << share << "%) missing=" << counts.missing << " undecodable=" << counts.undecodable
              << " | old rule (data/audio/sfx/<name>, 16-bit) resolved=" << counts.legacy_resolved << " (" << legacy_share << "%)\n";
    for (const auto& [cause, count] : counts.causes) std::cout << "  undecodable: " << cause << " x" << count << '\n';
    if (!list) return;
    for (const auto& [name, count] : counts.missing_names) std::cout << "  missing: " << name << " x" << count << '\n';
}

std::optional<vfs::Vfs> mount_chain(const std::vector<vfs::LayerRoot>& leaf_roots, const std::filesystem::path& game_root) {
    auto roots = leaf_roots;
    roots.emplace_back("expansion", game_root / "corruption" / "Data");
    roots.emplace_back("base", game_root / "GameData" / "Data");
    auto chain = vfs::resolve_manifest_chain(roots);
    expect(static_cast<bool>(chain), "an audio corpus chain resolves its manifests");
    if (!chain) return std::nullopt;
    std::vector<vfs::MountSpec> mounts;
    for (auto& manifest : chain.value()) mounts.push_back(std::move(manifest.mount));
    auto filesystem = vfs::Vfs::mount(mounts);
    expect(static_cast<bool>(filesystem), "an audio corpus VFS mounts");
    if (!filesystem) return std::nullopt;
    return std::move(filesystem.value());
}

void installed_data() {
    const auto game = environment("EAWR_EAW_GAME_ROOT");
    if (!game) {
        std::cout << "audio sample corpus: skipped (set EAWR_EAW_GAME_ROOT)\n";
        return;
    }
    const bool list = environment("EAWR_AUDIO_CORPUS_LIST").has_value();
    const std::filesystem::path game_root(*game);
    if (auto foc = mount_chain({}, game_root)) {
        const Tally stock = tally(*foc);
        report("FoC", stock, list);
        // Stock FoC is plain 16-bit PCM under bare names: the same samples resolve, byte for byte as before.
        expect(stock.unique > 1000, "FoC defines its sample list");
        expect(stock.undecodable == 0, "every stock FoC sample that resolves decodes");
        expect(stock.resolved == stock.legacy_resolved && stock.digest == stock.legacy_digest,
               "stock FoC samples are the same bytes the old rule read");
    }
    const auto list_text = environment("EAWR_MOD_AUDIO_ROOTS");
    if (!list_text) {
        std::cout << "audio sample corpus mods: skipped (set EAWR_MOD_AUDIO_ROOTS)\n";
        return;
    }
    std::string_view rest = *list_text;
    while (!rest.empty()) {
        const auto bar = rest.find('|');
        const std::string_view entry = rest.substr(0, bar);
        rest = bar == std::string_view::npos ? std::string_view{} : rest.substr(bar + 1);
        const auto equals = entry.find('=');
        expect(equals != std::string_view::npos && equals != 0U && equals + 1U < entry.size(),
               "EAWR_MOD_AUDIO_ROOTS entries are name=leaf[;parent]");
        if (equals == std::string_view::npos || equals == 0U || equals + 1U >= entry.size()) continue;
        const auto roots = vfs::mod_chain_roots(std::filesystem::path(std::string(entry.substr(equals + 1U))));
        if (auto chain = mount_chain(roots, game_root)) {
            const Tally mod = tally(*chain);
            report(std::string(entry.substr(0, equals)), mod, list);
            // A sample may be absent from the mod itself, but nearly every reference must play.
            const double share = mod.unique == 0 ? 0.0 : static_cast<double>(mod.resolved) / static_cast<double>(mod.unique);
            expect(mod.unique > 0 && share >= 0.97, "an installed mod resolves at least 97% of its samples");
        }
    }
}

} // namespace

int main() {
    synthetic_tree();
    installed_data();
    if (failures != 0) {
        std::cerr << failures << " audio sample corpus check(s) failed\n";
        return 1;
    }
    std::cout << "audio sample corpus checks passed\n";
    return 0;
}
