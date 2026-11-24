// Contracts of the perf diagnostics core (#8058, P1): rings, phase ids, markers, hitch detectors, pattern analyzer and the
// verdict table. Synthetic series only, no game data and no wall clock: every input is injected.

#include "eawr/core/perf_diag/hitch.hpp"
#include "eawr/core/perf_diag/marker.hpp"
#include "eawr/core/perf_diag/pattern.hpp"
#include "eawr/core/perf_diag/phase.hpp"
#include "eawr/core/perf_diag/ring.hpp"
#include "eawr/core/perf_diag/sim_feed.hpp"
#include "eawr/core/perf_diag/verdict.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <new>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

// Counts global allocations while armed: the steady-state hot paths must allocate nothing.
std::atomic<bool> counting{false};
std::atomic<std::uint64_t> allocations{0};

} // namespace

namespace {

// Out of line on purpose: GCC 14 flags std::free reached through an inlined replacement operator delete as a mismatched
// new/delete pair even though the replacement operator new below allocates with std::malloc.
#if defined(__GNUC__)
__attribute__((noinline))
#endif
void release_memory(void* const memory) noexcept {
    std::free(memory);
}

} // namespace

void* operator new(const std::size_t size) {
    if (counting.load(std::memory_order_relaxed)) allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* const memory = std::malloc(size == 0 ? 1 : size)) return memory;
    throw std::bad_alloc();
}
void operator delete(void* const memory) noexcept { release_memory(memory); }
void operator delete(void* const memory, std::size_t) noexcept { ::operator delete(memory); }

