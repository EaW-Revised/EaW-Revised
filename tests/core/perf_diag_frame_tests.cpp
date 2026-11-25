// Contracts of the frame side records and the frame matcher (#8058, P3). Synthetic series only: every input is injected,
// no wall clock, no engine.

#include "eawr/core/perf_diag/frame.hpp"

#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>

namespace {

int failures = 0;
void check(const bool ok, const char* const what) {
    if (!ok) {
        std::cerr << "FAIL: " << what << '\n';
        ++failures;
    }
}

using namespace eawr::core::perf_diag;

MainFrame main_frame(const std::uint64_t frame, const float ms = 10.0F) {
    MainFrame record{};
    record.frame = frame;
    record.frame_ms = ms;
    return record;
}
RenderSample render_sample(const std::uint64_t tag, const float cpu, const float gpu) {
    RenderSample record{};
    record.tag = tag;
    record.cpu_ms = cpu;
    record.gpu_ms = gpu;
    return record;
}

void joins_by_frame_number_not_arrival() {
    FrameMatcher<64> matcher;
    std::vector<FrameSample> out;
    const auto sink = [&](const FrameSample& sample) { out.push_back(sample); };
    matcher.add_render(render_sample(12, 5.0F, 7.0F), sink);
    matcher.add_render(render_sample(11, 4.0F, 6.0F), sink);
    auto stale = main_frame(10);
    stale.flags = frame_flag_stale;
    matcher.add_main(stale, sink);
    matcher.add_main(main_frame(11), sink);
    check(out.size() == 2 && out[0].main.frame == 10 && out[0].render_cpu_ms == 4.0F,
          "frame 10 receives CPU from its engine tag");
    std::uint64_t stale_count = 0;
    for (const auto& sample : out) count_stale_frame(stale_count, sample.main.flags);
    check(out.size() == 2 && stale_count == 1, "only the flagged frame is counted stale after the join");
    check(out.size() == 2 && !out[0].have_gpu && !out[1].have_gpu,
          "GPU values with unknown source frames are never assigned to frames");
}

void evicts_incomplete_frames_and_counts_them() {
    FrameMatcher<8> matcher;
    std::vector<FrameSample> out;
    const auto sink = [&](const FrameSample& sample) { out.push_back(sample); };
    matcher.add_main(main_frame(1), sink); // no render sample ever comes for 1
    for (std::uint64_t f = 2; f < 20; ++f) matcher.add_main(main_frame(f), sink);
    check(matcher.incomplete() >= 10, "frames that never got render data are counted incomplete");
    bool first_marked = false;
    for (const FrameSample& s : out) if (s.main.frame == 1) first_marked = s.have_main && !s.have_cpu && !s.have_gpu;
    check(first_marked, "an evicted frame carries which parts are missing");
    matcher.add_render(render_sample(3, 1.0F, 1.0F), sink); // describes frame 2 and 1: far behind the window
    check(matcher.late() >= 1, "a sample for a frame already out of the window is counted late");
}

void flush_emits_what_is_waiting() {
    FrameMatcher<16> matcher;
    std::vector<FrameSample> out;
    const auto sink = [&](const FrameSample& sample) { out.push_back(sample); };
    matcher.add_main(main_frame(5), sink);
    matcher.add_main(main_frame(6), sink);
    matcher.flush(sink);
    check(out.size() == 2, "flush emits both waiting frames");
    check(matcher.incomplete() == 2, "and counts them incomplete");
}

void render_ring_carries_samples_between_threads() {
    FrameRings rings(256);
    constexpr std::uint64_t count = 2000;
    std::thread producer([&] {
        for (std::uint64_t i = 1; i <= count; ++i) rings.render.push(render_sample(i, static_cast<float>(i), static_cast<float>(2 * i)));
    });
    std::uint64_t seen = 0, last = 0;
    bool ordered = true, consistent = true;
    RenderSample sample{};
    while (seen + rings.render.dropped() < count) {
        if (!rings.render.pop(sample)) continue;
        ++seen;
        ordered = ordered && sample.tag > last;
        consistent = consistent && sample.cpu_ms == static_cast<float>(sample.tag) && sample.gpu_ms == static_cast<float>(2 * sample.tag);
        last = sample.tag;
    }
    producer.join();
    check(ordered && consistent, "ring records are whole and in order");
    check(seen + rings.render.dropped() == count, "every record is read or counted dropped");
}

} // namespace

int main() {
    joins_by_frame_number_not_arrival();
    evicts_incomplete_frames_and_counts_them();
    flush_emits_what_is_waiting();
    render_ring_carries_samples_between_threads();
    if (failures == 0) std::cout << "perf_diag frame contracts passed\n";
    return failures == 0 ? 0 : 1;
}
