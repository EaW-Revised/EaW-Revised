// Measurement tool: content digest of one folder, cold then warm.
// Usage: mods_content_digest_tool <folder> [cache folder] [--workers N] [--foreground] [--normal-io] [--yield-ms N]
// Reads the folder (never writes to it); the cache folder is the only thing written. --workers and --foreground
// (normal instead of background priority) exist to compare digest settings; the digest value never depends on them.
// Prints the process CPU time (user / kernel) after the cold run.
#include "eawr/mods/content_digest.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace eawr::mods;

int main(int argc, char** argv) {
    std::vector<std::string> positional;
    ContentDigestOptions options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--workers" && i + 1 < argc) options.worker_count = static_cast<std::size_t>(std::atoi(argv[++i]));
        else if (arg == "--foreground") options.lower_priority = false;
        else if (arg == "--normal-io") options.background_io = false;
        else if (arg == "--yield-ms" && i + 1 < argc) options.yield_for = std::chrono::milliseconds(std::atoi(argv[++i]));
        else positional.push_back(arg);
    }
    if (positional.empty()) {
        std::cerr << "usage: mods_content_digest_tool <folder> [cache folder] [--workers N] [--foreground] [--normal-io] [--yield-ms N]\n";
        return 2;
    }
    const auto root = steam::path_from_utf8(positional[0]);
    if (positional.size() > 1) options.cache_folder = steam::path_from_utf8(positional[1]);
    const auto t0 = std::chrono::steady_clock::now();
    const TreeFingerprint print = tree_fingerprint(root);
    const auto t1 = std::chrono::steady_clock::now();
    ModRecord record;
    record.key = "tool:measure";
    record.root = positional[0];
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
    std::cout << "state " << to_string(done.state) << " note '" << done.note << "' digest " << done.sha256 << " files " << done.files
              << " bytes " << done.bytes << "\ncold wall " << wall << " s, " << (wall > 0 ? static_cast<double>(done.bytes) / 1048576.0 / wall : 0.0) << " MiB/s\n";
#ifdef _WIN32
    {
        FILETIME created{}, exited{}, kernel{}, user{};
        if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
            const auto secs = [](const FILETIME& f) { return static_cast<double>((static_cast<ULONGLONG>(f.dwHighDateTime) << 32) | f.dwLowDateTime) / 1e7; };
            std::cout << "process cpu user " << secs(user) << " s, kernel " << secs(kernel) << " s, total " << secs(user) + secs(kernel) << " s\n";
        }
    }
#endif
    const ContentJobStats stats = digests.stats(record.key);
    std::cout << "files hashed " << stats.files_hashed << ", reused " << stats.files_reused << ", bytes read " << stats.bytes_read << "\n";
    const auto t4 = std::chrono::steady_clock::now();
    const ContentDigest warm = digests.lookup(record, false);
    const auto t5 = std::chrono::steady_clock::now();
    std::cout << "warm lookup " << seconds(t4, t5) * 1000.0 << " ms, state " << to_string(warm.state) << "\n";
    return done.state == ContentDigestState::ready ? 0 : 1;
}
