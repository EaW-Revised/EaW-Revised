"""Run production clip preparation, retirement and reuse against engine-free stand-ins."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'tools'))
from native_cpp_fixture import compile_fixture, select_toolchain
from tests.presentation.renderer.pose_threads_harness import HARNESS as POSE_HARNESS


def function(path, signature):
    source = (ROOT / path).read_text()
    start = source.index(signature)
    return source[start:source.index('\n}', source.index('{', start)) + 2]


HARNESS = r"""
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>
namespace sim { using EntityId = std::uint64_t; using AssetId = std::uint64_t;
struct RenderInstance { EntityId entity_id{}; AssetId asset_id{}; };
}
namespace core {
struct Error { std::string text; };
std::string format_diagnostic(const Error& error) { return error.text; }
template<class T> struct Result {
    std::optional<T> data;
    explicit operator bool() const { return data.has_value(); }
    T& value() { return *data; }
    Error error() const { return {"sample failed"}; }
};
}
namespace animation {
using Matrix = std::array<float, 16>;
struct BonePose { Matrix skin_asset{}, model_asset{}; bool visible = true; };
struct Pose { std::vector<BonePose> bones; };
struct ClipPosition { std::uint64_t position{}; std::uint32_t subdivisions = 1;
    friend bool operator==(const ClipPosition&, const ClipPosition&) = default; };
struct DeathFrame { ClipPosition position; float blend_from{}; bool shown = true; };
struct Player {
    bool visible_end{};
    mutable std::atomic<int> samples{};
    std::uint32_t playable_frames() const { return 3; }
    core::Result<int> sample_position(std::uint64_t position, std::uint32_t subdivisions, Pose& output) const;
};
core::Result<Pose> sample_death_frame(const Player& player, const DeathFrame& frame) {
    ++player.samples;
    Pose pose; pose.bones.resize(2);
    // The root is always visible but weights no geometry. Middle pose hides
    // the piece transiently; end visibility belongs to the authored variant.
    pose.bones[1].visible = frame.position.position >= 3 * frame.position.subdivisions
        ? player.visible_end : frame.position.position != frame.position.subdivisions;
    for (auto& bone : pose.bones) { bone.skin_asset.fill(1.0F); bone.model_asset.fill(1.0F); }
    return {std::move(pose)};
}
core::Result<int> Player::sample_position(std::uint64_t position, std::uint32_t subdivisions, Pose& output) const {
    auto pose = sample_death_frame(*this, {{position, subdivisions}, 0.0F});
    output.bones.assign(pose.value().bones.begin(), pose.value().bones.end());
    return {0};
}

}
namespace particles {
class StepExecutor {
public:
    virtual ~StepExecutor() = default;
    virtual bool run(std::size_t, const std::function<void(std::size_t)>&) const = 0;
};
}
struct Executor : particles::StepExecutor {
    unsigned count; mutable unsigned runs{}; bool fail{};
    explicit Executor(unsigned workers) : count(workers) {}
    bool run(std::size_t size, const std::function<void(std::size_t)>& task) const override {
        ++runs;
        if (fail) { if (size) task(0); return false; }
        std::vector<std::thread> jobs;
        for (unsigned worker = 0; worker < count; ++worker)
            jobs.emplace_back([&, worker] { for (auto i = worker; i < size; i += count) task(i); });
        for (auto& job : jobs) job.join();
        return true;
    }
};
struct GodotRenderer {
    std::map<sim::AssetId, std::vector<std::uint8_t>> usage;
    std::set<sim::EntityId> instances, palettes;
    std::map<sim::EntityId, std::array<float,3>> colours;
    std::vector<sim::AssetId> released;
    std::vector<sim::EntityId> uploads;
    std::span<const std::uint8_t> skin_geometry_bones_view(sim::AssetId id) const {
        const auto found = usage.find(id); return found == usage.end() ? std::span<const std::uint8_t>{} : found->second;
    }
    struct PreparedSkinPose { std::vector<animation::BonePose> bones; };
    static inline std::atomic<unsigned> preparations{};
    static PreparedSkinPose prepare_skin_pose(std::span<const animation::BonePose> bones) {
        ++preparations;
        return {std::vector<animation::BonePose>(bones.begin(), bones.end())};
    }
    core::Result<int> set_prepared_skin_pose(sim::EntityId entity, sim::AssetId asset, const PreparedSkinPose& pose) {
        return set_skin_pose(entity, asset, pose.bones);
    }
    core::Result<int> set_skin_pose(sim::EntityId entity, sim::AssetId, std::span<const animation::BonePose>) {
        palettes.insert(entity); uploads.push_back(entity); return {0};
    }
    void forget_instance_pose(sim::EntityId id) { instances.erase(id); palettes.erase(id); colours.erase(id); }
    void set_unit_colorization(sim::EntityId id, std::array<float,3> value) { colours[id] = value; }
    int release(sim::AssetId id) { released.push_back(id); return 0; }
    std::vector<int> resources() const { return {}; }
};
void prune_effect_clock_assets(std::vector<sim::AssetId>&, const std::vector<int>&) {}
struct SpacePopulation {
    struct LiveClipPose { std::size_t ship{}; animation::ClipPosition position{}; float blend_from{}; bool alternate{}; };
    struct GatedInstance {
        sim::RenderInstance instance; std::optional<std::size_t> live;
        std::optional<std::array<std::uint8_t,3>> colour;
    };
    struct LiveClipInstance { std::size_t ship{}; sim::EntityId entity{}; sim::AssetId asset{}; };
    struct AnimatedInstance { sim::EntityId entity{}; sim::AssetId asset{}; std::size_t idle{}; };
    struct HeldClip { animation::ClipPosition position; std::vector<animation::BonePose> bones; bool hidden{}; };
    struct ParkedShip { std::vector<GatedInstance> pieces; std::vector<LiveClipInstance> clips;
        std::vector<AnimatedInstance> animated; };
    struct PreparedLiveClip {
        const animation::Player* player{}; bool terminal{}, cached{}, hidden{}, consumed{};
        std::vector<std::span<const std::uint8_t>> geometry;
        animation::Pose sample;
        bool pose{}; GodotRenderer::PreparedSkinPose palette; std::string failure;
    };
    std::vector<PreparedLiveClip> prepared_clips_;
    std::vector<std::size_t> active_clips_;
    std::span<const LiveClipPose> pending_clip_poses_;
    void prepare_live_clip(std::size_t);
    bool arrival_sample{};
    void schedule_arrival_sample() { arrival_sample = true; }

