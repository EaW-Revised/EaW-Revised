#include "particle_batch_census.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>

namespace {
using namespace eawr::presentation::godot_backend::detail;

void check(const bool condition, const char* message) {
    if (!condition) { std::cerr << message << '\n'; std::exit(EXIT_FAILURE); }
}

void key_depends_on_every_part() {
    const auto base = ParticleBatchCensus::key(1, 2, 3, 4);
    check(base == ParticleBatchCensus::key(1, 2, 3, 4), "key is a pure function");
    check(base != ParticleBatchCensus::key(9, 2, 3, 4), "shader qualifies a merge");
    check(base != ParticleBatchCensus::key(1, 9, 3, 4), "colour texture qualifies a merge");
    check(base != ParticleBatchCensus::key(1, 2, 9, 4), "normal texture qualifies a merge");
    check(base != ParticleBatchCensus::key(1, 2, 3, 9), "priority qualifies a merge");
    check(ParticleBatchCensus::key(1, 2, 3, 4) != ParticleBatchCensus::key(2, 1, 3, 4), "parts are not commutative");
    check(ParticleBatchCensus::key(1, 2, 3, -1) != ParticleBatchCensus::key(1, 2, 3, 1), "negative priority is distinct");
}

void keys_count_once_per_frame_and_class() {
    ParticleBatchCensus census;
    ParticleBatchCounts counts;
    const auto additive = ParticleBatchCensus::key(1, 1, 0, 0);
    const auto alpha = ParticleBatchCensus::key(2, 1, 0, 0);
    census.record(7, additive, BlendGroup::additive, 8, false, counts);
    census.record(7, additive, BlendGroup::additive, 4, true, counts);
    census.record(7, alpha, BlendGroup::alpha, 4, false, counts);
    check(counts.streams == 3 && counts.vertices == 16 && counts.identical == 1, "stream totals");
    check(counts.class_streams[0] == 2 && counts.class_streams[1] == 1 && counts.class_streams[2] == 0,
        "streams are split by blend class");
    check(counts.class_keys[0] == 1 && counts.class_keys[1] == 1 && counts.class_keys[2] == 0,
        "a repeated key is one merged draw");
}

void a_new_frame_restarts_the_key_set() {
    ParticleBatchCensus census;
    ParticleBatchCounts counts;
    const auto key = ParticleBatchCensus::key(3, 3, 3, 3);
    census.record(1, key, BlendGroup::other, 1, false, counts);
    census.record(2, key, BlendGroup::other, 1, false, counts);
    check(counts.class_keys[2] == 2, "the same key counts again in the next frame");
    ParticleBatchCounts other;
    ParticleBatchCensus second;
    second.record(2, key, BlendGroup::other, 1, false, other);
    check(other.class_keys[2] == 1, "recorders are independent");
}
} // namespace

int main() {
    key_depends_on_every_part();
    keys_count_once_per_frame_and_class();
    a_new_frame_restarts_the_key_set();
    return EXIT_SUCCESS;
}
