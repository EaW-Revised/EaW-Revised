#pragma once

// Private seams between the tactical AI host, Lua bindings, orders and loading.

#include "ai_engine.hpp"
#include "host.hpp"

#include "eawr/vfs/vfs_fwd.hpp"

namespace eawr::script::foc::tactical_ai_detail {

using HostPtr = std::shared_ptr<detail::Host>;
inline constexpr std::string_view pending_debit = "foc.pending_debit";
inline constexpr std::string_view turn_grant = "foc.turn_grant";
core::Result<void> resolve_commands(detail::Host& host, authoritative::ServiceReport& report);

inline authoritative::RandomStream object_phase_random(std::uint64_t seed, std::uint64_t tick, std::uint64_t instance) {
    return {seed, tick | authoritative::object_phase_random_domain, instance};
}
core::Result<std::string> native_configuration(const AiSetup&);

// SOS-01/45: resolved Lua_Script data admits modules, including mod renames.
// Attachment separately respects existing native ownership until the
// same-turn lifecycle migration; unsupported APIs diagnose when Lua runs.
inline bool admitted_object_script(std::string_view name) {
    return !name.empty();
}

core::Result<void> service_objects(const sim::PartitionExecutor& executor, const HostPtr& host,
    authoritative::ScriptScheduler& scripts, std::uint64_t& sequence,
    std::vector<authoritative::ScriptDiagnostic>& diagnostics, std::uint32_t& active, std::uint32_t& pumps, bool schedule = true);

// The first live battle object whose type is the named one (nil when there is none); Find_First_Object and FindPlanet share it.
core::Result<authoritative::ValueList> first_object_of_type(detail::Host& host, authoritative::BindingContext& context,
    const std::string& name);

authoritative::Binding noop();
// tactical_world_queries=false leaves out the globals that read the tactical battle view (Find_First_Object); a galactic
// scheduler passes false because the galactic freestore registers its own Find_First_Object there.
void register_globals(authoritative::ScriptScheduler& scripts, const HostPtr& host,
    std::vector<core::Diagnostic>& errors, bool install_type_lists = true, bool tactical_world_queries = true);
void register_methods(authoritative::ScriptScheduler& scripts, const HostPtr& host,
    std::vector<core::Diagnostic>& errors, bool install_type_lists = true);
bool battle_bindings_enabled(const HostPtr& host);
void register_battle_distribution(authoritative::ScriptScheduler& scripts, const HostPtr& host,
    std::vector<core::Diagnostic>& errors);
// The object methods only a battle scheduler binds (tactical_object_bindings.cpp); a galactic scheduler gets none.
void register_battle_object_bindings(authoritative::ScriptScheduler& scripts, const HostPtr& host,
    std::vector<core::Diagnostic>& errors);
// The shared staging of a script creation (Create_Generic_Object, Spawn_Unit, Spawn_Special_Weapon): validates, queues one
// ordered, replayed creation command and answers its base reference. `units` also admits a squadron or lone craft type.
core::Result<std::uint64_t> stage_script_creation(const HostPtr& host, authoritative::BindingContext& context, const std::string& verb,
    const AiType* type, const std::optional<sim::math::Vec3>& position, const authoritative::Handle* owner, bool units);
// The type a name string or type handle argument names; null when it names none.
const AiType* named_type(const detail::Host& host, const authoritative::Value& value);
// The presentation verbs only a battle scheduler binds (tactical_presentation_bindings.cpp): story camera, fades, environment.
void register_tactical_presentation(authoritative::ScriptScheduler& scripts, const HostPtr& host,
    std::vector<core::Diagnostic>& errors);
core::Result<authoritative::TacticalOrder> translate_order(const authoritative::ScriptCommand& command);
core::Result<void> load_contrast(detail::Host& host, const std::map<std::string, std::string>& modules);
// The directories load_contrast's scheduler searches.
std::vector<std::string> contrast_script_directories();
// Every module of those directories read from the mounted view. A host that has no module map of its own passes this
// one, so `require` inside PGAICommands resolves whatever the mounted library holds (ML-31, ML-38, L-03a) and no
// fixed module list stands between the scripts and their data.
core::Result<std::map<std::string, std::string>> read_contrast_modules(const vfs::Vfs& files);
core::Result<std::vector<ai::PlanDef>> load_plans(const HostPtr& host,
    const std::map<std::string, std::string>& modules, std::vector<std::string>& notes,
    const std::set<std::string>* selected_goals = nullptr);
ai::ConverterFunction converters(const HostPtr& host);

} // namespace eawr::script::foc::tactical_ai_detail
