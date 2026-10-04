#include "debris_props.hpp"

#include "eawr/scene/scene.hpp"
#include "eawr/skirmish/start.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>

namespace eawr::presentation::godot_backend {
namespace {

namespace tactical = sim::tactical;

// Every prop's fire and explosion draws from its own capacity.
constexpr std::size_t effect_capacity = 4096;
// A detached effect that never reports finished is released after this many further samples.
constexpr std::uint32_t drain_limit_frames = 300;
// The report keeps this many spawn and expiry rows.
constexpr std::size_t row_limit = 1024;
// Breakoff props are live ships of their own, never session units: their IDs sit far above
// the session's and apart from the death clones' (max - unit).
constexpr sim::EntityId first_prop_entity = std::numeric_limits<sim::EntityId>::max() / 2U;

[[nodiscard]] std::string trim(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
    return std::string(text);
}

[[nodiscard]] std::string lower(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (const char character : text) {
        result.push_back(character == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
    }
    return result;
}

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

[[nodiscard]] std::string tag(const data::EffectiveObject& object, const std::string_view name) {
    const data::EffectiveValue* value = object.value(name);
    return value == nullptr ? std::string{} : trim(value->value.raw_text);
}

[[nodiscard]] double to_double(const sim::math::Fixed value) {
    return static_cast<double>(value.raw()) / static_cast<double>(sim::math::Fixed::scale);
}

[[nodiscard]] std::string number(const double value) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.3f", value);
    return text;
}

// SpaceBehavior is a comma or space separated list; DEBRIS makes a prop drift (BP-31).
[[nodiscard]] bool has_debris_behaviour(const std::string& behaviours) {
    std::string token;
    const auto match = [&token] { return lower(token) == "debris"; };
    for (const char character : behaviours) {
        if (character == ',' || std::isspace(static_cast<unsigned char>(character))) {
            if (match()) return true;
            token.clear();
        } else {
            token.push_back(character);
        }
    }
    return match();
}

} // namespace

DebrisProps::DebrisProps(godot::Node3D& host, const vfs::Vfs& filesystem, const data::Catalog& catalog)
    : host_(&host), filesystem_(&filesystem), catalog_(&catalog),
      backend_(std::make_unique<GodotParticleBackend>(
          host, [this](const std::string_view name) { return resolve_texture(name); })),
      registry_(std::make_unique<particles::EffectRegistry>(*backend_)) {
    // #638: nothing here reads the streams' hashes, so the frames do not compute them.
    registry_->set_stream_hashes(false);
}

DebrisProps::~DebrisProps() { release(); }

const assets::Texture* DebrisProps::resolve_texture(const std::string_view authored) {
    const std::size_t slash = authored.find_last_of("/\\");
    const std::string name = lower(slash == std::string_view::npos ? authored : authored.substr(slash + 1));
    auto found = textures_.find(name);
    if (found == textures_.end()) {
        std::optional<assets::Texture> decoded;
        std::string stem = name;
        for (const std::string_view suffix : {std::string_view(".tga"), std::string_view(".dds")}) {
            if (stem.ends_with(suffix)) stem.resize(stem.size() - suffix.size());
        }
        for (const std::string& candidate : {"data/art/textures/" + name, "data/art/textures/" + stem + ".tga",
                                             "data/art/textures/" + stem + ".dds"}) {
            if (!filesystem_->stat(candidate)) continue;
            if (auto loaded = assets::load_texture(*filesystem_, candidate)) {
                decoded = std::move(loaded.value());
                break;
            }
        }
        found = textures_.emplace(name, std::move(decoded)).first;
    }
    return found->second ? &*found->second : nullptr;
}

const DebrisProps::ParticleType* DebrisProps::particle_type(const std::string& name) {
    if (name.empty()) return nullptr;
    auto found = particle_types_.find(name);
    if (found != particle_types_.end()) return &found->second;
    ParticleType type;
    auto object = catalog_->resolve(name, data::Category::game_object);
    if (!object) {
        type.cause = "not in the XML catalog";
    } else {
        const std::string model = tag(object.value(), "Space_Model_Name");
        const auto lifetime = space::debris_seconds(tag(object.value(), "Particle_Lifetime_Frames"));
        type.lifetime_frames = lifetime && *lifetime > 0 ? static_cast<std::uint32_t>(*lifetime) : 0U;
        if (model.empty()) {
            type.cause = "no Space_Model_Name";
        } else {
            const std::string path = "data/art/models/" + lower(model);
            auto bytes = filesystem_->open(path);
            if (!bytes) {
                type.cause = core::format_diagnostic(bytes.error());
            } else if (auto system = particles::load_alo(bytes.value(), path); !system) {
                type.cause = core::format_diagnostic(system.error());
            } else if (system.value().emitters.empty()) {
                type.cause = "particle system declares no emitters";
            } else {
                type.system = std::move(system).value();
            }
        }
    }
    return &particle_types_.emplace(name, std::move(type)).first->second;
}

void DebrisProps::prepare(const units::UnitTables& tables, const tactical::CombatTable& combat,
                          std::vector<SpacePopulation::Options::PlacedShip>& placed_ships, const std::size_t units) {
    std::map<std::string, const units::UnitType*> types;
    for (const units::UnitType& type : tables.units) types.emplace(lower(type.id), &type);
    const std::size_t count = std::min(units, placed_ships.size());
    for (std::size_t index = 0; index < count; ++index) {
        const std::string unit_type = placed_ships[index].object_id;
        const sim::EntityId unit = placed_ships[index].live_entity;
        const auto type = types.find(lower(unit_type));
        if (unit == 0 || type == types.end()) continue;
        const tactical::CombatProfile* profile = combat.find(skirmish::type_id(type->second->id));
        for (std::uint32_t hardpoint = 0; hardpoint < type->second->hardpoints.size(); ++hardpoint) {
            const std::string& id = type->second->hardpoints[hardpoint].id;
            auto object = catalog_->resolve(id, data::Category::hardpoint);
            const std::string prop_type = object ? tag(object.value(), "Death_Breakoff_Prop") : std::string{};
            if (prop_type.empty()) continue;
            std::string status = "ready";
            Prop prop{.unit = unit, .hardpoint = hardpoint, .ship = placed_ships.size(), .type = prop_type};
            auto prop_object = catalog_->resolve(prop_type, data::Category::game_object);
            const tactical::TargetHardpoint* point = nullptr;
            if (profile != nullptr) {
                for (const tactical::TargetHardpoint& entry : profile->hardpoints) {
                    if (entry.hardpoint == hardpoint) point = &entry;
                }
            }
            if (!prop_object) {
                status = "prop type not in the catalog";
            } else if (!has_debris_behaviour(tag(prop_object.value(), "SpaceBehavior"))) {
                status = "prop type has no DEBRIS SpaceBehavior";
            } else if (tag(prop_object.value(), "Space_Model_Name").empty()) {
                status = "prop type has no Space_Model_Name";
            } else if (point == nullptr) {
                // FoC falls back to Fire_Bone_A without an attachment bone; the M2 roster's
                // breakoff hardpoints all have one (fidelity list).
                status = "hardpoint has no attachment point";
            } else {
                const data::EffectiveObject& value = prop_object.value();
                const auto vector = [&](const std::string_view name) {
                    const std::string text = tag(value, name);
                    return text.empty() ? std::array<double, 3>{} : space::debris_vector(text).value_or(std::array<double, 3>{});
                };
                prop.motion.movement = vector("Debris_Movement_Vector");
                prop.motion.rotation = vector("Debris_Facing_Rotate_Vector");
                prop.motion.min_lifetime_seconds = space::debris_seconds(tag(value, "Debris_Min_Lifetime_Seconds")).value_or(0);
                prop.motion.max_lifetime_seconds = space::debris_seconds(tag(value, "Debris_Max_Lifetime_Seconds")).value_or(0);
                prop.attachment = {to_double(point->position.x), to_double(point->position.y), to_double(point->position.z)};
                prop.fire = tag(value, "Debris_Attached_Particle");
                // Death_Explosions may list alternatives; the first is shown, as for units.
                const std::string explosions = tag(value, "Death_Explosions");
                prop.explosion = trim(std::string_view(explosions).substr(0, explosions.find(',')));
            }
            prepared_rows_.push_back("{\"unit\": " + std::to_string(unit) + ", \"hardpoint\": " + std::to_string(hardpoint)
                + ", \"hardpoint_type\": " + json(id) + ", \"prop\": " + json(prop_type) + ", \"fire\": " + json(prop.fire)
                + ", \"explosion\": " + json(prop.explosion) + ", \"status\": " + json(status) + "}");
            if (status != "ready") continue;
            SpacePopulation::Options::PlacedShip ship;
            ship.object_id = prop_type;
            ship.position = placed_ships[index].position;
            ship.yaw_degrees = placed_ships[index].yaw_degrees;
            ship.live_entity = first_prop_entity - props_.size();
            ship.breakoff = true;
            placed_ships.push_back(std::move(ship));
            prop_of_.emplace(std::pair{unit, hardpoint}, props_.size());
            props_.push_back(std::move(prop));
        }
    }
}

space::DebrisPose DebrisProps::pose_at(const space::DebrisFlight& flight, const double presented_tick) const {
    const double frames = std::max(0.0, presented_tick - (static_cast<double>(flight.tick) - 1.0));
    return space::debris_pose(flight.spawn, props_[flight.prop].motion, frames);
}

void DebrisProps::retire(const space::DebrisFlights::Ended& ended) {
    // BP-34: the lifetime has run out. FoC destroys the attached fire and kills the prop, which
    // shows its Death_Explosions and is removed (Remove_Upon_Death).
    const Prop& prop = props_[ended.flight.prop];
    pending_.push_back({prop.explosion, ended.death_tick, pose_at(ended.flight, ended.death_tick)});
    for (auto effect = effects_.begin(); effect != effects_.end();) {
        if (effect->follows != ended.serial) {
            ++effect;
            continue;
        }
        static_cast<void>(registry_->release(effect->handle));
        effect = effects_.erase(effect);
    }
    if (expired_rows_.size() < row_limit) {
        expired_rows_.push_back("{\"unit\": " + std::to_string(prop.unit) + ", \"hardpoint\": "
            + std::to_string(prop.hardpoint) + ", \"tick\": " + std::to_string(ended.flight.tick) + ", \"death_tick\": "
            + number(ended.death_tick + 1.0) + "}");
    }
}

void DebrisProps::pose(const std::span<const platform::LiveTickEvents> reached, const SnapshotAt& snapshot_at,
                       const tactical::PlayerId viewer, const double presented_tick,
                       std::vector<SpacePopulation::LivePose>& live, const bool reveal) {
    if (released_) return;
    // #401 review 2: the effect clock starts at the first frame, or at the birth of the oldest
    // tick that frame reaches, so an effect born before the first frame keeps its age.
    if (!clock_start_) {
        clock_start_ = space::debris_clock_start(
            presented_tick, reached.empty() ? std::nullopt : std::optional<std::uint64_t>(reached.front().tick));
    }
    std::vector<space::DebrisFlights::Ended> ended;
    for (const platform::LiveTickEvents& record : reached) {
        for (const tactical::Event& event : record.events) {
            if (event.kind != tactical::EventKind::hardpoint_destroyed) continue;
            const auto found = prop_of_.find({event.unit, event.hardpoint});
            if (found == prop_of_.end()) continue;
            const Prop& prop = props_[found->second];
            // The ship at the event's tick, as the local player saw it then. Once that tick's
            // snapshot has left the history, what the player saw is unknown and no prop is
            // thrown; a later frame's visibility never stands in for it (#401 review 1).
            const auto snapshot = snapshot_at(record.tick);
            if (!snapshot) {
                ++unknown_;
                continue;
            }
            const auto ship = space::debris_ship_at(snapshot.get(), event.unit, viewer, reveal);
            if (!ship) {
                ++not_seen_;
                continue;
            }
            space::DebrisFlight flight;
            flight.prop = found->second;
            flight.tick = record.tick;
            flight.spawn = space::debris_spawn(ship->position, ship->yaw_degrees, ship->roll_degrees, prop.attachment);
            flight.lifetime = space::debris_lifetime_frames(prop.motion, prop.unit, prop.hardpoint, record.tick,
                                                            tactical::logical_frames_per_second);
            // The flights that ran out before this event's birth end first, in tick order, so a
            // stall that reaches a destroy, repair and destroy again throws both (#401 review 3).
            const auto serial = flights_.launch(flight, ended);
            for (const space::DebrisFlights::Ended& gone : ended) retire(gone);
            ended.clear();
            if (!serial) {
                // A repaired hardpoint destroyed again while its first prop still flies.
                ++busy_;
                continue;
            }
            max_live_ = std::max<std::uint64_t>(max_live_, flights_.flights().size());
            if (spawn_rows_.size() < row_limit) {
                spawn_rows_.push_back("{\"unit\": " + std::to_string(prop.unit) + ", \"hardpoint\": "
                    + std::to_string(prop.hardpoint) + ", \"prop\": " + json(prop.type) + ", \"tick\": "
                    + std::to_string(record.tick) + ", \"lifetime_frames\": "
                    + (flight.lifetime ? std::to_string(*flight.lifetime) : std::string("null")) + ", \"position\": ["
                    + number(flight.spawn.position[0]) + ", " + number(flight.spawn.position[1]) + ", "
                    + number(flight.spawn.position[2]) + "]}");
            }
        }
    }
    flights_.expire(presented_tick, ended);
    for (const space::DebrisFlights::Ended& gone : ended) retire(gone);
    for (const auto& entry : flights_.flights()) {
        const space::DebrisFlight& flight = entry.second;
        const Prop& prop = props_[flight.prop];
        const space::DebrisPose now = pose_at(flight, presented_tick);
        SpacePopulation::LivePose placed;
        placed.ship = prop.ship;
        std::array<sim::math::Fixed, 6> values{};
        const std::array<double, 6> source{now.position[0], now.position[1], now.position[2], now.facing_degrees[2],
                                           now.facing_degrees[1], now.facing_degrees[0]};
        bool finite = true;
        for (std::size_t index = 0; index < values.size(); ++index) {
            auto fixed = scene::fixed_from_binary32(static_cast<float>(source[index]));
            finite = finite && static_cast<bool>(fixed);
            if (fixed) values[index] = fixed.value();
        }
        if (finite) {
            placed.position = {values[0], values[1], values[2]};
            placed.yaw_degrees = values[3];
            placed.pitch_degrees = values[4];
            placed.roll_degrees = values[5];
            live.push_back(std::move(placed));
        }
    }
}

particles::EmitterFrame DebrisProps::frame_of(const space::DebrisPose& pose) {
    // The fire rides on the prop's root bone: the prop's own R-ROT-01 transform (BP-33).
    particles::EmitterFrame frame;
    frame.origin = {static_cast<float>(pose.position[0]), static_cast<float>(pose.position[1]),
                    static_cast<float>(pose.position[2])};
    const auto fixed = [](const double value) {
        return sim::math::Fixed::from_raw(static_cast<std::int64_t>(std::llround(value * sim::math::Fixed::scale)));
    };
    const sim::math::Fixed zero = sim::math::Fixed::from_raw(0);
    const auto transform = scene::placement_transform(zero, zero, zero, fixed(pose.facing_degrees[2]),
        fixed(pose.facing_degrees[1]), fixed(pose.facing_degrees[0]), sim::math::Fixed::from_raw(sim::math::Fixed::scale));
    if (transform) {
        const auto& rows = transform.value().rows;
        const auto column = [&rows](const std::size_t index) {
            return particles::Vec3{static_cast<float>(to_double(rows[0][index])), static_cast<float>(to_double(rows[1][index])),
                                   static_cast<float>(to_double(rows[2][index]))};
        };
        frame.basis = {column(0), column(1), column(2)};
    }
    return frame;
}

bool DebrisProps::start(const std::string& particle, const space::DebrisPose& pose, const std::uint64_t born,
                        const std::uint64_t due, const std::optional<std::uint64_t> follows, const std::string& reason) {
    if (particle.empty()) return true;
    const std::string key = reason + ":" + particle;
    const ParticleType* type = particle_type(particle);
    if (type == nullptr || !type->system) {
        ++start_failed_[key];
        return true;
    }
    // #401 review 2: a frame that reaches the effect's birth only after its lifetime has run out
    // (a presentation stall) does not show it at all.
    if (space::debris_effect_ended(born, type->lifetime_frames, due)) {
        ++skipped_[key];
        return true;
    }
    auto handle = registry_->spawn(*type->system, seed_++, effect_capacity);
    if (!handle || !registry_->set_frame(handle.value(), frame_of(pose))) {
        if (handle) static_cast<void>(registry_->release(handle.value()));
        ++start_failed_[key];
        return true;
    }
    ++started_[key];
    Effect effect{handle.value(), particle, born, 0U, type->lifetime_frames, false, follows};
    // Born before the clock's present: it catches up.
    bool gone = false;
    for (std::uint64_t sample = born; sample < samples_ && !gone; ++sample) {
        if (!step(effect, sample, gone)) return false;
    }
    if (!gone) effects_.push_back(std::move(effect));
    return true;
}

bool DebrisProps::follow(const Effect& effect, const std::uint64_t sample) {
    if (!effect.follows) return true;
    const auto flight = flights_.flights().find(*effect.follows);
    if (flight == flights_.flights().end()) return true;
    const double tick = *clock_start_ + static_cast<double>(sample);
    if (!registry_->set_frame(effect.handle, frame_of(pose_at(flight->second, tick)))) {
        failure_ = "breakoff fire " + effect.particle + ": its frame was refused";
        return false;
    }
    return true;
}

bool DebrisProps::step(Effect& effect, const std::uint64_t sample, bool& gone) {
    gone = false;
    if (!follow(effect, sample)) return false;
    auto advanced = registry_->advance(effect.handle, 1.0F / 30.0F, camera_frame_);
    if (!advanced) {
        failure_ = "breakoff effect " + effect.particle + ": " + core::format_diagnostic(advanced.error());
        return false;
    }
    after_step(effect, advanced.value(), gone);
    return true;
}

void DebrisProps::after_step(Effect& effect, const particles::EffectFrameStats& advanced, bool& gone) {
    gone = false;
    ++effect.age;
    if (!effect.detached && effect.age >= effect.lifetime) {
        // The particle object's lifetime ends: FoC detaches its system, which drains.
        auto detached = registry_->detach(effect.handle);
        effect.detached = true;
        gone = !detached || detached.value() == particles::EffectDetachState::released;
    } else if (effect.detached && (advanced.finished || effect.age >= effect.lifetime + drain_limit_frames)) {
        static_cast<void>(registry_->release(effect.handle));
        gone = true;
    }
}

bool DebrisProps::advance_until(const std::uint64_t target) {
    for (; samples_ < target; ++samples_) {
        // #638: the effects born by this sample stand at their frames, step in one batch on the
        // particle workers, then age, detach or go in effect order, as one step after another did.
        batch_handles_.clear();
        for (const Effect& effect : effects_) {
            if (effect.born > samples_) continue;
            if (!follow(effect, samples_)) return false;
            batch_handles_.push_back(effect.handle);
        }
        if (auto advanced = registry_->advance_all(batch_handles_, 1.0F / 30.0F, camera_frame_, batch_stats_);
            !advanced) {
            failure_ = "breakoff effect: " + core::format_diagnostic(advanced.error());
            return false;
        }
        std::size_t next = 0;
        for (auto effect = effects_.begin(); effect != effects_.end();) {
            bool gone = false;
            if (effect->born <= samples_) after_step(*effect, batch_stats_[next++], gone);
            effect = gone ? effects_.erase(effect) : effect + 1;
        }
    }
    return true;
}

bool DebrisProps::effects(const FixedCamera& camera, const double presented_tick) {
    if (released_) return true;
    // pose() starts the clock; this is for a frame that never posed.
    if (!clock_start_) clock_start_ = presented_tick;
    camera_frame_ = particles::camera_frame_from_render(camera.eye, camera.target, camera.up);
    const auto sample_of = [this](const double tick) {
        return static_cast<std::uint64_t>(std::max(0.0, std::ceil(tick - *clock_start_ - 1.0e-9)));
    };
    const auto due = static_cast<std::uint64_t>(std::max(0.0, std::floor(presented_tick - *clock_start_ + 1.0e-9)));
    // BP-33: each new prop carries its fire from its birth.
    for (const auto& [serial, flight] : flights_.flights()) {
        if (serial <= fired_through_) continue;
        fired_through_ = serial;
        const double birth = static_cast<double>(flight.tick) - 1.0;
        if (!start(props_[flight.prop].fire, pose_at(flight, birth), sample_of(birth), due, serial, "fire")) return false;
    }
    for (const PendingExplosion& explosion : pending_) {
        if (!start(explosion.particle, explosion.pose, sample_of(explosion.birth_tick), due, std::nullopt, "death")) {
            return false;
        }
    }
    pending_.clear();
    return advance_until(due);
}

void DebrisProps::release() {
    if (released_) return;
    released_ = true;
    for (const Effect& effect : effects_) static_cast<void>(registry_->release(effect.handle));
    effects_.clear();
    flights_.clear();
    pending_.clear();
}

void DebrisProps::write_report(std::ostream& output) const {
    const auto rows = [&output](const char* name, const std::vector<std::string>& values) {
        output << ", \"" << name << "\": [";
        for (std::size_t index = 0; index < values.size(); ++index) output << (index ? ", " : "") << values[index];
        output << "]";
    };
    const auto counts = [&output](const char* name, const std::map<std::string, std::uint64_t>& values) {
        output << ", \"" << name << "\": {";
        bool first = true;
        for (const auto& [key, value] : values) {
            output << (first ? "" : ", ") << json(key) << ": " << value;
            first = false;
        }
        output << "}";
    };
    output << "  \"breakoff_props\": {\"props\": " << props_.size() << ", \"live\": " << flights_.flights().size()
           << ", \"max_live\": " << max_live_ << ", \"not_seen\": " << not_seen_ << ", \"unknown\": " << unknown_
           << ", \"busy\": " << busy_ << ", \"live_effects\": " << effects_.size()
           << ", \"clock_start\": " << (clock_start_ ? number(*clock_start_) : std::string("null"));
    rows("prepared", prepared_rows_);
    rows("spawned", spawn_rows_);
    rows("expired", expired_rows_);
    counts("effects_started", started_);
    counts("effects_failed", start_failed_);
    counts("effects_skipped", skipped_);
    output << ", \"particle_types\": {";
    bool first = true;
    for (const auto& [name, type] : particle_types_) {
        output << (first ? "" : ", ") << json(name) << ": " << json(type.system ? "ready" : type.cause);
        first = false;
    }
    output << "}, \"error\": " << (failure_.empty() ? std::string("null") : json(failure_)) << "},\n";
}

} // namespace eawr::presentation::godot_backend
