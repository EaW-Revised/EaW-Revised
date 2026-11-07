#include "../../src/presentation/godot/ui/bink_movie.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <thread>

using namespace eawr::presentation::godot_backend;
namespace {
struct State {
    std::atomic<int> count{};
    std::atomic<bool> destroyed{};
    std::thread::id owner;
    std::mutex mutex;
    std::condition_variable changed;
    bool release_second{};
};
class Fake final : public BinkDecoder {
public:
    explicit Fake(std::shared_ptr<State> state) : state_(std::move(state)) { state_->owner = std::this_thread::get_id(); }
    ~Fake() override { if (state_->owner == std::this_thread::get_id()) {
        { std::lock_guard lock(state_->mutex); state_->destroyed = true; }
        state_->changed.notify_one();
    } }
    bool next(BinkFrame& frame) override {
        if (state_->owner != std::this_thread::get_id()) return false;
        const auto n = ++state_->count;
        if (n == 2) {
            std::unique_lock lock(state_->mutex);
            state_->changed.notify_one();
            state_->changed.wait(lock, [&] { return state_->release_second; });
        }
        frame = {1, 1, 30, 1, {static_cast<std::uint8_t>(n), 2, 3, 4}, {}, 0, 0, {}, 0};
        return true;
    }
private:
    std::shared_ptr<State> state_;
};
bool wait_frame(BinkWorker& worker, BinkFrame& frame) {
    for (int i = 0; i < 2000; ++i) {
        if (worker.take(frame)) return true;
        if (worker.failed()) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}
}
int main() {
    std::vector<std::byte> movie(56);
    movie[0] = std::byte{'B'}; movie[1] = std::byte{'I'}; movie[2] = std::byte{'K'}; movie[3] = std::byte{'i'};
    const auto put = [&](std::size_t offset, std::uint32_t value) {
        for (std::size_t i = 0; i < 4; ++i) movie[offset + i] = static_cast<std::byte>((value >> (8U * i)) & 0xffU);
    };
    put(4, 48); put(8, 1); put(12, 4); put(16, 1); put(20, 2); put(24, 3);
    put(28, 30); put(32, 1); put(44, 53); put(48, 56);
    BinkMetadata metadata;
    if (!admit_bink(movie, 1024, metadata) || metadata.width != 2 || metadata.frames != 1) return EXIT_FAILURE;
    if (admit_bink(std::span(movie).first(55), 1024, metadata)) return EXIT_FAILURE;
    put(44, 44);
    if (admit_bink(movie, 1024, metadata)) return EXIT_FAILURE;
    put(44, 53); put(40, 1);
    if (admit_bink(movie, 1024, metadata)) return EXIT_FAILURE;
    put(40, 0);
    if (admit_bink(movie, 57, metadata)) return EXIT_FAILURE;
    const auto bad_word = [&](std::size_t offset, std::uint32_t value) {
        const auto original = movie;
        put(offset, value);
        const bool rejected = !admit_bink(movie, 1024, metadata);
        movie = original;
        return rejected;
    };
    // Contract matrix: invalid declared length, counts, packet bounds, geometry,
    // time base, audio profile and first/terminal positions.
    for (const auto& [offset, value] : std::vector<std::pair<std::size_t, std::uint32_t>>{
             {4, 0xffffffffU}, {4, 47}, {4, 49}, {8, 0}, {8, 2}, {12, 0}, {12, 1},
             {12, 57}, {16, 0}, {16, 1'000'001}, {20, 0}, {20, 32768}, {24, 0},
             {24, 32768}, {28, 0}, {32, 0}, {40, 1}, {44, 44}, {44, 60},
             {48, 0}, {48, 52}, {48, 60}}) {
        if (!bad_word(offset, value)) return EXIT_FAILURE;
    }
    put(20, 0xfffffffeU); // Verified signed-width normalization.
    if (!admit_bink(movie, 1024, metadata) || metadata.width != 2) return EXIT_FAILURE;
    put(20, 2);
    put(16, 2); // The native bound may exceed the actual frame count.
    if (!admit_bink(movie, 1024, metadata)) return EXIT_FAILURE;
    put(16, 1);
    // Exercise each truncation boundary independently, including a cut table,
    // a cut first packet, and the final byte. The declared length stays intact.
    for (const std::size_t length : {0U, 3U, 4U, 8U, 43U, 44U, 48U, 51U, 52U, 53U, 55U})
        if (admit_bink(std::span(movie).first(length), 1024, metadata)) return EXIT_FAILURE;
    put(20, 32767);
    if (!admit_bink(movie, 1'000'000, metadata)) return EXIT_FAILURE;
    put(20, 2); put(24, 32767);
    if (!admit_bink(movie, 1'000'000, metadata)) return EXIT_FAILURE;
    put(24, 3);
    // Payload changes retain structural validity; the helper is the safety boundary.
    for (const auto value : {std::byte{0}, std::byte{255}, std::byte{0xa5}}) {
        movie[52] = value;
        if (!admit_bink(movie, 1024, metadata)) return EXIT_FAILURE;
    }
    // A two-frame file checks adjacent starts independently of the terminal.
    movie.resize(68);
    put(4, 60); put(8, 2); put(12, 8); put(16, 2); put(44, 57); put(48, 60); put(52, 68);
    if (!admit_bink(movie, 1024, metadata)) return EXIT_FAILURE;
    for (const auto value : {56U, 52U, 72U}) if (!bad_word(48, value)) return EXIT_FAILURE;
    if (!bad_word(52, 0) || !bad_word(52, 72)) return EXIT_FAILURE;
    const auto state = std::make_shared<State>();
    {
        BinkWorker worker([state](std::shared_ptr<std::atomic_bool>) { return std::make_unique<Fake>(state); });
        BinkFrame first;
        if (!wait_frame(worker, first) || first.rgba != std::vector<std::uint8_t>{1, 2, 3, 4}) return EXIT_FAILURE;
        // Snapshot only: this does not prove absence of delayed run-ahead.
        if (state->count != 1) return EXIT_FAILURE;
        worker.request();
        {
            std::unique_lock lock(state->mutex);
            if (!state->changed.wait_for(lock, std::chrono::seconds(2), [&] { return state->count == 2; })) return EXIT_FAILURE;
            state->release_second = true;
        }
        state->changed.notify_one();
        BinkFrame second;
        if (!wait_frame(worker, second) || second.rgba[0] != 2) return EXIT_FAILURE;
    }
    {
        std::unique_lock lock(state->mutex);
        if (!state->changed.wait_for(lock, std::chrono::seconds(2), [&] { return state->destroyed.load(); })) return EXIT_FAILURE;
    }
    if (state->owner == std::this_thread::get_id()) return EXIT_FAILURE;
    {
        BinkWorker worker([](std::shared_ptr<std::atomic_bool>) { return std::unique_ptr<BinkDecoder>{}; });
        BinkFrame ignored;
        if (wait_frame(worker, ignored) || !worker.failed()) return EXIT_FAILURE;
    }
    std::cout << "bink worker contracts passed\n";
    return EXIT_SUCCESS;
}