    struct Index {
        std::vector<std::vector<std::size_t>> pieces_, clips_;
        void reset_opacity(std::size_t) {}
        std::span<const std::size_t> pieces(std::size_t ship) const { return pieces_[ship]; }
        std::span<const std::size_t> clips(std::size_t ship) const { return clips_[ship]; }
    } piece_index_;
    std::vector<bool> live_retired_, live_death_clone_, live_shown_;
    std::vector<std::optional<int>> live_transforms_;
    std::vector<std::optional<std::size_t>> live_clips_, live_alternate_clips_;
    std::vector<std::shared_ptr<const animation::Player>> clips_;
    std::vector<std::vector<animation::BonePose>> contact_bones_;
    std::map<sim::EntityId,std::size_t> contact_live_ships_;
    std::vector<GatedInstance> pieces_;
    std::vector<LiveClipInstance> live_clip_instances_;
    std::vector<AnimatedInstance> animated_instances_;
    std::map<std::size_t,HeldClip> held_clips_;
    std::map<std::size_t,ParkedShip> parked_ships_;
    std::vector<sim::AssetId> uploaded_, effect_clock_assets_;
    std::map<sim::AssetId,int> passes_;
    std::uint64_t live_clip_hidden_bones_{}, live_clip_poses_{};
    std::size_t live_released_assets_{};
    std::string failure_;
    const animation::Player* live_clip(std::size_t ship, bool = false) const {
        return ship < live_clips_.size() && live_clips_[ship] ? clips_[*live_clips_[ship]].get() : nullptr;
    }
    void rebuild_piece_index() {
        piece_index_.pieces_.assign(live_clips_.size(), {}); piece_index_.clips_.assign(live_clips_.size(), {});
        for (std::size_t i = 0; i < pieces_.size(); ++i) piece_index_.pieces_[*pieces_[i].live].push_back(i);
        for (std::size_t i = 0; i < live_clip_instances_.size(); ++i) piece_index_.clips_[live_clip_instances_[i].ship].push_back(i);
    }
    void refresh_instances() {}
    bool pose_live_clips(GodotRenderer&, std::span<const LiveClipPose>, const particles::StepExecutor* = nullptr);
    bool live_clip_terminal_hidden(std::size_t) const noexcept;
    std::span<const animation::BonePose> terminal_clip_bones(std::size_t) const noexcept;
    std::span<const animation::BonePose> held_clip_bones(std::size_t, animation::ClipPosition, float) const noexcept;
    void retire_live_ship(GodotRenderer&, std::size_t, bool = false);
    void restore_live_ship(GodotRenderer&, std::size_t);
};
using BattleScene = SpacePopulation;
__FUNCTIONS__
int main() {
    for (const unsigned workers : {1,2,4,8}) {
        Executor executor(workers); SpacePopulation population; GodotRenderer renderer;
        population.live_retired_.resize(260, false); population.live_death_clone_.resize(260, true);
        population.live_shown_.resize(260, true); population.live_transforms_.resize(260, 1);
        population.live_alternate_clips_.resize(260); population.contact_bones_.resize(260);
        for (std::size_t ship = 0; ship < 260; ++ship) {
            auto player = std::make_shared<animation::Player>(); player->visible_end = ship == 1;
            population.clips_.push_back(player); population.live_clips_.push_back(ship);
            population.pieces_.push_back({{ship+10, ship+100}, ship, std::array<std::uint8_t,3>{10,20,30}});
            population.live_clip_instances_.push_back({ship,ship+10,ship+100});
            population.uploaded_.push_back(ship+100); renderer.usage[ship+100] = {0,1};
            renderer.instances.insert(ship+10); population.contact_live_ships_[ship+1000] = ship;
        }
        // Unknown/unskinned extra geometry must veto terminal retirement.
        population.pieces_.push_back({{9999,99999}, 3, std::nullopt});
        population.rebuild_piece_index();
        std::vector<SpacePopulation::LiveClipPose> requests{{0,{3,1},0,false}, {1,{3,1},0,false},
            {2,{1,1},0,false}, {3,{3,1},0,false}, {4,{3,1},0.1F,false}};
        for (std::size_t ship = 5; ship < 260; ++ship) requests.push_back({ship,{3,1},0,false});
        assert(population.pose_live_clips(renderer, requests, &executor));
        assert(executor.runs == 1);
        const auto clean = [&] {
            assert(population.pending_clip_poses_.empty() && population.active_clips_.empty());
            for (const auto& row : population.prepared_clips_)
                assert(!row.player && !row.pose && row.geometry.empty() && row.sample.bones.empty());
        };
        clean();
        assert(population.live_clip_terminal_hidden(0));
        assert(!population.live_clip_terminal_hidden(1)); // visible end pose
        assert(!population.live_clip_terminal_hidden(2)); // transient mid-clip hide
        assert(!population.live_clip_terminal_hidden(3)); // unposed/static extra geometry
        assert(!population.live_clip_terminal_hidden(4)); // blend still active
        const auto* bone_data = population.prepared_clips_[2].sample.bones.data();
        const auto bone_capacity = population.prepared_clips_[2].sample.bones.capacity();
        const auto* geometry_data = population.prepared_clips_[0].geometry.data();
        const auto uploads = renderer.uploads.size();
        const auto* scratch = population.prepared_clips_.data();
        assert(scratch != nullptr && population.prepared_clips_.capacity() >= 260);
        assert(population.pose_live_clips(renderer, {}, &executor));
        assert(population.prepared_clips_.data() == scratch && executor.runs == 1);

        const auto* retained = population.live_clip(0);
        assert(population.pose_live_clips(renderer, std::span(requests).first(2), &executor));
        assert(retained->samples == 1 && population.live_clip(1)->samples == 1);
        assert(population.prepared_clips_.data() == scratch && executor.runs == 1);
        assert(renderer.uploads.size() == uploads); // held poses do not cross renderer API
        clean();
        assert(population.prepared_clips_[2].sample.bones.data() == bone_data);
        assert(population.pose_live_clips(renderer, requests, &executor));
        clean();
        assert(population.prepared_clips_[2].sample.bones.data() == bone_data);
        assert(population.prepared_clips_[2].sample.bones.capacity() == bone_capacity);
        assert(population.prepared_clips_[0].geometry.data() == geometry_data);
        Executor failed(workers); failed.fail = true;
        assert(!population.pose_live_clips(renderer, requests, &failed));
        clean();
        assert(population.pose_live_clips(renderer, requests, &executor));
        clean();
        assert(population.held_clip_bones(0,{3,1},0).size() == 2);
        assert(population.held_clip_bones(0,{1,1},0).empty()); // catch-up reads its own sample
        population.animated_instances_.push_back({10,100,0});
        population.retire_live_ship(renderer, 0, true);
        assert(population.live_retired_[0] && population.contact_bones_[0].empty());
        assert(population.animated_instances_.empty());
        const auto before_retired = retained->samples.load();
        assert(population.pose_live_clips(renderer, std::span(requests).first(1), &executor));
        assert(retained->samples == before_retired && population.contact_bones_[0].empty());
        assert(!population.contact_live_ships_.contains(1000) && !population.live_transforms_[0]);
        assert(!renderer.instances.contains(10) && !renderer.palettes.contains(10));
        assert(population.piece_index_.pieces(0).empty() && population.piece_index_.clips(0).empty());
        assert(!population.live_clip_terminal_hidden(0) && renderer.released.empty());
        population.restore_live_ship(renderer, 0);
        assert(!population.live_retired_[0] && population.piece_index_.pieces(0).size() == 1);
        assert(population.animated_instances_.size() == 1 && population.arrival_sample);
        assert(renderer.colours.at(10)[0] == 10.0F / 255.0F);
        requests[0].position = {0,1};
        assert(population.pose_live_clips(renderer, std::span(requests).first(1), &executor));
        assert(retained->samples == 2 && !population.live_clip_terminal_hidden(0));
        // A later retirement really releases a nonreusable upload and player.
        population.retire_live_ship(renderer, 0, false);
        assert(renderer.released == std::vector<sim::AssetId>{100} && !population.live_clip(0));
        // Sampling remains necessary for contacts even with no palette consumer.
        SpacePopulation contacts; GodotRenderer unused;
        auto contact_player = std::make_shared<animation::Player>();
        contacts.clips_.push_back(contact_player); contacts.live_clips_ = {0};
        contacts.live_alternate_clips_.resize(1); contacts.live_retired_ = {false};
        contacts.live_death_clone_ = {false}; contacts.live_shown_ = {true};
        contacts.contact_bones_.resize(1); contacts.rebuild_piece_index();
        const SpacePopulation::LiveClipPose contact_request{0,{1,1},0,false};
        const auto preparations = GodotRenderer::preparations.load();
        assert(contacts.pose_live_clips(unused, std::span(&contact_request,1), &executor));
        assert(contact_player->samples == 1 && contacts.contact_bones_[0].size() == 2);
        assert(unused.uploads.empty() && GodotRenderer::preparations == preparations);
        // One-worker admission uses the BASE serial upload and retains its buffers.
        contacts.live_clip_instances_.push_back({0,42,100});
        contacts.rebuild_piece_index();
        const auto capacity = contacts.prepared_clips_.capacity();
        assert(contacts.pose_live_clips(unused, std::span(&contact_request,1), nullptr));
        assert(contact_player->samples == 2 && unused.uploads == std::vector<sim::EntityId>{42});
        assert(GodotRenderer::preparations == preparations && contacts.prepared_clips_.capacity() == capacity);
    }
}
"""


EMITTER_HARNESS = r"""
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <vector>
#include <utility>
namespace animation { struct BonePose { bool visible = true; }; }
struct Life {
    std::optional<int> attached;
    std::vector<int> drains;
    static inline std::vector<int> released;
    auto active() const { return attached; }
    const auto& draining() const { return drains; }
    int release_all() {
        released.insert(released.end(), drains.begin(), drains.end());
        if (attached) released.push_back(*attached);
        attached.reset(); drains.clear(); return 0;
    }
};
struct Registry {
    std::set<int> ended, loops;
    bool exhausted(int handle) const { return !loops.contains(handle) && ended.contains(handle); }
};
struct UnitEmitters {
    struct CloneProxy { std::size_t bone{}; std::optional<Life> life; std::optional<int> last; };
    struct Ship { std::vector<CloneProxy> clone; bool clone_planned = true; };
    std::vector<Ship> ships_;
    std::vector<CloneProxy> clone_orphans_;
    Registry registry;
    Registry* registry_ = &registry;
    void orphan_clone(Ship&);
    bool retire_clone(std::size_t, std::span<const animation::BonePose>);
};
__FUNCTIONS__
int main() {
    UnitEmitters emitters;
    emitters.ships_.resize(1);
    emitters.ships_[0].clone = {{0,Life{10,{}},123}, {1,Life{std::nullopt,{11}},456},
        {1,Life{std::nullopt,{}},789}};
    std::vector<animation::BonePose> bones{{true},{false}};
    assert(!emitters.retire_clone(0,bones)); // visible burning effect must keep its owner
    assert(emitters.clone_orphans_.empty() && emitters.ships_[0].clone.size() == 3);
    bones[0].visible = false;
    assert(emitters.retire_clone(0,bones));
    assert(emitters.ships_[0].clone.empty() && !emitters.ships_[0].clone_planned);
    assert(emitters.clone_orphans_.size() == 2); // empty terminal lifecycle discarded
    assert(emitters.clone_orphans_[0].last == 123 && emitters.clone_orphans_[1].last == 456);
    assert(emitters.clone_orphans_[0].life->active() == 10);
    assert(emitters.clone_orphans_[1].life->draining() == std::vector<int>{11});
    assert(!emitters.retire_clone(0,bones)); // unplanned reused slot cannot suppress a future birth

    // Exhausted visible effects no longer pin terminal hidden geometry. Keep
    // hidden drains on their original orphan path with their last frame.
    emitters.ships_[0].clone_planned = true;
    emitters.ships_[0].clone = {{0,Life{20,{21}},100}, {1,Life{std::nullopt,{22}},200}};
    bones[0].visible = true;
    emitters.registry.ended = {20};
    assert(!emitters.retire_clone(0,bones)); // live child/drain still needs its owner
    assert(emitters.ships_[0].clone[0].life->active() == 20);
    assert(emitters.clone_orphans_.size() == 2); // failed approval changes nothing
    assert(Life::released.empty());
    emitters.registry.ended.insert(21);
    assert(emitters.retire_clone(0,bones));
    assert(emitters.ships_[0].clone.empty() && !emitters.ships_[0].clone_planned);
    assert(emitters.clone_orphans_.size() == 3);
    assert(emitters.clone_orphans_.back().last == 200);
    assert(emitters.clone_orphans_.back().life->draining() == std::vector<int>{22});
    assert((std::multiset<int>(Life::released.begin(), Life::released.end()) == std::multiset<int>{20,21}));
    Life::released.clear();

    emitters.ships_[0].clone_planned = true;
    emitters.ships_[0].clone = {{0,Life{std::nullopt,{}},100}};
    assert(!emitters.retire_clone(0,bones)); // no generation yet: do not lose a future birth
    emitters.ships_[0].clone = {{0,Life{30,{}},100}};
    assert(!emitters.retire_clone(0,bones)); // live finite effect
    // Registry semantics (including empty intervals and looping roots) are
    // exercised with real systems in render_batch_tests.cpp:586-619.
    emitters.registry.ended.insert(40);
    emitters.registry.loops.insert(40);
    emitters.ships_[0].clone = {{0,Life{40,{}},100}};
    assert(!emitters.retire_clone(0,bones)); // an empty looping root never exhausts
    assert(Life::released.empty());
    emitters.ships_[0].clone = {{bones.size(),Life{40,{}},100}};
    emitters.registry.loops.clear(); // even proven exhaustion cannot validate a missing bone
    assert(!emitters.retire_clone(0,bones));
    assert(Life::released.empty());
    emitters.ships_[0].clone = {{0,Life{30,{}},100}};
    emitters.registry.ended.insert(30);
    emitters.ships_[0].clone.push_back({0,Life{31,{}},200});
    assert(!emitters.retire_clone(0,bones)); // another proxy can veto without releasing the first
    assert(emitters.ships_[0].clone[0].life->active() == 30);
    assert(Life::released.empty());
    emitters.registry.ended.insert(31);
    assert(emitters.retire_clone(0,bones));
    assert(emitters.clone_orphans_.size() == 3); // no empty exhausted effects moved to drains
    assert((std::multiset<int>(Life::released.begin(), Life::released.end()) == std::multiset<int>{30,31}));
}
"""


LIFECYCLE_HARNESS = r"""
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>
namespace animation { struct Player {}; struct DeathFrame { bool shown = true; }; }
struct GodotRenderer {};
struct SpacePopulation {
    struct LivePose { std::size_t ship{}; };
    animation::Player player;
    std::set<std::size_t> hidden;
    std::vector<std::pair<std::size_t,bool>> retired;
    const animation::Player* live_clip(std::size_t) const { return &player; }
    bool live_clip_terminal_hidden(std::size_t ship) const { return hidden.contains(ship); }
    void retire_live_ship(GodotRenderer&, std::size_t ship, bool reusable) { retired.emplace_back(ship,reusable); }
};
using BattleScene = SpacePopulation;
struct LiveSessionView {
    struct DeathClone { std::size_t ship{}; };
    struct ActiveClone { std::uint64_t unit{}, death_tick{}; SpacePopulation::LivePose pose;
                         std::optional<std::uint64_t> hidden_since; };
    struct CloneFrame { SpacePopulation::LivePose pose; std::optional<animation::DeathFrame> death; };
    std::vector<ActiveClone> active_clones_, retiring_clones_;
    std::set<std::uint64_t> terminal_source_leases_;
    std::map<std::uint64_t,DeathClone> death_clones_;
    std::vector<DeathClone> launch_clones_;
    double presented_tick_ = 12;
    static std::optional<animation::DeathFrame> clone_death_frame(const DeathClone&, const animation::Player*,
                                                                std::uint64_t) { return animation::DeathFrame{}; }
    void retire_clones(SpacePopulation&, GodotRenderer&, const std::function<bool(std::size_t)>& = {});
    std::optional<CloneFrame> clone_frame(const SpacePopulation&, std::size_t, double) const;
};
__FUNCTIONS__
int main() {
    LiveSessionView view; SpacePopulation population; GodotRenderer renderer;
    view.death_clones_ = {{42,{7}},{43,{8}}}; view.launch_clones_ = {{7}};
    view.active_clones_ = {{42,10,{7},std::nullopt},{43,10,{8},std::nullopt}};
    population.hidden = {7,8};
    std::vector<std::size_t> approvals;
    view.retire_clones(population,renderer,[&](std::size_t ship) { approvals.push_back(ship); return ship == 7; });
    assert(approvals == std::vector<std::size_t>({7,8}));
    assert(view.active_clones_[0].hidden_since == 3 && !view.active_clones_[1].hidden_since);
    assert(population.retired.empty()); // selection does not release before the frame drops geometry
    assert(view.clone_frame(population,7,11) && !view.clone_frame(population,7,12));
    view.retiring_clones_.push_back(view.active_clones_[0]);
    view.active_clones_.erase(view.active_clones_.begin());
    assert(view.clone_frame(population,7,11) && !view.clone_frame(population,7,12));
    // The same production retirement function chooses warm pooling for a launch
    // clone and permanent release for an ordinary start unit; historical samples
    // remain accessible until the emitter catch-up call commits the retirement.
    view.retire_clones(population,renderer);
    assert(view.retiring_clones_.empty() && !view.clone_frame(population,7,11));
    assert((population.retired == std::vector<std::pair<std::size_t,bool>>{{7,true}}));
    assert(view.terminal_source_leases_ == std::set<std::uint64_t>{42});
    view.active_clones_[0].hidden_since = 3; // terminal non-launch units reserve nothing
    view.retiring_clones_.push_back(view.active_clones_[0]); view.active_clones_.clear();
    view.retire_clones(population,renderer);
    assert((population.retired == std::vector<std::pair<std::size_t,bool>>{{7,true},{8,false}}));
    // Ordinary finite expiry must still return its source lease.
    view.death_clones_.emplace(44, LiveSessionView::DeathClone{9});
    view.launch_clones_.push_back({9});
    view.retiring_clones_.push_back({44,10,{9},std::nullopt});
    view.retire_clones(population,renderer);
    assert(view.terminal_source_leases_ == std::set<std::uint64_t>{42});
    assert((population.retired.back() == std::pair<std::size_t,bool>{9,true}));
}
"""


MATERIAL_GUARD_HARNESS = r"""
#include <cassert>
#include <cstdint>
#include <map>
#include <span>
#include <vector>
namespace sim { using AssetId = std::uint64_t; }
enum class MaterialRoute { legacy, modern_spatial };
struct Description { MaterialRoute route = MaterialRoute::legacy; bool wind{}; };
namespace legacy { bool reads_wind(const Description& description) { return description.wind; } }
struct GodotRenderer {
    struct Impl {
        struct Resource {
            bool skinned = true, hidden_geometry_collapses = true;
            Description description;
            std::vector<std::uint8_t> skin_used_bones{1};
        };
        std::map<sim::AssetId, Resource> resources_;
        std::vector<std::uint8_t> skin_geometry_bones(sim::AssetId) const;
        std::span<const std::uint8_t> skin_geometry_bones_view(sim::AssetId) const;
    };
};
__FUNCTIONS__
int main() {
    GodotRenderer::Impl renderer;
    auto& resource = renderer.resources_[1];
    const auto original = resource.skin_used_bones;
    assert(renderer.skin_geometry_bones(1) == original);
    assert(renderer.skin_geometry_bones_view(1).data() == resource.skin_used_bones.data());
    resource.skinned = false;
    assert(renderer.skin_geometry_bones(1).empty());
    assert(renderer.skin_geometry_bones_view(1).empty());
    resource.skinned = true;
    resource.hidden_geometry_collapses = false;
    assert(renderer.skin_geometry_bones(1).empty());
    assert(renderer.skin_geometry_bones_view(1).empty());
    resource.hidden_geometry_collapses = true;
    resource.description.route = MaterialRoute::modern_spatial;
    assert(renderer.skin_geometry_bones(1).empty());
    assert(renderer.skin_geometry_bones_view(1).empty());
    resource.description.route = MaterialRoute::legacy;
    resource.description.wind = true;
    assert(renderer.skin_geometry_bones(1).empty());
    assert(renderer.skin_geometry_bones_view(1).empty());
    resource.description.wind = false;
    assert(renderer.skin_geometry_bones(1) == original);
    assert(renderer.skin_geometry_bones_view(1).data() == resource.skin_used_bones.data());
    assert(renderer.skin_geometry_bones(2).empty());
    assert(renderer.skin_geometry_bones_view(2).empty());
    assert(renderer.resources_.size() == 1 && resource.skin_used_bones == original);
}
"""


TOPOLOGY_HARNESS = r"""
#include "__HEADER__"
#include <cassert>
#include <limits>
using namespace eawr;
using presentation::godot_backend::detail::hidden_skin_geometry_collapses;

