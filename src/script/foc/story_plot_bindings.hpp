#pragma once

// WST-56 (docs/behaviour/walks/gc-story.md): Get_Story_Plot and the plot and event objects it returns, for the Lua
// of the AI plans (the retail interventions edit and switch their story plots). The plot engine itself lives in
// sim/galactic/story_engine; these bindings only validate and stage ordered, plain-data requests, which the AI
// engine commits to it (StoryPlotLink::apply) in command order.

#include "eawr/script/authoritative/scheduler.hpp"
#include "eawr/sim/galactic/story_engine.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::script::foc {

inline constexpr std::uint32_t handle_story_plot = 19;  // id: sub-plot index + 1
inline constexpr std::uint32_t handle_story_event = 20; // id: (sub-plot index + 1) << 32 | (event index + 1)
inline constexpr std::string_view verb_story_plot = "foc.story_plot"; // issuer, op, sub-plot, event, index, tokens...

// What the campaign tells the AI host about the loaded plots: immutable names (so lookups are thread-safe and
// identical on every peer) and the sink that commits a request to the plot engine. Empty where no plots are hosted.
struct StoryPlotLink {
    struct Plot {
        std::string name;                                // the sub-plot file name as written
        std::map<std::string, std::uint32_t> events;     // upper-cased event name -> index
    };
    std::vector<Plot> plots;
    std::map<std::string, std::uint32_t> by_name;        // exact file name -> index
    std::function<core::Result<void>(const sim::galactic::story::PlotCommand&)> apply;

    [[nodiscard]] static std::shared_ptr<StoryPlotLink> from(std::span<const sim::galactic::story::SubPlotDef> definitions,
        std::function<core::Result<void>(const sim::galactic::story::PlotCommand&)> apply);
};

namespace detail {
struct Host;
void register_story_plot_bindings(authoritative::ScriptScheduler&, const std::shared_ptr<Host>&, std::vector<core::Diagnostic>& errors);
// The AI engine's commit of one staged request. Returns an empty string, or why the request was not applied.
[[nodiscard]] std::string commit_story_plot(const Host&, const authoritative::ScriptCommand&);
} // namespace detail

} // namespace eawr::script::foc
