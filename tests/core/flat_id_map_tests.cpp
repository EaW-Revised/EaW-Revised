// eawr::core::FlatIdMap against std::map: a seeded random sequence of the operations the presentation callers
// use, compared after every step, with dense and sparse ids, erase by id and by slot, clear and refill.

#include "eawr/core/flat_id_map.hpp"

#include <cstdint>
#include <iostream>
#include <map>
#include <random>
#include <string>
#include <vector>

namespace {

struct Record final {
    double x{};
    std::uint32_t tag{};
    friend bool operator==(const Record&, const Record&) noexcept = default;
};

int failures = 0;

void expect(const bool condition, const std::string& message) {
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

[[nodiscard]] bool same(const eawr::core::FlatIdMap<Record>& flat, const std::map<std::uint64_t, Record>& reference,
                        const std::vector<std::uint64_t>& universe) {
    if (flat.size() != reference.size() || flat.empty() != reference.empty()) return false;
    for (const std::uint64_t id : universe) {
        const auto want = reference.find(id);
        const auto* got = flat.find(id);
        if ((want == reference.end()) != (got == nullptr)) return false;
        if (flat.contains(id) != (want != reference.end())) return false;
        if (got != nullptr && (got->first != id || !(got->second == want->second))) return false;
    }
    return true;
}

void test_matches_std_map() {
    // Dense ids, sparse ids, the extremes, and a cluster that collides in the low hash bits.
    std::vector<std::uint64_t> universe;
    for (std::uint64_t id = 0; id < 400; ++id) universe.push_back(id);
    for (std::uint64_t id = 0; id < 100; ++id) universe.push_back(id * 100003ULL + 7000000ULL);
    for (std::uint64_t id = 0; id < 60; ++id) universe.push_back(id << 40U);
    universe.push_back(std::uint64_t{1} << 63U);
    universe.push_back(~std::uint64_t{0});

    std::mt19937_64 random(0xf1a7f1a7ULL);
    eawr::core::FlatIdMap<Record> flat;
    std::map<std::uint64_t, Record> reference;
    for (int step = 0; step < 200000; ++step) {
        const std::uint64_t id = universe[random() % universe.size()];
        switch (random() % 8) {
        case 0:
        case 1:
        case 2: {
            const Record value{static_cast<double>(random() % 1000) / 8.0, static_cast<std::uint32_t>(random())};
            flat[id] = value;
            reference[id] = value;
            break;
        }
        case 3:
            expect(flat.erase(id) == reference.erase(id), "erase by id returns the std::map count");
            break;
        case 4:
            if (auto* slot = flat.find(id)) {
                flat.erase_slot(slot);
                reference.erase(id);
            }
            break;
        case 5:
            // operator[] on a missing id default-constructs, like std::map.
            expect(flat[id] == reference[id], "operator[] default-constructs a new record");
            break;
        case 6:
            expect(flat.contains(id) == (reference.find(id) != reference.end()), "contains matches");
            break;
        default:
            if (step % 20000 == 19999) {
                flat.clear();
                reference.clear();
            }
            break;
        }
        if (step % 97 == 0 || step < 2000) {
            if (!same(flat, reference, universe)) {
                expect(false, "FlatIdMap differs from std::map at step " + std::to_string(step));
                return;
            }
        }
    }
    expect(same(flat, reference, universe), "final state matches");
}

// Fill past several growths, erase everything in a scrambled order, refill: lookups never lose an entry.
void test_growth_and_drain() {
    eawr::core::FlatIdMap<Record> flat;
    std::map<std::uint64_t, Record> reference;
    std::vector<std::uint64_t> ids;
    for (std::uint64_t id = 1; id <= 5000; ++id) ids.push_back(id * 3);
    for (const std::uint64_t id : ids) {
        flat[id] = {static_cast<double>(id), static_cast<std::uint32_t>(id)};
        reference[id] = {static_cast<double>(id), static_cast<std::uint32_t>(id)};
    }
    expect(same(flat, reference, ids), "after growth every entry is found");
    std::mt19937_64 random(7);
    std::vector<std::uint64_t> order = ids;
    for (std::size_t index = order.size(); index > 1; --index) std::swap(order[index - 1], order[random() % index]);
    for (std::size_t index = 0; index < order.size(); ++index) {
        flat.erase(order[index]);
        reference.erase(order[index]);
        if (index % 250 == 0) expect(same(flat, reference, ids), "partway through draining every remaining entry is found");
    }
    expect(flat.empty() && same(flat, reference, ids), "drained");
    for (const std::uint64_t id : ids) flat[id].tag = static_cast<std::uint32_t>(id);
    expect(flat.size() == ids.size(), "refilled");
}

// erase_if drops exactly the records the predicate names, whatever the probe runs and backward shifts do on the way.
void test_erase_if() {
    std::mt19937_64 random(0x5e1ec7ULL);
    for (int round = 0; round < 300; ++round) {
        eawr::core::FlatIdMap<Record> flat;
        std::map<std::uint64_t, Record> reference;
        std::vector<std::uint64_t> universe;
        const std::uint64_t count = 1 + random() % 700;
        const std::uint64_t spread = round % 3 == 0 ? 1 : round % 3 == 1 ? 7 : 1ULL << 33U;
        for (std::uint64_t index = 0; index < count; ++index) {
            const std::uint64_t id = index * spread + (random() % 3);
            universe.push_back(id);
            const Record value{static_cast<double>(index), static_cast<std::uint32_t>(random())};
            flat[id] = value;
            reference[id] = value;
        }
        const std::uint32_t mask = static_cast<std::uint32_t>(random() % 7) + 1U;
        const auto drop = [mask](const std::uint64_t id, const Record& record) { return ((record.tag ^ id) & mask) == 0U; };
        std::size_t want = 0;
        for (auto it = reference.begin(); it != reference.end();) {
            if (drop(it->first, it->second)) { it = reference.erase(it); ++want; }
            else ++it;
        }
        expect(flat.erase_if(drop) == want, "erase_if returns the number dropped");
        expect(same(flat, reference, universe), "erase_if leaves exactly the kept records findable");
    }
}

} // namespace

int main() {
    test_matches_std_map();
    test_growth_and_drain();
    test_erase_if();
    if (failures != 0) {
        std::cerr << failures << " flat id map check(s) failed\n";
        return 1;
    }
    std::cout << "flat id map contracts passed\n";
    return 0;
}
