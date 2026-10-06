#include "eawr/core/load_profile.hpp"
#include "battle_effects.hpp"

#include "eawr/presentation/space/space.hpp"
#include "eawr/scene/scene.hpp"
#include "eawr/skirmish/start.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <limits>
#include <utility>

#include "battle_effects_internal.hpp"

namespace eawr::presentation::godot_backend {
using namespace godot;
namespace tactical = sim::tactical;
using namespace battle_effects_detail;

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
} // namespace

void BattleEffects::release() {
    if (released_) return;
    // The capture report is written after resources are released; retain its last shown count.
    move_feedback_at_release_ = static_cast<std::uint64_t>(std::count_if(effects_.begin(), effects_.end(), [this](const auto& effect) {
        return std::find(move_particles_.begin(), move_particles_.end(), effect.particle) != move_particles_.end();
    }));
    released_ = true;
    for (const LiveEffect& effect : effects_) static_cast<void>(registry_->release(effect.handle));
    effects_.clear();
    energy_owner_effects_.clear();
    weaken_effects_.clear();
    spawned_projectile_types_.clear();
    for (auto& look : hero_beams_) {
        if (look.batch.resource != 0) backend_->destroy_emitter(look.batch.resource);
        look.batch.resource = 0;
        if (look.sparks.resource != 0) backend_->destroy_emitter(look.sparks.resource);
        look.sparks.resource = 0;
        look.active_at_release = look.births.size();
        look.births.clear();
    }
    for (Batch* target : {&kites_, &beams_}) {
        if (target->resource != 0) backend_->destroy_emitter(target->resource);
        target->resource = 0;
    }
}