namespace {

using namespace eawr::core::perf_diag;

int failures = 0;

void expect(const bool condition, const std::string& message) {
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

struct Pair final {
    std::uint64_t a{};
    std::uint64_t b{};
};

void ring_contracts() {
    SpscRing<Pair> ring(8);
    expect(ring.capacity() == 8, "capacity rounds to a power of two");
    Pair out;
    expect(!ring.pop(out), "empty ring pops nothing");
    for (std::uint64_t i = 0; i < 5; ++i) ring.push({i, ~i});
    for (std::uint64_t i = 0; i < 5; ++i) {
        expect(ring.pop(out) && out.a == i && out.b == ~i, "records come out in order");
    }
    expect(!ring.pop(out) && ring.dropped() == 0, "no drops below capacity");

    // Overwrite-oldest: 20 pushes into 8 slots keep the newest 8 and count 12 dropped.
    for (std::uint64_t i = 100; i < 120; ++i) ring.push({i, ~i});
    std::uint64_t first = 0, count = 0, last = 0;
    while (ring.pop(out)) {
        if (count == 0) first = out.a;
        last = out.a;
        ++count;
    }
    expect(count == 8 && first == 112 && last == 119, "the newest capacity records survive an overflow");
    expect(ring.dropped() == 12, "overflow is counted: " + std::to_string(ring.dropped()));
    expect(SpscRing<Pair>(3).capacity() == 4 && SpscRing<Pair>(1).capacity() == 2, "capacity rounding");

    // One producer, one consumer, a tiny ring so the producer laps the consumer: no torn record, strictly increasing,
    // and delivered + dropped equals pushed.
    SpscRing<Pair> shared(16);
    constexpr std::uint64_t total = 200000;
    std::atomic<bool> done{false};
    std::thread producer([&] {
        for (std::uint64_t i = 1; i <= total; ++i) shared.push({i, ~i});
        done.store(true, std::memory_order_release);
    });
    std::uint64_t delivered = 0, previous = 0;
    bool torn = false, ordered = true;
    for (;;) {
        const bool finished = done.load(std::memory_order_acquire);
        Pair item;
        while (shared.pop(item)) {
            ++delivered;
            if (item.b != ~item.a) torn = true;
            if (item.a <= previous) ordered = false;
            previous = item.a;
        }
        if (finished) break;
    }
    producer.join();
    expect(!torn, "a lapped consumer never sees a torn record");
    expect(ordered, "records stay in push order");
    expect(delivered + shared.dropped() == total, "delivered + dropped == pushed");
}

void phase_contracts() {
    PhaseTable<4> table;
    const PhaseId step = table.intern("step");
    const PhaseId fog = table.intern("fog");
    expect(step == 0 && fog == 1 && table.intern("step") == step, "interning is stable");
    expect(table.name(fog) == "fog" && table.name(9).empty(), "name lookup");
    (void)table.intern("a");
    (void)table.intern("b");
    expect(table.intern("overflow") == no_phase && table.size() == 4, "a full table refuses new names");

    SimFeed feed;
    std::vector<std::string> names;
    for (int i = 0; i < 384; ++i) names.push_back("long phase name beyond small string storage " + std::to_string(i));
    for (std::size_t i = 0; i < 256; ++i) (void)feed.intern(names[i]);
    const auto known = feed.intern(names[0]);
    allocations.store(0, std::memory_order_relaxed);
    counting.store(true, std::memory_order_relaxed);
    bool refused = true;
    for (std::size_t i = 256; i < names.size(); ++i) refused &= feed.intern(names[i]) == no_phase;
    const auto still_known = feed.intern(names[0]);
    const auto static_overflow = feed.intern_static("another phase beyond capacity");
    counting.store(false, std::memory_order_relaxed);
    expect(refused && static_overflow == no_phase && feed.phase_count() == 256 && still_known == known &&
               feed.phase_name(known) == names[0], "full feed retains stable ids and refuses new names");
    expect(allocations.load(std::memory_order_relaxed) == 0, "interning beyond the cap allocates nothing");
    expect(feed.names_dropped() == 129 && feed.dropped() == 129, "overflowed phase names are counted");
}

void marker_contracts() {
    SpscRing<Marker> ring(8);
    Marker out;
    MarkerSink::bind_thread(&ring);
    MarkerSink::set_enabled(false);
    MarkerSink::emit(MarkerKind::save, 5);
    expect(!ring.pop(out), "disabled emit writes nothing");
    MarkerSink::set_enabled(true);
    MarkerSink::emit(MarkerKind::galactic_day, 1350, 3);
    MarkerSink::emit(MarkerKind::pipeline_compile, 77, 0, true);
    expect(ring.pop(out) && out.kind == MarkerKind::galactic_day && out.tick == 1350 && out.value == 3 && out.frame_side == 0,
           "enabled emit pushes a record");
    expect(ring.pop(out) && out.kind == MarkerKind::pipeline_compile && out.frame_side == 1, "frame side flag");
    MarkerSink::bind_thread(nullptr);
    MarkerSink::emit(MarkerKind::save, 6);
    MarkerSink::set_enabled(false);
    expect(!ring.pop(out), "no bound ring, no write");
    expect(marker_name(MarkerKind::lua_collect) == "lua.collect" && marker_bit(MarkerKind::autosave) == 2, "marker names and bits");
}

void median_contracts() {
    RollingMedian<16> window;
    std::mt19937 rng(7);
    std::vector<float> all;
    bool same = true;
    for (int i = 0; i < 200; ++i) {
        const float value = static_cast<float>(rng() % 1000) / 10.0F;
        window.push(value);
        all.push_back(value);
        const std::size_t keep = std::min<std::size_t>(16, all.size());
        std::vector<float> last(all.end() - static_cast<std::ptrdiff_t>(keep), all.end());
        std::sort(last.begin(), last.end());
        if (window.median() != last[last.size() / 2] || window.size() != keep) same = false;
    }
    expect(same, "rolling median equals a brute-force median of the last N");
}

TickObservation tick(const float ms, const float lag = 0.0F, const bool reset = false) { return {ms, lag, 33.0F, reset}; }

void hitch_contracts() {
    TickHitchDetector detector;
    bool early = false;
    for (int i = 0; i < 31; ++i) early = detector.observe(tick(i == 10 ? 50.0F : 3.0F)) || early;
    expect(!early, "no hitch is called before the warm-up");
    for (int i = 0; i < 100; ++i) expect(!detector.observe(tick(3.0F)), "steady ticks are no hitch");
    expect(detector.median_ms() == 3.0F, "median of a steady series");
    expect(!detector.observe(tick(3.9F)), "below the 4 ms floor and 2x median");
    expect(detector.observe(tick(8.5F)), "median + 5 ms is a hitch");
    expect(!detector.observe(tick(7.9F)), "just under median + 5 ms is not");
    expect(detector.observe(tick(3.0F, 40.0F)), "pacing lag over the interval is a hitch");
    expect(detector.observe(tick(3.0F, 0.0F, true)), "a clock reset is always a hitch");

    TickHitchDetector heavy; // a 30 ms median: the 2x term (60 ms) dominates median + 5
    for (int i = 0; i < 64; ++i) (void)heavy.observe(tick(30.0F));
    expect(!heavy.observe(tick(59.0F)) && heavy.observe(tick(61.0F)), "2x median governs heavy ticks");

    FrameHitchDetector frames;
    for (int i = 0; i < 64; ++i) expect(!frames.observe(6.9F), "steady frames");
    expect(!frames.observe(15.0F) && frames.observe(15.3F), "frame hitch above median + 8.3 ms");
}

void excess_contracts() {
    PhaseTable<8> table;
    const PhaseId step = table.intern("step");
    const PhaseId ai = table.intern("command-source");
    auto tracker = std::make_unique<PhaseExcessTracker<8>>();
    for (int i = 0; i < 64; ++i) {
        const PhaseSample normal[] = {{step, 20.0F}, {ai, 1.0F}};
        tracker->commit(normal);
    }
    const PhaseSample spike[] = {{step, 21.0F}, {ai, 15.0F}};
    const PhaseExcess top = tracker->top_excess(spike);
    expect(top.id == ai && top.excess_ms > 13.9F && top.excess_ms < 14.1F,
           "the spiking phase wins over the biggest phase (step is always biggest)");
    expect(tracker->excess(step, 20.0F) == 0.0F, "no excess on the median");

    // The default tracker holds every PhaseTable id (256): GC-sized phase sets are not dropped; ids beyond it are counted.
    auto wide = std::make_unique<PhaseExcessTracker<>>();
    for (int i = 0; i < 40; ++i) {
        const PhaseSample normal[] = {{200, 2.0F}, {100, 5.0F}};
        wide->commit(normal);
    }
    const PhaseSample high[] = {{200, 12.0F}, {100, 5.5F}};
    const PhaseExcess wide_top = wide->top_excess(high);
    expect(wide_top.id == 200 && wide_top.excess_ms > 9.9F && wide_top.excess_ms < 10.1F && wide->skipped() == 0,
           "ids above 64 are tracked");
    const PhaseSample beyond[] = {{300, 9.0F}};
    (void)wide->top_excess(beyond);
    wide->commit(beyond);
    expect(wide->skipped() == 2, "ids beyond the capacity are counted, not dropped silently: " + std::to_string(wide->skipped()));

    // A phase with too few samples has no excess yet (its median is not meaningful).
    auto fresh = std::make_unique<PhaseExcessTracker<8>>();
    const PhaseSample first[] = {{3, 30.0F}};
    fresh->commit(first);
    expect(fresh->excess(3, 30.0F) == 0.0F, "no excess before the warm-up");
}

// Hitch series helpers. interval_ms: wall ms per tick (33 at 1x).
std::vector<PatternSample> series(const std::vector<std::uint64_t>& ticks, const std::vector<std::uint64_t>& wall_ms,
                                  const std::uint64_t marker_mask = 0, const PhaseId phase = 1) {
    std::vector<PatternSample> out;
    for (std::size_t i = 0; i < ticks.size(); ++i) out.push_back({ticks[i], wall_ms[i] * 1000, phase, 14.0F, marker_mask});
    return out;
}

void pattern_contracts() {
    PatternAnalyzer analyzer;
    PhaseTable<8> names;
    (void)names.intern("step");
    (void)names.intern("command-source");
    const auto name_of = [&](const PhaseId id) { return names.name(id); };

    // 300-tick spikes at a constant 1x: tick- and wall-locked at once, the readout says so.
    std::vector<std::uint64_t> ticks, wall;
    for (std::uint64_t k = 1; k <= 20; ++k) { ticks.push_back(k * 300 + 7); wall.push_back((k * 300 + 7) * 33); }
    PatternReadout r = analyzer.analyze(series(ticks, wall, marker_bit(MarkerKind::ai_perception)));
    expect(r.kind == PatternKind::tick_locked && r.period == 300 && r.locked == 20 && r.speed_ambiguous,
           "300-tick spikes at 1x: tick-locked 300, speed ambiguous");
    expect(r.cadence_hint != 0 && cadence_table[r.cadence_hint - 1].period_ticks == 300, "300 ticks matches the cadence table");
    expect(r.phase_id == 1 && r.phase_count == 20 && r.has_marker && r.marker == MarkerKind::ai_perception && r.marker_count == 20,
           "dominant phase and marker match");
    const std::string line = format_pattern(r, name_of);
    expect(line.find("every 300 ticks (20/20, tick-locked") != std::string::npos
               && line.find("command-source +14.0 ms") != std::string::npos
               && line.find("marker gc.ai.perception 20/20") != std::string::npos,
           "readout line: " + line);

    // Tick-locked across a speed change (1x then 4x): wall spacing changes, tick spacing does not.
    ticks.clear(); wall.clear();
    std::uint64_t now_ms = 0;
    for (std::uint64_t k = 1; k <= 24; ++k) {
        const std::uint64_t per_tick = k <= 12 ? 33 : 8;
        now_ms += 300 * per_tick;
        ticks.push_back(k * 300);
        wall.push_back(now_ms);
    }
    r = analyzer.analyze(series(ticks, wall));
    expect(r.kind == PatternKind::tick_locked && r.period == 300 && !r.speed_ambiguous, "tick-locked cause survives a speed change");

    // Wall-locked 5 s spikes while the speed alternates between 1x and 4x: tick gaps vary, wall gaps do not.
    ticks.clear(); wall.clear();
    std::uint64_t tick_now = 0;
    for (std::uint64_t k = 1; k <= 24; ++k) {
        tick_now += (k % 2 == 0) ? 613 : 157;
        ticks.push_back(tick_now);
        wall.push_back(k * 5000);
    }
    r = analyzer.analyze(series(ticks, wall));
    expect(r.kind == PatternKind::wall_locked && r.period >= 4900 && r.period <= 5100, "5 s wall-locked cause at mixed speeds");

    // Wall-locked 5 s at a constant 1x reads as tick-locked 150 with the ambiguity note.
    ticks.clear(); wall.clear();
    for (std::uint64_t k = 1; k <= 20; ++k) { ticks.push_back(k * 150); wall.push_back(k * 5000 - k * 50); }
    r = analyzer.analyze(series(ticks, wall));
    expect(r.kind == PatternKind::tick_locked && r.period == 150 && r.speed_ambiguous, "constant speed: ambiguous, flagged");

    // Two overlapping periods: every 300 ticks plus the 1350-tick fiscal cycle (offset so that it sits off the 300 grid).
    ticks.clear(); wall.clear();
    for (std::uint64_t t = 300; t <= 9000; t += 300) ticks.push_back(t);
    for (std::uint64_t t = 1357; t <= 9000; t += 2700) ticks.push_back(t); // off-grid ones
    std::sort(ticks.begin(), ticks.end());
    for (const std::uint64_t t : ticks) wall.push_back(t * 33);
    r = analyzer.analyze(series(ticks, wall));
    expect(r.kind == PatternKind::tick_locked && r.period == 300, "two periods: the stronger one is reported");

    // Noise: irregular hitch ticks give no pattern.
    std::mt19937 rng(11);
    ticks.clear(); wall.clear();
    std::uint64_t at = 0;
    for (int i = 0; i < 40; ++i) { at += 37 + rng() % 900; ticks.push_back(at); wall.push_back(at * 33); }
    r = analyzer.analyze(series(ticks, wall));
    expect(r.kind == PatternKind::none && !r.allocation_driven, "noise gives no pattern");
    expect(format_pattern(r, name_of).rfind("pattern: none (40 hitches, irregular)", 0) == 0, "no-pattern line");

    // Irregular spacing with the lua walk marker on every hitch reads as allocation-driven.
    r = analyzer.analyze(series(ticks, wall, marker_bit(MarkerKind::lua_memory_walk)));
    expect(r.kind == PatternKind::none && r.allocation_driven && r.marker == MarkerKind::lua_memory_walk, "allocation-driven readout");

    expect(analyzer.analyze({}).kind == PatternKind::none, "empty input");

    // A single spike is no pattern.
    r = analyzer.analyze(series({4200}, {138600}));
    expect(r.kind == PatternKind::none && r.hitches == 1, "single spike");
    expect(format_pattern(r, name_of).rfind("pattern: none (1 hitches, irregular)", 0) == 0, "single spike line");

    // Exactly five regular hitches (four equal gaps) is the smallest series that can be a pattern.
    r = analyzer.analyze(series({300, 600, 900, 1200, 1500}, {9900, 19800, 29700, 39600, 49500}));
    expect(r.kind == PatternKind::tick_locked && r.period == 300 && r.locked == 5, "five regular hitches");

    // Jitter: the fold window is +-1 tick, the gap bin +-2 ticks (or 3 %). One tick of wobble still reads as tick-locked;
    // wobble well beyond the window reads as none instead of a wrong period.
    std::mt19937 jitter(5);
    // -range..+range, signed: the generator's unsigned result must not wrap when it is shifted down.
    const auto wobble = [&](const int range) { return static_cast<std::int64_t>(jitter() % static_cast<unsigned>(2 * range + 1)) - range; };
    ticks.clear(); wall.clear();
    for (std::uint64_t k = 1; k <= 20; ++k) {
        const std::uint64_t t = k * 300 + 7 + static_cast<std::uint64_t>(wobble(1));
        ticks.push_back(t); wall.push_back(t * 33);
    }
    r = analyzer.analyze(series(ticks, wall));
    expect(r.kind == PatternKind::tick_locked && r.period == 300 && r.locked == 20, "+-1 tick jitter stays tick-locked");
    ticks.clear(); wall.clear();
    for (std::uint64_t k = 1; k <= 20; ++k) {
        const std::uint64_t t = k * 300 + 7 + static_cast<std::uint64_t>(wobble(30)); // +-30 ticks
        ticks.push_back(t); wall.push_back(t * 33);
    }
    r = analyzer.analyze(series(ticks, wall));
    expect(r.kind == PatternKind::none, "wobble beyond the fold window is no pattern");

    // Wall-clock jitter of +-2 % on a 5 s wall-locked cause (mixed speeds) is inside the gap bin.
    ticks.clear(); wall.clear();
    tick_now = 0;
    for (std::uint64_t k = 1; k <= 24; ++k) {
        tick_now += (k % 2 == 0) ? 613 : 157;
        ticks.push_back(tick_now);
        wall.push_back(static_cast<std::uint64_t>(static_cast<std::int64_t>(k * 5000) + wobble(30)));
    }
    r = analyzer.analyze(series(ticks, wall));
    expect(r.kind == PatternKind::wall_locked, "small wall jitter stays wall-locked");
    const std::vector<PatternSample> few = series({100, 400, 700, 1000}, {3300, 13200, 23100, 33000});
    expect(analyzer.analyze(few).kind == PatternKind::none, "four hitches are too few");
}

WindowStats base() {
    WindowStats s;
    s.delivered_ratio = 1.0F;
    s.interval_ms = 33.0F;
    s.tick_p95_ms = 10.0F;
    s.mean_tick_ms = 8.0F;
    s.sim_cpu = 0.9F;
    s.frame_budget_ms = 16.7F;
    s.frame_p95_ms = 10.0F;
    s.mean_frame_ms = 8.0F;
    return s;
}

void verdict_contracts() {
    PhaseTable<4> names;
    (void)names.intern("step");
    const PhaseId ai = names.intern("command-source");
    const auto name_of = [&](const PhaseId id) { return names.name(id); };

    WindowStats s = base();
    s.idle = true;
    expect(evaluate_verdict(s).kind == VerdictKind::idle && format_verdict(evaluate_verdict(s), name_of) == "idle", "rule 0 idle");

    s = base();
    expect(evaluate_verdict(s).kind == VerdictKind::ok && format_verdict(evaluate_verdict(s), name_of) == "ok", "rule 1 ok");

    s = base(); // rule 2: slow, loaded, busy on CPU
    s.delivered_ratio = 0.7F; s.tick_p95_ms = 31.0F; s.sim_cpu = 0.95F; s.top_phase = ai; s.top_phase_ms = 31.0F;
    Verdict v = evaluate_verdict(s);
    expect(v.kind == VerdictKind::sim_bound && v.sim_rule == 2, "rule 2 sim-bound");
    expect(format_verdict(v, name_of) == "sim-bound: command-source 31.0 ms", "rule 2 line: " + format_verdict(v, name_of));

    s = base(); // rule 3: waits on the session lock
    s.delivered_ratio = 0.7F; s.tick_p95_ms = 20.0F; s.sim_cpu = 0.7F; s.session_lock_ms = 4.0F; s.mean_tick_ms = 10.0F;
    v = evaluate_verdict(s);
    expect(v.sim_rule == 3 && v.kind == VerdictKind::coupled, "rule 3 coupled by lock");
    expect(format_verdict(v, name_of) == "coupled: sim waits on frame (session lock 4.0 ms)", "rule 3 line");

    s = base(); // rule 4: starved while others use the CPU
    s.delivered_ratio = 0.7F; s.tick_p95_ms = 20.0F; s.sim_cpu = 0.4F; s.others_cpu = 0.31F;
    v = evaluate_verdict(s);
    expect(v.sim_rule == 4 && v.kind == VerdictKind::external, "rule 4 external");
    expect(format_verdict(v, name_of) == "external: other processes 31 %", "rule 4 line: " + format_verdict(v, name_of));

    s = base(); // rule 5: starved, nobody else to blame
    s.delivered_ratio = 0.7F; s.tick_p95_ms = 20.0F; s.sim_cpu = 0.4F; s.others_cpu = 0.05F;
    v = evaluate_verdict(s);
    expect(v.sim_rule == 5 && v.kind == VerdictKind::coupled, "rule 5 own-thread starvation");

    s = base(); // rule 6: slow, on-CPU share known but neither busy nor starved, no wait, no external load
    s.delivered_ratio = 0.7F; s.tick_p95_ms = 40.0F; s.sim_cpu = 0.7F;
    v = evaluate_verdict(s);
    expect(v.sim_rule == 6 && v.kind == VerdictKind::sim_bound && format_verdict(v, name_of) == "sim-bound (unattributed)",
           "rule 6 unattributed");

    s = base(); // unknown on-CPU share: no sim cause is claimed
    s.delivered_ratio = 0.7F; s.tick_p95_ms = 40.0F; s.sim_cpu = -1.0F;
    v = evaluate_verdict(s);
    expect(v.kind == VerdictKind::unknown && v.sim_unknown && v.sim_rule == 0
               && format_verdict(v, name_of) == "unknown: sim slow, on-CPU share not measured",
           "unknown on-CPU share gives an unknown verdict, not sim-bound: " + format_verdict(v, name_of));
    s.frame_p95_ms = 24.0F; s.gpu_ms = 21.0F; // the frame side is still diagnosed
    v = evaluate_verdict(s);
    expect(v.kind == VerdictKind::render_bound && v.frame_rule == 7
               && format_verdict(v, name_of) == "render-bound: GPU 21.0 ms; sim slow, cause unknown (on-CPU share not measured)",
           "unknown sim plus a frame verdict: " + format_verdict(v, name_of));
    s = base(); // a measured lock wait still explains a slow sim without the on-CPU share
    s.delivered_ratio = 0.7F; s.tick_p95_ms = 40.0F; s.sim_cpu = -1.0F; s.session_lock_ms = 4.0F; s.mean_tick_ms = 10.0F;
    expect(evaluate_verdict(s).sim_rule == 3, "rule 3 needs no on-CPU share");

    s = base(); // rule 7: GPU
    s.frame_p95_ms = 24.0F; s.gpu_ms = 21.0F; s.render_cpu_ms = 5.0F;
    v = evaluate_verdict(s);
    expect(v.frame_rule == 7 && v.kind == VerdictKind::render_bound && format_verdict(v, name_of) == "render-bound: GPU 21.0 ms",
           "rule 7 GPU");

    s = base(); // rule 8: render thread
    s.frame_p95_ms = 24.0F; s.gpu_ms = 6.0F; s.render_cpu_ms = 22.0F;
    v = evaluate_verdict(s);
    expect(v.frame_rule == 8 && format_verdict(v, name_of) == "render-bound: render thread 22.0 ms", "rule 8 render thread");

    s = base(); // rule 9: the frame waits on the session lock
    s.frame_p95_ms = 24.0F; s.mean_frame_ms = 20.0F; s.ui_lock_ms = 6.0F; s.gpu_ms = 5.0F; s.render_cpu_ms = 5.0F;
    v = evaluate_verdict(s);
    expect(v.frame_rule == 9 && v.kind == VerdictKind::coupled
               && format_verdict(v, name_of) == "coupled: frame waits on sim (lock 6.0 ms)", "rule 9 frame waits on sim");

    s = base(); // rule 10: main thread
    s.frame_p95_ms = 24.0F; s.gpu_ms = 5.0F; s.render_cpu_ms = 5.0F; s.main_ms = 20.0F; s.main_viewer_ms = 12.0F;
    v = evaluate_verdict(s);
    expect(v.frame_rule == 10 && format_verdict(v, name_of) == "render-bound: main thread (viewer 12.0 ms, engine 8.0 ms)",
           "rule 10 main thread: " + format_verdict(v, name_of));

    // Unknown GPU / render-thread numbers never match their rules.
    s = base();
    s.frame_p95_ms = 24.0F;
    expect(evaluate_verdict(s).frame_rule == 10, "unmeasured GPU and render thread fall through to the main thread");

    // Both slow: independent unless coupled evidence exists.
    s = base();
    s.delivered_ratio = 0.7F; s.tick_p95_ms = 31.0F; s.sim_cpu = 0.95F; s.top_phase = ai; s.top_phase_ms = 31.0F;
    s.frame_p95_ms = 24.0F; s.gpu_ms = 21.0F;
    v = evaluate_verdict(s);
    expect(v.kind == VerdictKind::both_independent && v.sim_rule == 2 && v.frame_rule == 7, "independent sim and render");
    expect(format_verdict(v, name_of).rfind("sim-bound + render-bound (independent)", 0) == 0, "independent line");
    s.cost_correlation = 0.8F;
    expect(evaluate_verdict(s).kind == VerdictKind::coupled, "correlated series are coupled");
    s.cost_correlation = 0.0F;
    s.hitch_coincidence = 0.7F;
    expect(evaluate_verdict(s).kind == VerdictKind::coupled, "coinciding hitches are coupled");

    // Stale frames are the visible consequence of a sim verdict, never a render verdict.
    s = base();
    s.delivered_ratio = 0.7F; s.tick_p95_ms = 31.0F; s.sim_cpu = 0.95F; s.stale_share = 0.25F;
    v = evaluate_verdict(s);
    expect(v.picture_holds && v.kind == VerdictKind::sim_bound && format_verdict(v, name_of).find("(picture holds)") != std::string::npos,
           "stale frames with a sim verdict");
    s = base();
    s.stale_share = 0.25F;
    expect(!evaluate_verdict(s).picture_holds, "no picture-holds note without a sim verdict");
}

void steady_state_allocations() {
    SpscRing<Pair> ring(64);
    SpscRing<Marker> marker_ring(64);
    PhaseTable<8> table;
    const PhaseId a = table.intern("a");
    TickHitchDetector ticks;
    FrameHitchDetector frames;
    auto tracker = std::make_unique<PhaseExcessTracker<8>>();
    HitchLog<256> log;
    PatternAnalyzer analyzer;
    std::vector<PatternSample> input = series({300, 600, 900, 1200, 1500, 1800}, {1, 2, 3, 4, 5, 6});
    MarkerSink::bind_thread(&marker_ring);
    MarkerSink::set_enabled(true);
    // warm-up
    for (int i = 0; i < 300; ++i) { (void)ticks.observe(tick(3.0F)); (void)frames.observe(6.0F); }

    allocations.store(0);
    counting.store(true);
    Pair out;
    for (std::uint64_t i = 0; i < 1000; ++i) {
        ring.push({i, ~i});
        (void)ring.pop(out);
        MarkerSink::emit(MarkerKind::fog, i);
        (void)ticks.observe(tick(3.0F + static_cast<float>(i % 7)));
        (void)frames.observe(6.0F + static_cast<float>(i % 5));
        const PhaseSample samples[] = {{a, static_cast<float>(i % 9)}};
        (void)tracker->top_excess(samples);
        tracker->commit(samples);
        (void)table.intern("a");
        log.add(HitchRecord{});
    }
    (void)analyzer.analyze(input);
    counting.store(false);
    MarkerSink::set_enabled(false);
    MarkerSink::bind_thread(nullptr);
    expect(allocations.load() == 0, "steady state allocates nothing: " + std::to_string(allocations.load()));
}

void log_contracts() {
    HitchLog<4> log;
    for (std::uint64_t i = 1; i <= 6; ++i) { HitchRecord r; r.number = i; log.add(r); }
    expect(log.size() == 4 && log.total() == 6 && log.recent(0).number == 6 && log.recent(3).number == 3, "hitch log keeps the newest");
}

// The sim-thread feed (#8058, P2): off holds nothing and records nothing; on records; the producer path allocates nothing
// once names are interned; the thread-CPU read is sane.
void simulation_feed_contracts() {
    {
        SimFeed feed;
        counting.store(true);
        allocations.store(0);
        for (std::uint64_t i = 0; i < 1000; ++i) {
            feed.mark(MarkerKind::save, i);
            feed.push_tick(TickRecord{});
            feed.push_phase(i, 0, 1.0);
        }
        counting.store(false);
        TickRecord tick;
        expect(!feed.enabled() && !feed.allocated(), "a fresh feed is off and holds no ring");
        expect(allocations.load() == 0, "off allocates nothing");
        expect(!feed.pop_tick(tick) && feed.ticks_pushed() == 0 && feed.markers_pushed() == 0 && feed.phases_pushed() == 0,
            "off records nothing");
    }
    SimFeed feed;
    feed.set_enabled(true);
    expect(feed.enabled() && feed.allocated() && !feed.fine(), "enabling allocates the rings; fine stays off");
    feed.set_fine(true);
    expect(feed.fine(), "fine needs the switch and the feed");
    feed.set_enabled(false);
    expect(!feed.fine(), "fine does not count while the feed is off");
    feed.set_enabled(true);
    {
        const std::string temporary = std::string("worker.") + "some-barrier";
        const PhaseId id = feed.intern(temporary);
        const PhaseId again = feed.intern("worker.some-barrier");
        const PhaseId literal = feed.intern_static("step");
        expect(id == again && id != no_phase && feed.phase_name(id) == "worker.some-barrier", "interning copies and is stable");
        expect(feed.intern("step") == literal, "a copied name finds the static one");
    }
    const PhaseId phase = feed.intern("a-phase");
    counting.store(true);
    allocations.store(0);
    for (std::uint64_t i = 0; i < 10000; ++i) {
        feed.mark(MarkerKind::clock_reset, i, 3);
        feed.push_phase(i, phase, 0.5);
        TickRecord record;
        record.tick = i;
        record.cpu_ratio = thread_cpu_ratio(thread_cpu_now(), thread_cpu_now());
        feed.push_tick(record);
        (void)feed.intern("a-phase");
    }
    counting.store(false);
    expect(allocations.load() == 0, "the enabled producer path allocates nothing after warm-up: " + std::to_string(allocations.load()));
    TickRecord tick;
    PhaseRecord record;
    Marker marker;
    std::uint64_t ticks = 0, phases = 0, markers = 0;
    while (feed.pop_tick(tick)) ++ticks;
    while (feed.pop_phase(record)) ++phases;
    while (feed.pop_marker(marker)) { ++markers; expect(marker.kind == MarkerKind::clock_reset && marker.value == 3, "marker round trip"); }
    expect(ticks + feed.dropped() >= 10000 && phases > 0 && markers > 0, "the enabled feed records");

    // thread cpu: a busy loop reads a plausible ratio or unknown, never garbage.
    const ThreadCpuStamp begin = thread_cpu_now();
    volatile std::uint64_t sink = 0;
    for (std::uint64_t i = 0; i < 20000000; ++i) sink = sink + i;
    const float ratio = thread_cpu_ratio(begin, thread_cpu_now());
    expect(ratio == cpu_ratio_unknown || (ratio > 0.0F && ratio <= 4.0F), "thread cpu ratio is unknown or in range: " + std::to_string(ratio));
    expect(thread_cpu_ratio(ThreadCpuStamp{}, ThreadCpuStamp{}) == cpu_ratio_unknown, "invalid stamps read unknown");
    ThreadCpuStamp backwards{10, 10, true};
    expect(thread_cpu_ratio(backwards, ThreadCpuStamp{5, 5, true}) == cpu_ratio_unknown, "a backwards interval reads unknown");
}

} // namespace

int main() {
    ring_contracts();
    phase_contracts();
    marker_contracts();
    median_contracts();
    hitch_contracts();
    excess_contracts();
    pattern_contracts();
    verdict_contracts();
    log_contracts();
    steady_state_allocations();
    simulation_feed_contracts();
    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "perf_diag contracts ok\n";
    return 0;
}
