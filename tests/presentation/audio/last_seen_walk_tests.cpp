// presentation::audio::walk_last_seen against the keyed loop it replaced (a find in the pose map and a copy per
// instance): seeded frames with units appearing, vanishing and reappearing after a gap, ghost poses with no
// instance (spinning craft), a battle-start burst, duplicated ids and shuffled instance orders. The resulting
// last-seen map must equal the keyed loop's after every frame.

#include "eawr/core/flat_id_map.hpp"
#include "eawr/presentation/audio/last_seen_walk.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <map>
#include <random>
#include <string>
#include <vector>

namespace {

namespace audio = eawr::presentation::audio;

struct Instance final {
    std::uint64_t entity_id{};
    std::uint32_t type{};
    std::uint32_t owner{};
};
struct Pose final {
    double x{};
    double y{};
};
struct Seen final {
    double x{};
    double y{};
    std::uint32_t type{};
    std::uint32_t owner{};
    friend bool operator==(const Seen&, const Seen&) noexcept = default;
};

int failures = 0;

void expect(const bool condition, const std::string& message) {
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

[[nodiscard]] bool same(const eawr::core::FlatIdMap<Seen>& flat, const std::map<std::uint64_t, Seen>& reference) {
    if (flat.size() != reference.size()) return false;
    for (const auto& [id, want] : reference) {
        const auto* got = flat.find(id);
        if (got == nullptr || !(got->second == want)) return false;
    }
    return true;
}

void test_matches_keyed_loop() {
    std::mt19937_64 random(0x1a57ULL);
    eawr::core::FlatIdMap<Seen> flat;
    std::map<std::uint64_t, Seen> reference;
    std::size_t frames_with_burst = 0, shuffled = 0;
    for (int frame = 0; frame < 3000; ++frame) {
        std::vector<Instance> instances;
        std::map<std::uint64_t, Pose> poses;
        // Frame 0 is the battle-start burst: every unit at once.
        const std::uint64_t population = frame == 0 ? 1500 : 200 + random() % 80;
        for (std::uint64_t id = 1; id <= population; ++id) {
            const std::uint64_t entity = id * (id % 3 == 0 ? 7 : 1) + (id > 150 ? 100000 : 0);
            if (frame != 0 && random() % 100 >= 90) continue; // vanished this frame
            instances.push_back({entity, static_cast<std::uint32_t>(id % 17), static_cast<std::uint32_t>(id % 3)});
            if (frame == 0 || random() % 100 < 75) {
                poses[entity] = {static_cast<double>(random() % 1000) / 4.0, static_cast<double>(random() % 1000) / 4.0};
            }
        }
        for (int ghost = 0; ghost < 6; ++ghost) poses[500000 + random() % 5000] = {1.0, 2.0}; // poses without an instance
        std::sort(instances.begin(), instances.end(), [](const Instance& a, const Instance& b) { return a.entity_id < b.entity_id; });
        instances.erase(std::unique(instances.begin(), instances.end(),
                                    [](const Instance& a, const Instance& b) { return a.entity_id == b.entity_id; }), instances.end());
        if (frame % 5 == 4 && !instances.empty()) instances.push_back(instances[random() % instances.size()]); // a repeated id
        if (frame % 97 == 96) {
            ++shuffled;
            std::shuffle(instances.begin(), instances.end(), random);
        }
        if (frame == 0) ++frames_with_burst;

        for (const Instance& instance : instances) {
            const auto pose = poses.find(instance.entity_id);
            if (pose != poses.end()) {
                reference[instance.entity_id] = {pose->second.x, pose->second.y, instance.type, instance.owner};
            } else {
                reference.erase(instance.entity_id);
            }
        }
        audio::walk_last_seen(instances, poses, flat, [](const Instance& instance, const Pose& pose) {
            return Seen{pose.x, pose.y, instance.type, instance.owner};
        });
        if (!same(flat, reference)) {
            expect(false, "walk differs from the keyed loop at frame " + std::to_string(frame));
            return;
        }
    }
    expect(frames_with_burst == 1 && shuffled >= 20, "the sequence includes the burst frame and shuffled orders");
    expect(flat.size() > 100, "the last-seen map holds a realistic number of units at the end");
}

} // namespace

int main() {
    test_matches_keyed_loop();
    if (failures != 0) {
        std::cerr << failures << " last-seen walk check(s) failed\n";
        return 1;
    }
    std::cout << "last-seen walk contracts passed\n";
    return 0;
}