void BattleEffects::write_report(std::ostream& output) const {
    output << "  \"battle_effects\": {\"frames\": " << frames_ << ", \"projectiles_drawn\": " << projectiles_drawn_
           << ", \"projectiles_hidden\": " << projectiles_hidden_ << ", \"max_kites\": " << max_kites_
           << ", \"kites_head_leading\": " << kites_head_leading_ << ", \"kites_head_trailing\": " << kites_head_trailing_
           << ", \"kite_axis\": {\"samples\": " << kite_axis_samples_ << ", \"max_sine_error\": "
           << kite_axis_max_sine_ << ", \"reversed\": " << kite_axis_reversed_ << "}"
           << ", \"max_beams\": " << max_beams_ << ", \"projectile_models_drawn\": " << models_drawn_ << ", \"live_effects\": " << effects_.size()
           << ", \"max_live_effects\": " << max_live_effects_ << ", \"effects_dropped\": " << effects_dropped_;
    output << ", \"laser_z_scales\": [" << laser_scales_.beam << ',' << laser_scales_.kite << ']'
           << ", \"max_beam_width\": " << max_beam_width_ << ", \"max_kite_width\": " << max_kite_width_;
    output << ", \"laser_depth\": {\"clips\": [" << effect_clips_[0] << ',' << effect_clips_[1] << ']';
    const auto depth_sample = [&](const char* name, const auto& sample) {
        output << ", \"" << name << "\": ";
        if (sample) output << "{\"view_depth\":" << (*sample)[0] << ",\"normalized_depth\":" << (*sample)[1]
            << ",\"factor\":" << (*sample)[2] << ",\"half_width\":" << (*sample)[3] << '}';
        else output << "null";
    };
    depth_sample("first_kite", first_kite_depth_);
    depth_sample("first_beam", first_beam_depth_);
    output << '}';
    output << ", \"energy_beams_drawn\": " << energy_beams_drawn_ << ", \"tractor_beams_drawn\": " << tractor_beams_drawn_;
    output << ", \"hero_beam_looks\": [";
    for (std::size_t index = 0; index < hero_beams_.size(); ++index) {
        const auto& look = hero_beams_[index];
        output << (index ? "," : "") << "{\"width\":" << look.width << ",\"frames\":" << look.frames
            << ",\"colour\":[" << look.colour.x << ',' << look.colour.y << ',' << look.colour.z << ',' << look.colour.w
            << "],\"sparks_drawn\":" << look.sparks_drawn << ",\"moving_samples\":" << look.moving_samples
            << ",\"last_spark_fraction\":" << look.last_spark_fraction
            << ",\"active_sources\":" << (released_ ? look.active_at_release : look.births.size())
            << ",\"last_quad\":[";
        for (std::size_t vertex = 0; vertex < look.last_quad.size(); ++vertex) {
            const auto& point = look.last_quad[vertex];
            output << (vertex ? "," : "") << "[" << point.position.x << ',' << point.position.y << ',' << point.position.z
                << ',' << point.u << ',' << point.v << ']';
        }
        output << "]}";
    }
    output << ']';
    const auto move_active = released_ ? move_feedback_at_release_
        : static_cast<std::uint64_t>(std::count_if(effects_.begin(), effects_.end(), [this](const auto& effect) {
        return std::find(move_particles_.begin(), move_particles_.end(), effect.particle) != move_particles_.end();
    }));
    output << ", \"move_feedback_active\": " << move_active << ", \"move_feedback_scale\": " << move_scale_;
    const auto counts = [&output](const char* name, const std::map<std::string, std::uint64_t>& values) {
        output << ", \"" << name << "\": {";
        bool first = true;
        for (const auto& [key, count] : values) {
            output << (first ? "" : ", ") << json(key) << ": " << count;
            first = false;
        }
        output << "}";
    };
    counts("spawned", spawned_);
    counts("spawn_failed", spawn_failed_);
    counts("expired", expired_);
    const auto point = [&output](const particles::Vec3 value) {
        output << '[' << value.x << ',' << value.y << ',' << value.z << ']';
    };
    output << ", \"contacts\": {\"attached\": " << contact_attached_ << ", \"missing\": " << contact_missing_
           << ", \"hidden\": " << contact_hidden_ << ", \"removed\": " << contact_removed_ << ", \"samples\": [";
    for (std::size_t index = 0; index < contact_samples_.size(); ++index) {
        const auto& sample = contact_samples_[index];
        output << (index ? "," : "") << "{\"sample\":" << sample.sample << ",\"handle\":" << sample.handle
               << ",\"key\":" << json(sample.key) << ",\"age\":" << sample.age << ",\"attached\":"
               << (sample.contact ? "true" : "false") << ",\"owner\":" << (sample.contact ? sample.contact->owner : 0)
               << ",\"bone\":" << (sample.contact ? sample.contact->bone : 0) << ",\"origin\":";
        point(sample.frame.origin);
        output << ",\"bounds\":" << (sample.bounds ? "true" : "false") << ",\"centre\":";
        point(sample.centre);
        output << ",\"detached\":" << (sample.detached ? "true" : "false") << '}';
    }
    output << "]}";
    counts("projectiles_not_drawn", not_drawn_);
    // #862 (AB-66): ability shots fired and their drawn frames, by the projectile type they drew as.
    counts("ability_shots_fired", ability_shots_fired_);
    counts("ability_shot_frames_drawn", ability_shots_drawn_);
    // #456 BP-62: each model projectile pool: its slots, the most bound at once, the flights it
    // took and those it refused (every slot bound or resting).
    output << ", \"projectile_models\": {";
    for (auto pool = model_pools_.begin(); pool != model_pools_.end(); ++pool) {
        output << (pool == model_pools_.begin() ? "" : ", ") << json(pool->first) << ": {\"slots\": "
               << pool->second.slots.size() << ", \"max_bound\": " << pool->second.slots.max_bound()
               << ", \"bindings\": " << pool->second.slots.bindings() << ", \"refused\": "
               << pool->second.slots.refused() << "}";
    }
    output << "}, \"hit_picks\": {\"by_projectile\": " << hit_picks_by_projectile_ << ", \"by_event\": "
           << hit_picks_by_event_ << "}";
    output << ", \"shield_hits\": {\"on_mesh\": " << shield_on_mesh_ << ", \"no_mesh\": " << shield_no_mesh_
           << ", \"mesh_missed\": " << shield_mesh_missed_ << ", \"in_step\": " << shield_in_step_
           << ", \"ahead\": " << shield_ahead_ << ", \"casts\": {\"casts\": " << shield_casts_.casts
           << ", \"sphere_rejects\": " << shield_casts_.sphere_rejects << ", \"chunks_tested\": "
           << shield_casts_.chunks_tested << ", \"triangles_tested\": " << shield_casts_.triangles_tested
           << ", \"max_triangles_per_cast\": " << shield_casts_.max_triangles_per_cast << "}, \"meshes\": {";
    for (auto mesh = shield_meshes_.begin(); mesh != shield_meshes_.end(); ++mesh) {
        output << (mesh == shield_meshes_.begin() ? "" : ", ") << json(mesh->first) << ": {\"shield\": "
               << (mesh->second.first ? "true" : "false") << ", \"triangles\": " << mesh->second.second << "}";
    }
    const auto triple = [](const std::array<double, 3>& value) {
        char text[96];
        std::snprintf(text, sizeof(text), "[%.3f, %.3f, %.3f]", value[0], value[1], value[2]);
        return std::string(text);
    };
    output << "}, \"samples\": [";
    for (std::size_t index = 0; index < shield_samples_.size(); ++index) {
        const ShieldSample& sample = shield_samples_[index];
        output << (index ? ", " : "") << "{\"tick\": " << sample.tick << ", \"target\": " << sample.target
               << ", \"contact\": " << triple(sample.contact) << ", \"placed\": " << triple(sample.placed)
               << ", \"direction\": " << triple(sample.direction) << "}";
    }
    output << "]}";
    output << ", \"hit_events\": {";
    for (auto hit = hit_events_.begin(); hit != hit_events_.end(); ++hit) {
        output << (hit == hit_events_.begin() ? "" : ", ") << "\"" << hit->first << "\": " << hit->second;
    }
    output << "}, \"projectile_model_last_tick\": {";
    for (auto row = model_last_posed_tick_.begin(); row != model_last_posed_tick_.end(); ++row) {
        output << (row == model_last_posed_tick_.begin() ? "" : ", ") << "\"" << row->first << "\": " << row->second;
    }
    output << "}, \"spawn_log\": [";
    for (std::size_t index = 0; index < spawn_log_.size(); ++index) {
        const SpawnRow& row = spawn_log_[index];
        output << (index ? ", " : "") << "[" << row.tick << ", " << json(row.key) << ", "
               << (row.first_age ? std::to_string(*row.first_age) : std::string("null")) << "]";
    }
    output << "], \"spawn_log_full\": " << (spawn_log_.size() >= spawn_log_limit ? "true" : "false");
    output << ", \"particle_types\": {";
    bool first = true;
    for (const auto& [name, type] : particle_types_) {
        output << (first ? "" : ", ") << json(name) << ": {\"model\": " << json(type.model_path)
               << ", \"lifetime_frames\": " << type.lifetime_frames << ", \"ready\": " << (type.system ? "true" : "false")
               << ", \"cause\": " << json(type.cause) << "}";
        first = false;
    }
    output << "}, \"unresolved\": [";
    for (std::size_t index = 0; index < unresolved_.size(); ++index) {
        output << (index ? ", " : "") << json(unresolved_[index]);
    }
    output << "], \"failure\": " << (failure_.empty() ? std::string("null") : json(failure_)) << "},\n";
}

} // namespace eawr::presentation::godot_backend
