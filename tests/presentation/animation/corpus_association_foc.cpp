// The FoC disposition of the 356 association failures P1 #24 froze (#81).
//
// The frozen ledger was taken on the Remake view (mod over FoC over EaW) with
// a same-directory prefix heuristic. The project targets FoC only, and FoC
// names a model's clips by rule: `<model or anim override>_<TYPE>_NN.ala`
// (presentation/animation/unit_clips.hpp, docs/behaviour/unit-animation.md).
// This tool mounts the FoC view (corruption/Data over GameData/Data), reads
// every frozen failure row and writes one disposition per row:
//
//   bound           FoC's rule reaches the file and it binds to that model
//   binding_failed  FoC's rule reaches the file; Player::create rejects it
//   model_failed    FoC's rule reaches the file; the model does not parse
//   index_gap       the model exists but a lower index is missing, so the
//                   retail load loop stops before this file
//   never_loaded    no FoC model or anim override name matches the rule
//   not_in_foc      the file is not in the FoC view (Remake mod content)
//
// The last line is a receipt, the SHA-256 of every line before it. The CTest
// (corpus_association_foc.py) checks it and each cause against its
// disposition; with EAWR_EAW_GAME_ROOT set, animation_corpus_association_foc_rederive
// writes the file again from the game and compares it byte for byte.
//
// usage: animation_corpus_association_foc <game-root> <frozen-metadata.tsv> <out.tsv>
// Exit codes: 0 written; 1 mount, catalog or I/O failure; 2 usage.

#include "eawr/assets/assets.hpp"
#include "eawr/core/sha256.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/presentation/animation/animation.hpp"
#include "eawr/presentation/animation/unit_clips.hpp"
#include "eawr/vfs/vfs.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace clips = eawr::presentation::animation;

[[nodiscard]] std::string lower(std::string_view text) {
    std::string result(text);
    for (char& value : result) {
        value = value == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
    }
    return result;
}

[[nodiscard]] std::string trim(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
    return std::string(text);
}

[[nodiscard]] std::string clean(std::string text) {
    for (char& value : text) {
        if (value == '\t' || value == '\n' || value == '\r') value = ' ';
    }
    return text;
}

