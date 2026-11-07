// Measurement tool: content digest of one folder, cold then warm. Usage: mods_content_digest_tool <folder> [cache folder]
// Reads the folder (never writes to it); the cache folder is the only thing written.
#include "eawr/mods/content_digest.hpp"

#include <chrono>
#include <iostream>

using namespace eawr::mods;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: mods_content_digest_tool <folder> [cache folder]\n";
        return 2;
    }
    const auto root = steam::path_from_utf8(argv[1]);
    ContentDigestOptions options;
    if (argc > 2) options.cache_folder = steam::path_from_utf8(argv[2]);
    const auto t0 = std::chrono::steady_clock::now();
    const TreeFingerprint print = tree_fingerprint(root);
    const auto t1 = std::chrono::steady_clock::now();
    ModRecord record;
    record.key = "tool:measure";
    record.root = argv[1];
    record.fingerprint.tree = print.tree;
    record.fingerprint.tree_files = print.files;
    record.fingerprint.tree_capped = print.capped;
    const auto seconds = [](auto a, auto b) { return std::chrono::duration<double>(b - a).count(); };
    std::cout << "walk " << seconds(t0, t1) << " s, " << print.files << " files\n";
    ContentDigests digests(options);
    const auto t2 = std::chrono::steady_clock::now();
    (void)digests.lookup(record, true);
    const ContentDigest done = digests.wait(record, std::chrono::hours(24));
    const auto t3 = std::chrono::steady_clock::now();
    const double wall = seconds(t2, t3);
    std::cout << "state " << to_string(done.state) << " note '" << done.note << "' sha256 " << done.sha256 << " files " << done.files
              << " bytes " << done.bytes << "\ncold wall " << wall << " s, " << (wall > 0 ? static_cast<double>(done.bytes) / 1048576.0 / wall : 0.0) << " MiB/s\n";
    const auto t4 = std::chrono::steady_clock::now();
    const ContentDigest warm = digests.lookup(record, false);
    const auto t5 = std::chrono::steady_clock::now();
    std::cout << "warm lookup " << seconds(t4, t5) * 1000.0 << " ms, state " << to_string(warm.state) << "\n";
    return done.state == ContentDigestState::ready ? 0 : 1;
}
