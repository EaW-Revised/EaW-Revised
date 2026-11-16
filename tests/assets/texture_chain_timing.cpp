// legacy #6726: how long the load-time chain step takes over a mod's galactic-class textures. A measurement, not a contract:
// it passes whenever it runs and prints one table. Needs a mod on disk (EAWR_TEXTURE_TIMING_ROOT, else EAWR_REMAKE_MOD_ROOT);
// without one it prints that it was skipped. Single thread, the chain step alone (decode time is reported apart).
#include "../support/env.hpp"
#include "eawr/assets/texture_chain.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace {
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

std::string lower(std::string text) {
    for (auto& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

// The audit's name heuristic for the galactic class: backdrops, starfields, planets.
bool galactic_class(const std::string& name) {
    for (const char* word : {"galax", "planet", "star", "nebula", "backdrop"})
        if (name.find(word) != std::string::npos) return true;
    return false;
}

struct Totals {
    std::size_t files = 0, incomplete = 0, refused = 0;
    std::uint64_t top_bytes = 0, added_bytes = 0, largest = 0;
    double decode_ms = 0.0, chain_ms = 0.0, slowest_ms = 0.0;
    std::string slowest;
};
} // namespace

int main() {
    auto root = eawr::testing::environment("EAWR_TEXTURE_TIMING_ROOT");
    if (!root) root = eawr::testing::environment("EAWR_REMAKE_MOD_ROOT"); // a --needs-mod job
    if (!root || !fs::is_directory(fs::path(*root) / "Data")) {
        std::cout << "texture_chain_timing: no mod root (EAWR_TEXTURE_TIMING_ROOT / EAWR_REMAKE_MOD_ROOT); skipped\n";
        return 0;
    }
    const char* root_text = root->c_str();
    const fs::path textures = fs::path(root_text) / "Data" / "Art" / "Textures";
    Totals galactic, everything;
    std::error_code ignored;
    for (fs::recursive_directory_iterator it(textures, ignored), end; it != end; it.increment(ignored)) {
        if (!it->is_regular_file(ignored) || lower(it->path().extension().string()) != ".dds") continue;
        const std::string name = lower(it->path().filename().string());
        std::ifstream file(it->path(), std::ios::binary);
        std::vector<char> raw((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        std::vector<std::byte> bytes(raw.size());
        std::transform(raw.begin(), raw.end(), bytes.begin(), [](char c) { return static_cast<std::byte>(c); });
        const auto decode_start = Clock::now();
        eawr::assets::Source source;
        source.logical_path = it->path().string();
        auto loaded = eawr::assets::load_texture(bytes, source);
        const double decode_ms = std::chrono::duration<double, std::milli>(Clock::now() - decode_start).count();
        if (!loaded) continue;
        auto& texture = loaded.value();
        for (Totals* totals : {&everything, galactic_class(name) ? &galactic : nullptr}) {
            if (totals == nullptr) continue;
            ++totals->files;
            totals->decode_ms += decode_ms;
        }
        if (!eawr::assets::mip_chain_incomplete(texture)) continue;
        const std::uint64_t top = texture.mips.empty() ? 0 : texture.mips.front().bytes.size();
        const std::uint64_t added = eawr::assets::mip_chain_added_bytes(texture);
        const auto start = Clock::now();
        const bool done = eawr::assets::extend_mip_chain(texture, eawr::assets::ChainKind::colour);
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        for (Totals* totals : {&everything, galactic_class(name) ? &galactic : nullptr}) {
            if (totals == nullptr) continue;
            ++totals->incomplete;
            if (!done) { ++totals->refused; continue; }
            totals->top_bytes += top;
            totals->added_bytes += added;
            totals->chain_ms += ms;
            totals->largest = std::max(totals->largest, top);
            if (ms > totals->slowest_ms) { totals->slowest_ms = ms; totals->slowest = name; }
        }
    }
    const auto print = [](const char* label, const Totals& t) {
        std::cout << label << ": " << t.files << " textures decoded (" << static_cast<std::uint64_t>(t.decode_ms) << " ms), "
                  << t.incomplete << " with an incomplete chain, " << t.refused << " refused, chained "
                  << (t.incomplete - t.refused) << " (" << t.top_bytes / (1024 * 1024) << " MiB of top levels, +"
                  << t.added_bytes / (1024 * 1024) << " MiB of levels) in " << static_cast<std::uint64_t>(t.chain_ms)
                  << " ms single-threaded; slowest " << t.slowest << " " << static_cast<std::uint64_t>(t.slowest_ms) << " ms\n";
    };
    std::cout << "texture_chain_timing: " << root_text << "\n";
    print("galactic-class (name heuristic)", galactic);
    print("all textures", everything);
    return 0;
}
