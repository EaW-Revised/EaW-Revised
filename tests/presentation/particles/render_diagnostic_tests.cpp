// PF ablation ladder: the --eawr-diagnostic-off switches parse and announce themselves; unknown names are refused.
#include "render_test_support.hpp"

#include "eawr/presentation/diagnostic_switches.hpp"

namespace particle_render_contracts {

namespace ds = eawr::presentation::diagnostic_switches;

void test_diagnostic_switches() {
    ds::Switches parsed{};
    std::string error;
    expect(ds::parse("", parsed, error) && parsed == ds::Switches{} && ds::announce(parsed).empty(),
           "an empty list switches nothing off and announces nothing");
    expect(ds::parse("particles", parsed, error) && parsed.particles && !parsed.turrets && !parsed.skeletons,
           "particles alone");
    expect(ds::announce(parsed) == std::vector<std::string>{"EAWR diagnostic: particles active"},
           "an active switch prints its diagnostic line");
    expect(ds::parse("skeletons,turrets,particles", parsed, error) && parsed.particles && parsed.turrets && parsed.skeletons,
           "the order of a list does not matter");
    const std::vector<std::string> all{"EAWR diagnostic: particles active", "EAWR diagnostic: turrets active",
                                       "EAWR diagnostic: skeletons active"};
    expect(ds::parse("shadows", parsed, error) && parsed.shadows && !parsed.particles
           && ds::announce(parsed) == std::vector<std::string>{"EAWR diagnostic: shadows active"},
           "shadows alone");
    expect(ds::parse("skeletons,turrets,particles", parsed, error), "re-parse the three");
    expect(ds::announce(parsed) == all, "every active switch prints one line, in a fixed order");
    expect(ds::parse(",turrets,,", parsed, error) && !parsed.particles && parsed.turrets && !parsed.skeletons,
           "empty entries are ignored");
    const ds::Switches before = parsed;
    expect(!ds::parse("turrets,bloom", parsed, error) && parsed == before && !error.empty(),
           "an unknown name is refused, the caller's switches stay as they were");
    expect(error.find("bloom") != std::string::npos, "the refusal names the unknown switch");
    expect(!ds::parse("Particles", parsed, error), "names are case-sensitive");
    expect(ds::flag == "--eawr-diagnostic-off", "the flag name is pinned");
    // A repeated flag adds up: the viewer parses each occurrence on its own and merges it.
    ds::Switches merged{};
    ds::Switches first{};
    ds::Switches second{};
    expect(ds::parse("particles", first, error) && ds::parse("turrets,shadows", second, error), "two occurrences parse");
    ds::merge(merged, first);
    ds::merge(merged, second);
    expect(merged.particles && merged.turrets && merged.shadows && !merged.skeletons,
           "a repeated flag keeps the switches of every occurrence");
    expect(ds::announce(merged) == std::vector<std::string>{"EAWR diagnostic: particles active",
               "EAWR diagnostic: turrets active", "EAWR diagnostic: shadows active"},
           "a merged list announces every active switch, shadows last");
    ds::merge(merged, ds::Switches{});
    expect(merged.particles && merged.turrets && merged.shadows, "merging an empty occurrence switches nothing off again");

    // The particles switch makes every registry entry point inert: scalar and batched, advance and present.
    RecordingBackend backend;
    particles::EffectRegistry registry(backend);
    const auto spawned = registry.spawn(mixed_system(), 11, 128);
    expect(bool(spawned), "the inert test effect spawns");
    if (!spawned) return;
    const std::vector<particles::EffectHandle> handles{spawned.value()};
    std::vector<particles::EffectFrameStats> stats;
    const auto camera = test_camera();
    particles::EffectRegistry::set_diagnostic_inert(true);
    const auto scalar = registry.advance(spawned.value(), 1.0F / 30.0F, camera);
    const bool scalar_inert = scalar && scalar.value().hash == 0 && scalar.value().particles == 0;
    const bool present_ok = bool(registry.present(spawned.value(), camera));
    const bool batch_ok = bool(registry.advance_all(handles, 1.0F / 30.0F, camera, stats)) && bool(registry.present_all(handles, camera));
    const bool nothing_done = backend.record.empty() && registry.work().steps == 0 && registry.work().presents == 0;
    particles::EffectRegistry::set_diagnostic_inert(false);
    expect(scalar_inert, "an inert scalar advance answers success with empty statistics");
    expect(present_ok && batch_ok, "an inert present and batch answer success");
    expect(nothing_done, "an inert registry steps, builds and uploads nothing, scalar or batched");
    const auto live = registry.advance(spawned.value(), 1.0F / 30.0F, camera);
    expect(live && !backend.record.empty(), "with the switch off the registry works again");
}

} // namespace particle_render_contracts