assets::Model rigid_model() {
    assets::Model model;
    model.bones.resize(3);
    assets::Mesh mesh;
    mesh.bone = 0;
    assets::Submesh surface;
    surface.vertices.resize(3);
    surface.vertices[1].position.x = 1;
    surface.vertices[2].position.y = 1;
    surface.indices = {0, 1, 2};
    mesh.submeshes.push_back(surface);
    model.meshes.push_back(mesh);
    return model;
}

int main() {
    auto model = rigid_model();
    assert(hidden_skin_geometry_collapses(model));
    auto& mesh = model.meshes[0];
    auto& surface = mesh.submeshes[0];
    mesh.bone = -1;
    assert(!hidden_skin_geometry_collapses(model)); // unskinned geometry remains drawn
    surface.skin_bones = {0, 1, 2};
    for (std::size_t index = 0; index < 3; ++index) {
        surface.vertices[index].bone_indices[0] = static_cast<std::uint32_t>(index);
        surface.vertices[index].bone_weights[0] = 1;
    }
    // All three bases are hidden, but their retained origins span area 0.5.
    // The shortcut must leave this triangle submitted despite hidden bones.
    std::array<std::array<float, 16>, 3> palette{};
    for (auto& bone : palette) bone[15] = 1;
    palette[1][12] = 1;
    palette[2][13] = 1;
    const auto transformed = [&](const assets::Vertex& vertex) {
        assets::Vec3f point{};
        for (std::size_t influence = 0; influence < 4; ++influence) {
            const float weight = vertex.bone_weights[influence];
            if (weight == 0) continue;
            const auto& m = palette[surface.skin_bones[vertex.bone_indices[influence]]];
            point.x += weight * (m[0] * vertex.position.x + m[4] * vertex.position.y
                + m[8] * vertex.position.z + m[12]);
            point.y += weight * (m[1] * vertex.position.x + m[5] * vertex.position.y
                + m[9] * vertex.position.z + m[13]);
        }
        return point;
    };
    const auto a = transformed(surface.vertices[0]);
    const auto b = transformed(surface.vertices[1]);
    const auto c = transformed(surface.vertices[2]);
    assert(0.5F * std::abs((b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x)) == 0.5F);
    assert(!hidden_skin_geometry_collapses(model));
    surface.vertices[1].bone_indices[0] = 0;
    assert(hidden_skin_geometry_collapses(model)); // two vertices collapse to one point
    surface.vertices[1].bone_indices[0] = 1;
    surface.skin_bones[1] = 0;
    assert(hidden_skin_geometry_collapses(model)); // compare mapped global bone indices
    surface.skin_bones[1] = 1;
    surface.vertices[1].bone_indices[0] = 0;
    surface.vertices[2].bone_weights.fill(0);
    assert(!hidden_skin_geometry_collapses(model)); // even the unmatched vertex needs valid weights
    surface.vertices[2].bone_weights[0] = std::numeric_limits<float>::quiet_NaN();
    assert(!hidden_skin_geometry_collapses(model));
    surface.vertices[2].bone_weights[0] = -1;
    assert(!hidden_skin_geometry_collapses(model));
    surface.vertices[2].bone_weights[0] = 1;
    surface.vertices[2].bone_indices[0] = 99;
    assert(!hidden_skin_geometry_collapses(model));
    surface.vertices[2].bone_indices[0] = 2;
    surface.skin_bones[2] = 99;
    assert(!hidden_skin_geometry_collapses(model));
    surface.skin_bones[2] = 2;
    surface.indices[2] = 3;
    assert(!hidden_skin_geometry_collapses(model));
    surface.indices[2] = 2;
    surface.vertices[2].position.x = std::numeric_limits<float>::infinity();
    assert(!hidden_skin_geometry_collapses(model));
    surface.vertices[2].position.x = 0;
    surface.indices.clear();
    assert(hidden_skin_geometry_collapses(model)); // non-indexed triangle upload
    surface.vertices.push_back({});
    assert(!hidden_skin_geometry_collapses(model)); // incomplete triangle
    auto billboard = rigid_model();
    for (const std::uint32_t mode : {1U, 4U, 6U, 9U}) {
        billboard.bones[2].billboard = mode;
        assert(!hidden_skin_geometry_collapses(billboard));
    }
    auto mixed = rigid_model();
    auto unskinned = mixed.meshes[0];
    unskinned.bone = -1;
    mixed.meshes.push_back(unskinned);
    assert(!hidden_skin_geometry_collapses(mixed));
    mixed.meshes.back().visible = false;
    assert(hidden_skin_geometry_collapses(mixed)); // upload ignores invisible source meshes
    auto invalid = rigid_model();
    invalid.meshes[0].bone = 3;
    assert(!hidden_skin_geometry_collapses(invalid));
    assert(!hidden_skin_geometry_collapses(assets::Model{}));
    auto no_bones = rigid_model();
    no_bones.meshes[0].bone = -1;
    no_bones.meshes[0].submeshes[0].skin_bones = {0};
    for (auto& vertex : no_bones.meshes[0].submeshes[0].vertices) vertex.bone_weights[0] = 1;
    assert(hidden_skin_geometry_collapses(no_bones));
    no_bones.bones.clear();
    assert(!hidden_skin_geometry_collapses(no_bones)); // palette with no skeleton

}
"""


class TerminalWrecks(unittest.TestCase):
    def test_pose_call_sites_workers_fallback_and_ordered_publication(self):
        toolchain = select_toolchain()
        if toolchain is None:
            self.skipTest('requires C++ compiler')
        animation_path = 'apps/viewer/src/space_populate_animation.cpp'
        functions = [function(animation_path, signature) for signature in (
            'void SpacePopulation::pose_units(', 'bool SpacePopulation::pose_live_turrets(')]
        functions.append(function('apps/viewer/src/space_populate.cpp',
                                  'void SpacePopulation::refresh_instances('))
        visibility = function('include/eawr/presentation/space/population_index.hpp',
                              '[[nodiscard]] bool prepare_population_visibility(')
        harness = POSE_HARNESS.replace('__VISIBILITY__', visibility).replace('__FUNCTIONS__', '\n'.join(functions))
        with tempfile.TemporaryDirectory(prefix='eawr-pose-workers-') as temp:
            directory = Path(temp)
            cpp = directory / 'pose_workers.cpp'
            exe = directory / ('pose_workers.exe' if os.name == 'nt' else 'pose_workers')
            cpp.write_text(harness)
            compile_fixture(toolchain, cpp, exe)
            run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)

    def test_independent_material_deformation_vetoes_terminal_geometry(self):
        toolchain = select_toolchain()
        if toolchain is None:
            self.skipTest('requires C++ compiler')
        harness = MATERIAL_GUARD_HARNESS.replace('__FUNCTIONS__', function(
            'src/presentation/godot/renderer_instances.cpp',
            'std::vector<std::uint8_t> GodotRenderer::Impl::skin_geometry_bones(') + '\n' + function(
            'src/presentation/godot/renderer_instances.cpp',
            'std::span<const std::uint8_t> GodotRenderer::Impl::skin_geometry_bones_view('))
        with tempfile.TemporaryDirectory(prefix='eawr-wreck-material-') as temp:
            directory = Path(temp)
            cpp = directory / 'wreck_material.cpp'
            exe = directory / ('wreck_material.exe' if os.name == 'nt' else 'wreck_material')
            cpp.write_text(harness)
            compile_fixture(toolchain, cpp, exe)
            run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)

    def test_hidden_bones_require_uploaded_triangle_collapse(self):
        toolchain = select_toolchain()
        if toolchain is None:
            self.skipTest('requires C++ compiler')
        key = 'INCLUDE' if toolchain.msvc else 'CPLUS_INCLUDE_PATH'
        previous = toolchain.environment.get(key, '')
        toolchain.environment[key] = str(ROOT / 'include') + (os.pathsep + previous if previous else '')
        header = ROOT / 'src/presentation/godot/mesh_hidden_geometry.hpp'
        harness = TOPOLOGY_HARNESS.replace('__HEADER__', header.as_posix())
        with tempfile.TemporaryDirectory(prefix='eawr-wreck-topology-') as temp:
            directory = Path(temp)
            cpp = directory / 'wreck_topology.cpp'
            exe = directory / ('wreck_topology.exe' if os.name == 'nt' else 'wreck_topology')
            cpp.write_text(harness)
            compile_fixture(toolchain, cpp, exe)
            run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)

    def test_ordered_approval_history_cutoff_and_reusable_choice(self):
        toolchain = select_toolchain()
        if toolchain is None:
            self.skipTest('requires C++ compiler')
        path = 'apps/viewer/src/live_session_death.cpp'
        harness = LIFECYCLE_HARNESS.replace('__FUNCTIONS__', '\n'.join(function(path, signature) for signature in (
            'std::optional<LiveSessionView::CloneFrame> LiveSessionView::clone_frame(',
            'void LiveSessionView::retire_clones(')))
        with tempfile.TemporaryDirectory(prefix='eawr-wreck-lifecycle-') as temp:
            directory = Path(temp)
            cpp = directory / 'wreck_lifecycle.cpp'
            exe = directory / ('wreck_lifecycle.exe' if os.name == 'nt' else 'wreck_lifecycle')
            cpp.write_text(harness)
            compile_fixture(toolchain, cpp, exe)
            run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)

    def test_visible_effect_exhaustion_and_hidden_last_frame_drains(self):
        toolchain = select_toolchain()
        if toolchain is None:
            self.skipTest('requires C++ compiler')
        path = 'apps/viewer/src/unit_emitters_frame.cpp'
        harness = EMITTER_HARNESS.replace('__FUNCTIONS__', '\n'.join(function(path, signature) for signature in (
            'void UnitEmitters::orphan_clone(', 'bool UnitEmitters::retire_clone(')))
        with tempfile.TemporaryDirectory(prefix='eawr-wreck-drain-') as temp:
            directory = Path(temp)
            cpp = directory / 'wreck_drain.cpp'
            exe = directory / ('wreck_drain.exe' if os.name == 'nt' else 'wreck_drain')
            cpp.write_text(harness)
            compile_fixture(toolchain, cpp, exe)
            run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)

    def test_terminal_visibility_workers_release_and_reuse(self):
        toolchain = select_toolchain()
        if toolchain is None:
            self.skipTest('requires C++ compiler')
        path = 'apps/viewer/src/space_populate_animation.cpp'
        signatures = ('bool SpacePopulation::live_clip_terminal_hidden(',
                      'std::span<const animation::BonePose> SpacePopulation::terminal_clip_bones(',
                      'std::span<const animation::BonePose> SpacePopulation::held_clip_bones(',
                      'void SpacePopulation::prepare_live_clip(',
                      'bool SpacePopulation::pose_live_clips(',
                      'void SpacePopulation::retire_live_ship(',
                      'void SpacePopulation::restore_live_ship(')
        harness = HARNESS.replace('__FUNCTIONS__', '\n'.join(function(path, signature) for signature in signatures))
        with tempfile.TemporaryDirectory(prefix='eawr-terminal-wreck-') as temp:
            directory = Path(temp)
            cpp = directory / 'terminal_wreck.cpp'
            exe = directory / ('terminal_wreck.exe' if os.name == 'nt' else 'terminal_wreck')
            cpp.write_text(harness)
            compile_fixture(toolchain, cpp, exe)
            run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)


if __name__ == '__main__':
    unittest.main()
