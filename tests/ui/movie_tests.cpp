// HUD movie contracts (G12, #237; docs/ui/hud-movies.md). Synthetic movies.xml
// and movie files on a two-layer chain; with EAWR_EAW_GAME_ROOT (and
// EAWR_MOD_HUD_ROOTS) every declaration of each corpus is resolved as well.

#include "eawr/data/ui/movie.hpp"

#include "ui_test_support.hpp"

#include <array>
#include <cstddef>
#include <iostream>
#include <regex>
#include <string>
#include <utility>

namespace {
using namespace eawr;
namespace ui = data::ui;

void expect(const bool condition, const std::string& message) {
    test::ui::expect(condition, message.c_str());
}

bool fails_with(const core::Result<ui::HudMovie>& result, const std::string_view code) {
    return !result && result.error().code == code;
}

void synthetic() {
    test::ui::TempTree tree("movie");
    test::ui::write_text(tree.root / "parent/xml/movies.xml", R"(<?xml version="1.0"?>
<Movies>
  <Movie Name="Portrait"><Movie_File>Data\Art\Movies\Binked\portrait.bik</Movie_File>
    <Alpha>true</Alpha><Commandbar_Offset>0 -235</Commandbar_Offset></Movie>
  <Movie Name="Plain"><Movie_File>Data\Art\Movies\Binked\plain.bik</Movie_File></Movie>
  <!-- <Movie Name="Commented"><Movie_File>Data\Art\Movies\Binked\plain.bik</Movie_File></Movie> -->
  <Movie Name="Later"><Movie_File>Data\Art\Movies\Binked\absent.bik</Movie_File></Movie>
  <Movie Name="later"><Movie_File>Data\Art\Movies\Binked\plain.bik</Movie_File></Movie>
  <Movie Name="Empty"><Movie_File></Movie_File></Movie>
  <Movie Name="Missing"><Movie_File>Data\Art\Movies\Binked\absent.bik</Movie_File></Movie>
  <Movie Name="Bink2"><Movie_File>Data\Art\Movies\Binked\two.bk2</Movie_File></Movie>
  <Movie Name="Theora"><Movie_File>Data\Art\Movies\Binked\named.bik</Movie_File></Movie>
  <Movie Name="Junk"><Movie_File>Data\Art\Movies\Binked\junk.bik</Movie_File></Movie>
</Movies>)");
    test::ui::write_text(tree.root / "parent/art/movies/binked/portrait.bik", "BIKi parent bytes");
    test::ui::write_text(tree.root / "leaf/art/movies/binked/portrait.bik", "BIKi leaf bytes");
    test::ui::write_text(tree.root / "parent/art/movies/binked/plain.bik", "BIKi plain bytes");
    test::ui::write_text(tree.root / "parent/art/movies/binked/two.bk2", "KB2j bytes");
    test::ui::write_text(tree.root / "parent/art/movies/binked/named.bik", "OggS bytes");
    test::ui::write_text(tree.root / "parent/art/movies/binked/junk.bik", "RIFF bytes");
    const std::array mounts{vfs::MountSpec{"leaf", tree.root / "leaf", "data", {}},
                            vfs::MountSpec{"parent", tree.root / "parent", "data", {}}};
    auto chain = vfs::Vfs::mount(mounts);
    auto parent = vfs::Vfs::mount(std::span(mounts).subspan(1));
    expect(chain && parent, "the synthetic movie chain mounts");
    if (!chain || !parent) return;

    const auto portrait = ui::resolve_hud_movie(chain.value(), "pOrTrAiT");
    expect(static_cast<bool>(portrait), "a declared movie resolves, name ASCII case-insensitive");
    if (portrait) {
        const ui::HudMovie& movie = portrait.value();
        expect(movie.name == "pOrTrAiT", "the movie keeps the requested name");
        expect(movie.source.layer_id == "leaf", "the Movie_File resolves through the chain independently of movies.xml");
        expect(movie.source.canonical_path == "data/art/movies/binked/portrait.bik",
            "a backslashed Movie_File is a logical VFS path");
        expect(movie.format == ui::MovieFormat::bink1, "the BIK signature identifies Bink 1");
        expect(movie.alpha, "<Alpha>true</Alpha> is kept");
        expect(movie.offset_x == 0.0F && movie.offset_y == -235.0F, "<Commandbar_Offset> is kept");
        expect(std::regex_match(movie.cache_key, std::regex("[0-9a-f]{64}-hud-v1-alpha")),
            "the cache key is the source SHA-256, the recipe and the alpha packing");
        expect(ui::movie_cache_file(movie) == movie.cache_key + ".ogv", "the cache entry is <key>.ogv");
        const auto inherited = ui::resolve_hud_movie(parent.value(), "Portrait");
        expect(inherited && inherited.value().cache_key != movie.cache_key,
            "replaced movie bytes select another cache entry");
    }
    const auto plain = ui::resolve_hud_movie(chain.value(), "Plain");
    expect(plain && !plain.value().alpha && plain.value().cache_key.ends_with("-hud-v1-rgb"),
        "a movie without <Alpha> is converted without the alpha packing");
    const auto later = ui::resolve_hud_movie(chain.value(), "Later");
    expect(later && later.value().source.canonical_path == "data/art/movies/binked/plain.bik",
        "a later declaration of the same name wins");
    expect(plain && later && plain.value().cache_key == later.value().cache_key,
        "two names on one file share one cache entry");

    expect(fails_with(ui::resolve_hud_movie(chain.value(), "Commented"), ui::diagnostic_codes::movie_unknown),
        "a commented-out declaration is not a movie (EAWR-UI-0702)");
    expect(fails_with(ui::resolve_hud_movie(chain.value(), "Unknown"), ui::diagnostic_codes::movie_unknown),
        "an undeclared name is EAWR-UI-0702");
    expect(fails_with(ui::resolve_hud_movie(chain.value(), "Empty"), ui::diagnostic_codes::movie_file),
        "an empty Movie_File is EAWR-UI-0703");
    const auto missing = ui::resolve_hud_movie(chain.value(), "Missing");
    expect(fails_with(missing, ui::diagnostic_codes::movie_file) &&
               missing.error().logical_path == "data/art/movies/binked/absent.bik",
        "a Movie_File the VFS lacks is EAWR-UI-0703 at its logical path");
    for (const char* name : {"Bink2", "Theora", "Junk"}) {
        expect(fails_with(ui::resolve_hud_movie(chain.value(), name), ui::diagnostic_codes::movie_format),
            std::string(name) + ": a file that is not Bink 1 is EAWR-UI-0704, whatever its extension");
    }

    test::ui::write_text(tree.root / "broken/xml/movies.xml", "<Movies><Movie Name=\"x\">");
    const std::array broken{vfs::MountSpec{"broken", tree.root / "broken", "data", {}}};
    auto malformed = vfs::Vfs::mount(broken);
    expect(malformed && fails_with(ui::resolve_hud_movie(malformed.value(), "x"), ui::diagnostic_codes::movie_catalog),
        "a malformed movies.xml is EAWR-UI-0701");
    const std::array empty{vfs::MountSpec{"empty", tree.root / "leaf", "data", {}}};
    auto absent = vfs::Vfs::mount(empty);
    expect(absent && fails_with(ui::resolve_hud_movie(absent.value(), "Portrait"), ui::diagnostic_codes::movie_catalog),
        "a missing movies.xml is EAWR-UI-0701");

    const std::array<std::byte, 2> short_bytes{std::byte{'B'}, std::byte{'I'}};
    expect(ui::movie_format({}) == ui::MovieFormat::unknown && ui::movie_format(short_bytes) == ui::MovieFormat::unknown,
        "too few bytes are no format");
}

// Every declaration of a corpus resolves to Bink 1 or names a file the VFS
// lacks (EAWR-UI-0703): nothing else, in particular no Bink 2, is shipped.
void corpus(const std::string& label, const vfs::Vfs& filesystem) {
    const auto xml = filesystem.open(ui::movies_xml_path);
    expect(static_cast<bool>(xml), label + ": the corpus has movies.xml");
    if (!xml) return;
    std::string text(reinterpret_cast<const char*>(xml.value().data()), xml.value().size());
    text = std::regex_replace(text, std::regex(R"(<!--[\s\S]*?-->)"), "");
    const std::regex declaration(R"rx(<Movie\s+Name\s*=\s*"([^"]+)")rx", std::regex::icase);
    std::size_t resolved{};
    std::size_t missing{};
    for (auto it = std::sregex_iterator(text.begin(), text.end(), declaration); it != std::sregex_iterator(); ++it) {
        const std::string name = (*it)[1].str();
        const auto movie = ui::resolve_hud_movie(filesystem, name);
        if (movie) ++resolved;
        else if (movie.error().code == ui::diagnostic_codes::movie_file) ++missing;
        else expect(false, label + ": " + name + " fails with " + movie.error().code);
    }
    expect(resolved > 10, label + ": the corpus resolves its HUD movies");
    std::cout << "movies corpus " << label << ": " << resolved << " Bink 1, " << missing << " without a file\n";
}

} // namespace

void movie_contracts() {
    synthetic();
    if (auto retail = test::ui::foc_corpus("movies")) {
        corpus("FoC", *retail);
        const auto soldier = ui::resolve_hud_movie(*retail, "Underworld_soldier_Loop");
        expect(soldier && soldier.value().alpha && soldier.value().source.layer_id == "expansion",
            "FoC: the Underworld tactical intro portrait is an alpha Bink 1 from corruption");
    }
    for (const auto& mod : test::ui::mod_corpora("movies")) corpus(mod.name, mod.filesystem);
}
