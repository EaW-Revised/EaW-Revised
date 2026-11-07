#include "particle_merge.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {
using namespace eawr::presentation::godot_backend::detail;

void check(const bool condition, const char* message) {
    if (!condition) { std::cerr << message << '\n'; std::exit(EXIT_FAILURE); }
}

void streams_are_laid_end_to_end() {
    const std::array<std::uint32_t, 6> quad{0, 1, 2, 2, 1, 3};
    const std::array<std::uint32_t, 12> two_quads{0, 1, 2, 2, 1, 3, 4, 5, 6, 6, 5, 7};
    std::vector<std::uint32_t> topology;
    std::uint32_t offset = 0;
    append_rebased_indices(quad, offset, topology);
    offset += 4;
    append_rebased_indices(two_quads, offset, topology);
    offset += 8;
    append_rebased_indices(quad, offset, topology);
    check(topology.size() == 24, "every index is kept");
    check(topology[0] == 0 && topology[5] == 3, "the first stream keeps its indices");
    check(topology[6] == 4 && topology[11] == 7 && topology[17] == 11, "the second stream starts after four vertices");
    check(topology[18] == 12 && topology[23] == 15, "the third stream starts after twelve vertices");
    std::vector<std::uint32_t> empty;
    append_rebased_indices({}, 9, empty);
    check(empty.empty(), "an empty stream adds nothing");
}

void index_bytes_pad_with_degenerate_slots() {
    const std::array<std::uint32_t, 3> topology{1, 2, 70000};
    std::array<std::uint8_t, 16> wide{};
    wide.fill(0xff);
    write_index_bytes(topology, 4, 4, wide.data());
    std::uint32_t last{};
    std::memcpy(&last, wide.data() + 8, sizeof(last));
    check(last == 70000, "32-bit indices are stored whole");
    for (std::size_t byte = 12; byte < 16; ++byte) check(wide[byte] == 0, "the unused slot is zeroed");
    std::array<std::uint8_t, 10> narrow{};
    narrow.fill(0xff);
    write_index_bytes(std::span<const std::uint32_t>(topology).first(2), 2, 5, narrow.data());
    std::uint16_t second{};
    std::memcpy(&second, narrow.data() + 2, sizeof(second));
    check(second == 2, "16-bit indices are stored narrow");
    for (std::size_t byte = 4; byte < 10; ++byte) check(narrow[byte] == 0, "a shrunk stream leaves no stale index");
    std::array<std::uint8_t, 4> clipped{};
    write_index_bytes(topology, 2, 2, clipped.data());
    check(clipped[0] == 1 && clipped[2] == 2, "indices past the capacity are not written");
}

void the_flag_turns_the_merge_off_only_with_off() {
    check(!merge_additive_from_argument("off"), "off turns the merge off");
    check(merge_additive_from_argument("on"), "on keeps the merge on");
    check(merge_additive_from_argument(""), "a bare flag keeps the merge on");
    check(merge_additive_from_argument("--eawr-perf-no-render-timing"), "the next option is not a value");
    check(merge_additive_from_argument("OFF"), "only the exact value off counts");
}
} // namespace

int main() {
    streams_are_laid_end_to_end();
    index_bytes_pad_with_degenerate_slots();
    the_flag_turns_the_merge_off_only_with_off();
    return EXIT_SUCCESS;
}
