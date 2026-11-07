#include "../../apps/viewer/src/bink_child.hpp"
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <thread>
#include <vector>
using namespace eawr::presentation::godot_backend;
namespace {
std::vector<std::byte> fixture(unsigned mode) {
    std::vector<std::byte> movie(56);
    movie[0] = std::byte{'B'}; movie[1] = std::byte{'I'}; movie[2] = std::byte{'K'}; movie[3] = std::byte{'i'};
    const auto put = [&](std::size_t offset, std::uint32_t value) {
        for (std::size_t i = 0; i < 4; ++i) movie[offset + i] = static_cast<std::byte>((value >> (8U * i)) & 0xffU);
    };
    put(4, 48); put(8, 1); put(12, 4); put(16, 1); put(20, 2); put(24, 3);
    put(28, 30); put(32, 1); put(44, 53); put(48, 56); movie.back() = static_cast<std::byte>(mode);
    return movie;
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 3) return EXIT_FAILURE;
    const std::filesystem::path helper(argv[1]), root(argv[2]);
    std::error_code ec;
    if (std::filesystem::exists(root)) return EXIT_FAILURE;
    std::filesystem::create_directories(root / "corruption", ec);
    if (ec) return EXIT_FAILURE;
    { std::ofstream dll(root / "corruption" / "bink2w64.dll", std::ios::binary); dll << "fake"; }
    bool ok = true;
    {
        auto child = open_bink_child(helper, root, fixture(0), std::make_shared<std::atomic_bool>(false));
        BinkFrame frame;
        ok = child && child->next(frame) && frame.rgba.size() == 24 && frame.rgba[0] == 42;
    }
    for (const unsigned mode : {1U, 2U, 3U, 4U, 5U, 6U, 7U}) {
        const auto started = std::chrono::steady_clock::now();
        auto child = open_bink_child(helper, root, fixture(mode), std::make_shared<std::atomic_bool>(false));
        BinkFrame frame;
        if (mode == 3 || mode >= 5) ok = ok && child && !child->next(frame);
        else ok = ok && !child;
        child.reset();
        ok = ok && std::chrono::steady_clock::now() - started < std::chrono::seconds(8);
    }
    {
        const auto started = std::chrono::steady_clock::now();
        {
            BinkWorker worker([helper, root](std::shared_ptr<std::atomic_bool> cancelled) {
                return open_bink_child(helper, root, fixture(2), std::move(cancelled));
            });
            // Timing-based startup allowance: this does not prove the child has started.
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        ok = ok && std::chrono::steady_clock::now() - started < std::chrono::seconds(2);
    }
    std::filesystem::remove(root / "corruption" / "bink2w64.dll", ec);
    std::filesystem::remove(root / "corruption", ec);
    std::filesystem::remove(root, ec);
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
