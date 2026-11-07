#include "eawr/core/gc_load_trace.hpp"
#include "eawr/core/content_chain.hpp"

#include <iostream>
#include <string>

namespace {
int failures = 0;
void expect(const bool condition, const char* message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}
bool has(const std::string& line, const char* text) { return line.find(text) != std::string::npos; }
} // namespace

int main() {
    using namespace eawr::core::gc_load;
    expect(!trace.finish("b").has_value(), "no load, no line");
    { const Scope ignored("before"); }
    expect(!trace.finish("b").has_value(), "a span outside a load is dropped");

    trace.begin("mp_host", true);
    expect(trace.active(), "begin activates");
    { const Scope outer("outer"); { const Scope inner("outer.inner"); } }
    trace.mark("handover");
    trace.begin("sp");
    const auto line = trace.finish("build-x");
    expect(line.has_value(), "finish returns the line");
    if (line) {
        expect(has(*line, "[mp_host]"), "a second begin without restart keeps the first kind");
        expect(has(*line, "build=build-x"), "build identity");
        expect(has(*line, " outer@") && has(*line, " outer.inner@") && has(*line, " handover@"), "spans and marks listed");
        expect(line->find('\n') == std::string::npos, "a single line");
    }
    expect(!trace.active() && !trace.finish("b").has_value(), "finish ends the load");

    trace.begin("mp_guest", true);
    trace.begin("mp_host", true);
    const auto restarted = trace.finish("b");
    expect(restarted && has(*restarted, "[mp_host]"), "restart replaces an abandoned load");

    const auto first_origin = Clock::now() - std::chrono::seconds(2);
    trace.begin("sp", true, first_origin);
    trace.span("failed_ready", first_origin, first_origin + std::chrono::milliseconds(10));
    // ready() can fail without reaching the frame that finishes the trace. A new solo scene restarts it.
    const auto next_origin = first_origin + std::chrono::seconds(1);
    trace.begin("sp", true, next_origin);
    trace.span("next_ready", next_origin + std::chrono::milliseconds(5), next_origin + std::chrono::milliseconds(12));
    const auto next_solo = trace.finish("b");
    expect(next_solo && !has(*next_solo, "failed_ready") && has(*next_solo, " next_ready@5.0+7.0"),
        "solo retry discards failed phases and measures from its new origin");

    trace.begin("sp_start", true, first_origin);
    trace.span("gc.attract_stop", first_origin, first_origin + std::chrono::milliseconds(15));
    trace.begin("sp", !trace.kind_is("sp_start"), next_origin);
    const auto anchored = trace.finish("b");
    expect(anchored && has(*anchored, "[sp_start]") && has(*anchored, " gc.attract_stop@0.0+15.0"),
        "scene entry preserves the menu Start origin and teardown span");

    // Equal profile names do not establish equal installed content: a changed
    // game/mod root or precedence chain must choose the fresh-mount path.
    const std::string setup_game = "game-a", setup_mod = "mod-a";
    const std::vector<std::string> setup_layers = {"mod", "expansion", "base"};
    const auto reuse = [&](const std::string& game, const std::string& mod,
                           const std::vector<std::string>& layers) {
        return eawr::core::content_chain_matches(setup_game, setup_mod, "remake", setup_layers,
            game, mod, "remake", layers);
    };
    expect(reuse("game-a", "mod-a", setup_layers), "identical resolved chain reuses setup");
    expect(!reuse("game-b", "mod-a", setup_layers), "same profile with different game root loads fresh");
    expect(!reuse("game-a", "mod-b", setup_layers), "same profile with different mod root loads fresh");
    expect(!reuse("game-a", "mod-a", {"expansion", "mod", "base"}),
        "same profile with different layer order loads fresh");
    expect(!eawr::core::content_chain_matches(setup_game, setup_mod, "remake", setup_layers,
        setup_game, setup_mod, "foc", setup_layers), "different profile loads fresh");

    trace.begin("sp_start", true);
    trace.cancel();
    expect(!trace.active() && !trace.kind_is("sp_start") && !trace.finish("b"), "refused Start drops its origin");
    trace.begin("sp", true);
    trace.span("direct_retry", first_origin, first_origin + std::chrono::milliseconds(1));
    const auto retried = trace.finish("b");
    expect(retried && !has(*retried, "attract_stop") && has(*retried, "direct_retry"),
        "cancelled menu load cannot contaminate a later direct load");
    return failures == 0 ? 0 : 1;
}
