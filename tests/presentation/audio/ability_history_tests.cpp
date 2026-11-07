// presentation::audio::AbilityHistory against the std::map it replaced: a seeded sequence of ticks in which
// units appear and vanish, abilities change, a kind repeats within a unit and the visiting order is
// occasionally shuffled; every lookup must give what the rebuilt-every-tick map gave.

#include "eawr/presentation/audio/ability_history.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace audio = eawr::presentation::audio;
using eawr::sim::EntityId;
using Kind = eawr::sim::tactical::AbilityKind;

int failures = 0;

void expect(const bool condition, const std::string& message) {
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

[[nodiscard]] bool equal(const audio::AbilityMemory& a, const audio::AbilityMemory& b) {
    return a.active == b.active && a.timed == b.timed && a.remaining == b.remaining && a.started == b.started
        && a.positioned == b.positioned;
}

struct Status final {
    Kind kind{};
    audio::AbilityMemory memory{};
};
struct Unit final {
    EntityId entity{};
    std::vector<Status> statuses;
};

void test_matches_rebuilt_map() {
    const Kind kinds[] = {Kind::harmonic_bomb, Kind::weaken_enemy, Kind::buzz_droids, Kind::self_destruct,
                          Kind::corrupt_systems, Kind::blast, Kind::leech_shields};
    std::mt19937_64 random(0xab11171ULL);
    audio::AbilityHistory history;
    std::map<std::pair<EntityId, Kind>, audio::AbilityMemory> reference;
    std::size_t lookups = 0, hits = 0, shuffled_ticks = 0;
    for (int tick = 0; tick < 4000; ++tick) {
        std::vector<Unit> units;
        for (EntityId id = 1; id <= 120; ++id) {
            if (random() % 100 >= 82) continue;
            Unit unit{id * (id > 60 ? 977U : 1U), {}};
            const std::size_t count = 1 + random() % 4;
            for (std::size_t index = 0; index < count; ++index) {
                // Mostly distinct kinds, occasionally a repeated one.
                const Kind kind = kinds[(id + index * (random() % 8 == 0 ? 0 : 1)) % std::size(kinds)];
                unit.statuses.push_back({kind, {random() % 2 == 0, random() % 3 == 0, static_cast<std::uint32_t>(random() % 40),
                                                 random() % 50, random() % 4 == 0}});
            }
            units.push_back(std::move(unit));
        }
        std::sort(units.begin(), units.end(), [](const Unit& a, const Unit& b) { return a.entity < b.entity; });
        if (tick % 211 == 210) {
            ++shuffled_ticks;
            std::shuffle(units.begin(), units.end(), random);
        }
        std::map<std::pair<EntityId, Kind>, audio::AbilityMemory> next;
        history.begin_tick();
        for (const Unit& unit : units) {
            history.begin_instance(unit.entity);
            for (const Status& status : unit.statuses) {
                const auto want = reference.find({unit.entity, status.kind});
                const audio::AbilityMemory* got = history.before(status.kind);
                ++lookups;
                if (want != reference.end()) ++hits;
                if ((want == reference.end()) != (got == nullptr) || (got != nullptr && !equal(*got, want->second))) {
                    expect(false, "lookup differs from the rebuilt map at tick " + std::to_string(tick));
                    return;
                }
                next[{unit.entity, status.kind}] = status.memory;
                history.record(unit.entity, status.kind, status.memory);
            }
        }
        history.end_tick();
        reference = std::move(next);
    }
    expect(lookups > 100000 && hits > lookups / 2, "the sequence looked up enough remembered abilities to mean something");
    expect(shuffled_ticks >= 10, "the sequence includes shuffled visiting orders");
}

} // namespace

int main() {
    test_matches_rebuilt_map();
    if (failures != 0) {
        std::cerr << failures << " ability history check(s) failed\n";
        return 1;
    }
    std::cout << "ability history contracts passed\n";
    return 0;
}
