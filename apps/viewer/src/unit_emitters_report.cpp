#include "unit_emitters.hpp"

#include "eawr/presentation/animation/animation.hpp"
#include "eawr/presentation/particles/map_attachment_owner.hpp"
#include "eawr/presentation/particles/map_effect_plan.hpp"
#include "eawr/presentation/particles/prewarmed_capacity.hpp"
#include "eawr/presentation/particles/proxy_binding.hpp"
#include "eawr/presentation/space/debris.hpp"
#include "eawr/presentation/space/live_units.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <utility>

#include "unit_emitters_internal.hpp"

namespace eawr::presentation::godot_backend {
using namespace godot;
namespace tactical = sim::tactical;
using namespace unit_emitters_detail;

namespace {

[[nodiscard]] std::string json(const std::string_view text) {
    std::string result{"\""};
    for (const char character : text) {
        if (character == '"' || character == '\\') {
            result += '\\';
            result += character;
        } else if (static_cast<unsigned char>(character) < 0x20U) {
            result += ' ';
        } else {
            result += character;
        }
    }
    return result + "\"";
}

void counts(std::ostream& output, const char* name, const std::map<std::string, std::uint64_t>& values,
            const char* separator = ", ") {
    output << separator << "\"" << name << "\": {";
    bool first = true;
    for (const auto& [key, count] : values) {
        output << (first ? "" : ", ") << json(key) << ": " << count;
        first = false;
    }
    output << "}";
}
} // namespace

void UnitEmitters::release() {
    if (released_) return;
    // Reports are written after teardown. Preserve the last live state before releasing handles.
    emitting_at_release_ = emitting_units();
    released_ = true;
    for (Ship& ship : ships_) {
        for (const Running& running : ship.running) static_cast<void>(registry_->release(running.handle));
        ship.running.clear();
        orphan_clone(ship);
    }
    for (CloneProxy& proxy : clone_orphans_) {
        if (proxy.life) static_cast<void>(proxy.life->release_all());
    }
    clone_orphans_.clear();
}

UnitEmitters::Emitting UnitEmitters::emitting_units() const {
    Emitting emitting;
    for (const Ship& ship : ships_) {
        for (const Running& effect : ship.running) {
            if (effect.draining) continue;
            const auto wanted = std::find_if(ship.wanted.begin(), ship.wanted.end(),
                [&](const Wanted& entry) { return entry.proxy == effect.proxy; });
            if (wanted != ship.wanted.end()) ++emitting[ship.entity][wanted->proxy_name];
        }
    }
    return emitting;
}

void UnitEmitters::write_report(std::ostream& output) const {
    std::uint64_t running = 0;
    for (const Ship& ship : ships_) running += ship.running.size();
    output << "  \"unit_emitters\": {\"frames\": " << frames_count_ << ", \"samples\": " << samples_
           << ", \"ships\": " << ships_.size() << ", \"plans\": " << plans_ << ", \"running\": " << running
           << ", \"max_running\": " << max_running_ << ", \"max_particles\": " << max_particles_;
    counts(output, "started", started_);
    output << ", \"invulnerability_drains\": {\"started\": " << invulnerability_drains_started_
           << ", \"finished\": " << invulnerability_drains_finished_ << ", \"cut_short\": "
           << invulnerability_drains_cut_short_ << "}";
    const Emitting units = released_ ? emitting_at_release_ : emitting_units();
    std::map<std::string, std::uint64_t> emitting;
    for (const auto& [unit, proxies] : units) {
        static_cast<void>(unit);
        for (const auto& [proxy, count] : proxies) emitting[proxy] += count;
    }
    counts(output, "emitting", emitting);
    output << ", \"emitting_by_unit\": {";
    bool first_unit = true;
    for (const auto& [unit, proxies] : units) {
        counts(output, std::to_string(unit).c_str(), proxies, first_unit ? "" : ", ");
        first_unit = false;
    }
    output << "}";
    counts(output, "stopped", stopped_);
    counts(output, "not_admitted", not_admitted_);
    counts(output, "start_failed", start_failed_);
    output << ", \"engine_drains\": {\"started\": " << engine_drains_started_ << ", \"finished\": "
           << engine_drains_finished_ << ", \"cut_short\": " << engine_drains_cut_short_ << "}";
    output << ", \"ion_stun_drains\": {\"started\": " << ion_stun_drains_started_ << ", \"finished\": "
           << ion_stun_drains_finished_ << ", \"cut_short\": " << ion_stun_drains_cut_short_ << "}";
    output << ", \"ion_stun_population\": {\"max_particles\": " << ion_stun_max_particles_
           << ", \"dropped_at_capacity\": " << ion_stun_dropped_at_capacity_ << "}";
    output << ", \"power_to_weapons_drains\": {\"started\": " << power_to_weapons_drains_started_
           << ", \"finished\": " << power_to_weapons_drains_finished_ << ", \"cut_short\": "
           << power_to_weapons_drains_cut_short_ << "}";
    output << ", \"caught_up\": " << caught_up_ << ", \"unknown_samples\": " << unknown_samples_
           << ", \"presented_frames\": " << presented_frames_ << ", \"presented_effects\": " << presented_effects_;
    output << ", \"start_log\": [";
    for (std::size_t index = 0; index < start_log_.size(); ++index) {
        const StartRow& row = start_log_[index];
        output << (index ? ", " : "") << "{\"unit\": " << row.entity << ", \"proxy\": " << json(row.proxy)
               << ", \"tick\": " << row.tick << ", \"born\": " << row.born << ", \"origin\": ";
        if (row.origin) {
            output << "[" << (*row.origin)[0] << ", " << (*row.origin)[1] << ", " << (*row.origin)[2] << "]";
        } else {
            output << "null";
        }
        output << ", \"first_age\": " << (row.first_age ? std::to_string(*row.first_age) : std::string("null"))
               << ", \"presented\": " << row.presented << "}";
    }
    output << "], \"start_log_full\": " << (start_log_.size() >= start_log_limit ? "true" : "false");
    output << ", \"engine_brightness\": {";
    bool first = true;
    for (const auto& [entity, brightness] : engine_brightness_) {
        output << (first ? "" : ", ") << "\"" << entity << "\": " << brightness;
        first = false;
    }
    output << "}";
    // #421: the death clones' own proxies.
    std::uint64_t clone_live = 0;
    for (const Ship& ship : ships_) {
        for (const CloneProxy& proxy : ship.clone) {
            if (proxy.life) clone_live += proxy.life->draining().size() + (proxy.life->active() ? 1U : 0U);
        }
    }
    for (const CloneProxy& proxy : clone_orphans_) {
        if (proxy.life) clone_live += proxy.life->draining().size() + (proxy.life->active() ? 1U : 0U);
    }
    output << ", \"death_clones\": {\"ships\": " << clone_ships_ << ", \"live\": " << clone_live
           << ", \"max_live\": " << clone_max_live_ << ", \"orphans\": " << clone_orphans_.size()
           << ", \"max_particles\": " << clone_max_particles_
           << ", \"dropped_at_capacity\": " << clone_dropped_at_capacity_
           << ", \"drains_released\": " << clone_drains_released_
           << ", \"drains_cut_short\": " << clone_drains_cut_short_ << ", \"drains_reset\": " << clone_drains_reset_
           << ", \"reappearance\": " << json(particles::to_string(clone_reappearance));
    counts(output, "started", clone_started_);
    counts(output, "hidden", clone_hidden_);
    counts(output, "not_run", clone_not_run_);
    counts(output, "failed", clone_failed_);
    output << ", \"start_log\": [";
    for (std::size_t index = 0; index < clone_log_.size(); ++index) {
        const CloneStartRow& row = clone_log_[index];
        output << (index ? ", " : "") << "{\"unit\": " << row.entity << ", \"proxy\": " << json(row.proxy)
               << ", \"tick\": " << row.tick << ", \"born\": " << row.born << "}";
    }
    output << "], \"start_log_full\": " << (clone_log_.size() >= start_log_limit ? "true" : "false") << "}";
    output << ", \"failure\": " << (failure_.empty() ? std::string("null") : json(failure_)) << "},\n";
}

} // namespace eawr::presentation::godot_backend
