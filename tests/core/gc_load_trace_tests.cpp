#include "eawr/core/gc_load_trace.hpp"

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
    return failures == 0 ? 0 : 1;
}
