#pragma once

// Private seams between the tactical AI host, Lua bindings, orders and loading.

#include "ai_engine.hpp"
#include "host.hpp"

namespace eawr::script::foc::tactical_ai_detail {

using HostPtr = std::shared_ptr<detail::Host>;
core::Result<std::string> native_configuration(const AiSetup&);

// SOS-01/45: resolved Lua_Script data admits modules, including mod renames.
// Attachment separately respects existing native ownership until the
// same-turn lifecycle migration; unsupported APIs diagnose when Lua runs.
inline bool admitted_object_script(std::string_view name) {
    return !name.empty();
}

core::Result<void> service_objects(const sim::PartitionExecutor& executor, const HostPtr& host,
    authoritative::ScriptScheduler& scripts, std::uint64_t& sequence,
    std::vector<authoritative::ScriptDiagnostic>& diagnostics, std::uint32_t& active, std::uint32_t& pumps);

authoritative::Binding noop();
void register_globals(authoritative::ScriptScheduler& scripts, const HostPtr& host,
    std::vector<core::Diagnostic>& errors);
void register_methods(authoritative::ScriptScheduler& scripts, const HostPtr& host,
    std::vector<core::Diagnostic>& errors);
core::Result<authoritative::TacticalOrder> translate_order(const authoritative::ScriptCommand& command);
core::Result<void> load_contrast(detail::Host& host, const std::map<std::string, std::string>& modules);
core::Result<std::vector<ai::PlanDef>> load_plans(const HostPtr& host,
    const std::map<std::string, std::string>& modules, std::vector<std::string>& notes);
ai::ConverterFunction converters(const HostPtr& host);

} // namespace eawr::script::foc::tactical_ai_detail