[[nodiscard]] std::vector<std::string> split(const std::string& line) {
    std::vector<std::string> fields;
    std::size_t start = 0;
    for (;;) {
        const std::size_t tab = line.find('\t', start);
        fields.push_back(line.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
        if (tab == std::string::npos) return fields;
        start = tab + 1;
    }
}

struct Row final {
    std::string index;
    std::string frozen_stage;
    std::string path;
};

struct Override final {
    std::string object;
    std::string model;  // the object's own model name tag, as a logical path
};

} // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        std::cerr << "usage: animation_corpus_association_foc <game-root> <frozen-metadata.tsv> <out.tsv>\n";
        return 2;
    }
    std::ifstream frozen(argv[2], std::ios::binary);
    if (!frozen) {
        std::cerr << "cannot read " << argv[2] << '\n';
        return 1;
    }
    std::vector<Row> rows;
    for (std::string line; std::getline(frozen, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto fields = split(line);
        if (fields.size() >= 6 && fields[0] == "failure") rows.push_back({fields[1], fields[2], fields[5]});
    }

    const std::filesystem::path game_root = argv[1];
    std::vector<eawr::vfs::MountSpec> specs;
    for (const auto& [id, root] : std::vector<std::pair<std::string, std::filesystem::path>>{
             {"expansion", game_root / "corruption" / "Data"}, {"base", game_root / "GameData" / "Data"}}) {
        auto resolved = eawr::vfs::resolve_manifest_mount(id, root);
        if (!resolved) {
            std::cerr << resolved.error().message << '\n';
            return 1;
        }
        specs.push_back(std::move(resolved.value().mount));
    }
    auto mounted = eawr::vfs::Vfs::mount(specs);
    if (!mounted) {
        std::cerr << mounted.error().message << '\n';
        return 1;
    }
    const eawr::vfs::Vfs& vfs = mounted.value();
    const auto exists = [&](const std::string& path) { return static_cast<bool>(vfs.stat(path)); };

    // Anim override names: `<stem>` -> the objects that name it and their models.
    auto catalog = eawr::data::load_catalog(vfs, eawr::data::Profile::foc);
    if (!catalog) {
        std::cerr << catalog.error().message << '\n';
        return 1;
    }
    std::map<std::string, std::vector<Override>> overrides;
    for (const auto& definition : catalog.value().catalog.definitions()) {
        std::string anim;
        std::string model;
        for (const auto& child : definition.root.children) {
            const std::string name = lower(child.name);
            if (name == "land_model_anim_override_name" || name == "space_model_anim_override_name") {
                anim = lower(trim(child.raw_text));
            } else if (model.empty() && (name == "land_model_name" || name == "space_model_name" || name == "model_name")) {
                model = lower(trim(child.raw_text));
            }
        }
        if (anim.empty() || model.empty()) continue;
        if (anim.ends_with(".alo")) anim.resize(anim.size() - 4);
        if (!model.ends_with(".alo")) continue;
        overrides[anim].push_back({definition.id, "data/art/models/" + model});
    }

    std::ostringstream out;
    out << "eawr.animation-association-foc\t1\n"
        << "view\tfoc\texpansion:corruption/Data\tbase:GameData/Data\n"
        << "rule\tretail-model-anim-type-table\n";
    std::map<std::string, std::size_t> totals;
    for (const Row& row : rows) {
        std::string disposition;
        std::string model = "-";
        std::string detail;
        if (!exists(row.path)) {
            disposition = "not_in_foc";
            detail = "Remake mod file: absent from the FoC view (FoC-only target)";
        } else {
            // Every model the retail rule would load this file for, by model stem, then by override.
            std::vector<std::pair<std::string, clips::ClipName>> owners;
            for (const clips::ClipName& name : clips::parse_clip_name(row.path)) {
                if (exists(name.model_path)) owners.emplace_back("model_stem", name);
                const std::size_t slash = name.model_path.rfind('/');
                const std::string stem = name.model_path.substr(slash + 1, name.model_path.size() - slash - 5);
                if (const auto found = overrides.find(stem); found != overrides.end()) {
                    for (const Override& user : found->second) {
                        if (!exists(user.model)) continue;
                        clips::ClipName by_override = name;
                        by_override.model_path = user.model;
                        owners.emplace_back("anim_override:" + user.object, by_override);
                    }
                }
            }
            if (owners.empty()) {
                disposition = "never_loaded";
                detail = clips::parse_clip_name(row.path).empty()
                    ? "the name is not <base>_<TYPE>_NN.ala for any retail type"
                    : "no FoC model or anim override name is the base of this <base>_<TYPE>_NN name";
            }
            std::set<std::string> tried;
            for (const auto& [rule, owner] : owners) {
                if (!tried.insert(owner.model_path).second) continue;
                // The load loop runs 00, 01, ... up to the first missing file.
                bool reached = true;
                const std::string prefix = row.path.substr(0, row.path.size() - 6);
                for (std::uint32_t index = 0; index < owner.index; ++index) {
                    const char digits[3]{static_cast<char>('0' + index / 10), static_cast<char>('0' + index % 10), '\0'};
                    if (!exists(lower(prefix) + digits + ".ala")) reached = false;
                }
                std::string outcome;
                std::string message;
                if (!reached) {
                    outcome = "index_gap";
                    message = "a lower index of this type is missing";
                } else if (auto parsed = eawr::assets::load_model(vfs, owner.model_path); !parsed) {
                    outcome = "model_failed";
                    message = parsed.error().code + " " + parsed.error().message;
                } else if (auto clip = eawr::assets::load_animation(vfs, row.path); !clip) {
                    outcome = "binding_failed";
                    message = "clip: " + clip.error().code + " " + clip.error().message;
                } else if (auto player = clips::Player::create(parsed.value(), &clip.value()); !player) {
                    outcome = "binding_failed";
                    message = player.error().code + " " + player.error().message;
                } else {
                    outcome = "bound";
                    message = rule + " " + std::string(clips::clip_type_names[owner.type]) + " index "
                        + std::to_string(owner.index);
                }
                // The first owner that binds wins; otherwise the first owner's outcome stands.
                if (disposition.empty() || (outcome == "bound" && disposition != "bound")) {
                    disposition = outcome;
                    model = owner.model_path;
                    detail = message;
                }
            }
        }
        ++totals[disposition];
        out << "row\t" << row.index << '\t' << row.frozen_stage << '\t' << row.path << '\t' << disposition << '\t'
            << model << '\t' << clean(detail) << '\n';
    }
    for (const auto& [disposition, count] : totals) out << "total\t" << disposition << '\t' << count << '\n';
    const std::string body = out.str();
    out << "receipt\tsha256\t"
        << eawr::core::sha256_hex({reinterpret_cast<const std::uint8_t*>(body.data()), body.size()}) << '\n';

    std::ofstream output(argv[3], std::ios::binary | std::ios::trunc);
    output << out.str();
    if (!output) {
        std::cerr << "cannot write " << argv[3] << '\n';
        return 1;
    }
    for (const auto& [disposition, count] : totals) std::cout << disposition << ' ' << count << '\n';
    return 0;
}
