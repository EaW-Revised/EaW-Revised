// Execute the renderer's real bind and skeleton lifetime code with an in-memory RenderingServer.
#include <algorithm>
#include <array>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>
#include "eawr/presentation/skin_pose.hpp"
using namespace eawr::presentation;
namespace sim { using EntityId = std::uint64_t; using AssetId = std::uint64_t; }
struct RID { int id{}; bool is_valid() const { return id != 0; } friend bool operator==(RID, RID) = default; };
struct Transform3D {
    animation::Matrix value{};
    Transform3D() { value[0] = value[5] = value[10] = value[15] = 1; }
    explicit Transform3D(const animation::Matrix& m) : value(m) {}
    friend bool operator==(const Transform3D&, const Transform3D&) = default;
    // Stand-in for the engine product: the test only needs it to differ per operand (value[12] = x translation sum).
    friend Transform3D operator*(const Transform3D& a, const Transform3D& b) {
        Transform3D r; r.value[12] = a.value[12] + b.value[12]; return r;
    }
    friend bool operator==(const Transform3D& a, const animation::Matrix& b) { return a.value == b; }
};
bool operator==(const std::vector<Transform3D>& a, const std::vector<animation::Matrix>& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(),
        [](const auto& x, const auto& y) { return x == y; });
}
Transform3D transform_from(const animation::Matrix& m) { return Transform3D(m); }
struct RenderingServer {
    std::map<int, std::vector<Transform3D>> skeletons;
    std::map<int, int> attached;
    std::map<int, bool> visibility;
    std::map<int, Transform3D> transforms;
    std::map<int, int> transform_writes;
    int next{100};
    static RenderingServer* get_singleton() { static RenderingServer s; return &s; }
    RID skeleton_create() { RID r{++next}; skeletons[r.id] = {}; return r; }
    void skeleton_allocate_data(RID r, int n, bool) { skeletons.at(r.id).resize(n); }
    int skeleton_get_bone_count(RID r) { return static_cast<int>(skeletons.at(r.id).size()); }
    Transform3D skeleton_bone_get_transform(RID r, int n) { return skeletons.at(r.id).at(n); }
    void skeleton_bone_set_transform(RID r, int n, const Transform3D& m) { skeletons.at(r.id).at(n) = m; }
    void instance_attach_skeleton(RID i, RID r) { attached[i.id] = r.id; }
    void instance_set_visible(RID i, bool visible) { visibility[i.id] = visible; }
    void instance_set_transform(RID i, const Transform3D& t) { transforms[i.id] = t; ++transform_writes[i.id]; }
    void free_rid(RID r) {
        const auto erased = skeletons.erase(r.id);
        if (!erased) { const auto removed = attached.erase(r.id); assert(removed == 1); }
    }
};
namespace core { template<class T> struct Result; template<> struct Result<void> {
    bool ok; explicit operator bool() const { return ok; } static Result success() { return {true}; }
}; }
namespace diagnostic_codes { constexpr int invalid_skin_pose = 1; }
core::Result<void> failure(int, const char*) { return {false}; }
namespace hull_batching { inline bool enabled = __BATCHING__; }
enum class ResourceKind { gpu_proxy };
namespace render_churn {
enum class Event { freed };
inline int owner(int, bool = false) { return 0; }
inline void record(int, Event) {}
}
namespace render_categories {
struct Owner { int categories{}; }; inline bool fighter_experiment() { return false; }
inline std::string ablation{"off"}; inline bool enabled = true; inline bool engine_visible(int, bool requested) { return requested; }
inline bool authored_fighter(int) { return false; } inline void begin_fighter_frame(std::uint64_t) {}
struct Work { double binding_cpu_ms{}; int bindings{}; }; inline Work fighter_work;
}
struct Engine { static Engine* get_singleton() { static Engine e; return &e; } std::uint64_t get_process_frames() { return 0; } };
enum class IdleDisturbance : std::uint8_t { missing_instance, bind_failed, asset_mismatch, billboard, other_bind, cleared, copied, removed, replaced };
struct GodotRenderer { struct Impl {
    struct SharedIdleSkeleton { RID rid; std::uint64_t group{}; std::vector<animation::Matrix> palette;
        std::vector<std::uint8_t> valid_bones; ~SharedIdleSkeleton(); };
    struct SharedIdlePlan { std::size_t surfaces{}; std::vector<std::uint8_t> used; };
    struct Instance { sim::AssetId asset_id{}; RID rid, skeleton; std::shared_ptr<SharedIdleSkeleton> shared_idle_skeleton; bool reserved_proxy{}, visible{true};
        bool applied_visible{true}, band_hidden{}, group_hidden{}, part_hidden{}, batch_hidden{}; int category_role{}; std::uint64_t idle_group{};
        Transform3D object_transform; std::uint32_t batch_group{}; std::int32_t rigid_bone{-1}; Transform3D bone_transform; };
    static Transform3D world_transform(const Instance& instance) noexcept {
        return instance.rigid_bone >= 0 ? instance.object_transform * instance.bone_transform : instance.object_transform;
    }
    struct Resource { std::vector<int> billboard_modes; std::vector<std::uint8_t> skin_used_bones;
        bool skinned{true}; std::size_t bone_count{2}; std::int32_t rigid_bone{-1}; };
    struct PendingPose { sim::AssetId asset_id{}; std::vector<animation::Matrix> palette;
        std::shared_ptr<const SkinPosePalette<Transform3D>> prepared; std::uint64_t idle_group{};
        const auto& active_palette() const { return prepared ? prepared->palette : palette; } };
    using InstanceMap = std::map<sim::EntityId, Instance>;
    InstanceMap instances_;
    struct Budget { void release(ResourceKind, std::size_t) {} };
    struct Proxy { std::map<sim::EntityId, std::size_t> charges; Budget* budget{}; } proxy_;
    std::map<sim::AssetId, Resource> resources_;
    std::map<sim::EntityId, PendingPose> skin_poses_;
    std::map<sim::EntityId, render_categories::Owner> diagnostic_owners_;
    std::unordered_map<std::uint64_t, SharedIdlePlan> shared_idle_plans_;
    std::unordered_map<const SkinPosePalette<Transform3D>*, std::shared_ptr<SharedIdleSkeleton>> shared_idle_batch_;
    std::vector<std::uint8_t> skin_changes_;
    SkinPoseWork skin_work_{}; SkinWriteCensus skin_census_{}; bool skin_profile_{}, skin_batch_active_{true};
    void prepare_shared_idle_surface(std::uint64_t, sim::EntityId, sim::AssetId);
    static void release_instance_skeleton(RenderingServer*, Instance&);
    static void detach_shared_idle(RenderingServer&, Instance&);
    core::Result<void> bind_skin_pose(sim::EntityId, sim::AssetId, const Resource&, std::shared_ptr<const SkinPosePalette<Transform3D>>, std::uint64_t);
    core::Result<void> bind_skin_pose_body(sim::EntityId, sim::AssetId, const Resource&, std::shared_ptr<const SkinPosePalette<Transform3D>>, std::uint64_t);
    std::vector<std::uint64_t> idle_disturbed_;
    std::array<std::uint64_t, 9> idle_reasons_{};
    void note_idle_unbound(Instance& instance, const IdleDisturbance reason) {
        if (!instance.idle_group) return;
        idle_disturbed_.push_back(instance.idle_group);
        ++idle_reasons_[static_cast<std::size_t>(reason)];
        instance.idle_group = 0;
    }
    void note_idle_unposed(PendingPose& pose, const IdleDisturbance reason) {
        if (!pose.idle_group) return;
        idle_disturbed_.push_back(pose.idle_group);
        ++idle_reasons_[static_cast<std::size_t>(reason)];
        pose.idle_group = 0;
    }
    void note_idle_missed(const std::uint64_t group, const IdleDisturbance reason) {
        idle_disturbed_.push_back(group);
        ++idle_reasons_[static_cast<std::size_t>(reason)];
    }
    void track_idle_binding(sim::EntityId, sim::AssetId, const Resource&, std::uint64_t, bool);
    bool apply_skin_pose(RenderingServer&, Instance&, const PendingPose&,
        std::span<const std::uint8_t> = {}, SkinPoseWork* = nullptr, SkinWriteCensus* = nullptr, std::size_t = 1);
    void bind_instance_skin(RenderingServer&, Instance&, const Resource&, const PendingPose*, SkinWriteCensus* = nullptr, bool = false);
    void clear_skin_pose(sim::EntityId);
    void set_instance_visible(sim::EntityId, bool);
__VISIBILITY__
    void refresh_billboards(RenderingServer&, sim::EntityId, Instance&, const Resource&) {}
    std::map<sim::EntityId, int> marks;
    void batch_mark(sim::EntityId entity) { ++marks[entity]; }
    void batch_write_transform(sim::EntityId, const Instance&) {}
    // Records what is still alive when a removal tells batching to forget the piece: the order matters.
    struct Forgot { sim::EntityId entity; bool rid_live, skeleton_live; };
    std::vector<Forgot> forgot;
    void batch_forget(RenderingServer* rendering, sim::EntityId entity, Instance& instance) {
        forgot.push_back({entity, rendering && rendering->attached.contains(instance.rid.id),
            rendering && rendering->skeletons.contains(instance.skeleton.id)});
    }
    InstanceMap::iterator remove_instance(RenderingServer*, InstanceMap::iterator);
}; };
__FUNCTIONS__
int main() {
    auto& s = *RenderingServer::get_singleton(); GodotRenderer::Impl r;
    r.resources_[1] = {{}, {1, 1}};
    for (int id = 1; id <= 3; ++id) {
        auto skeleton = s.skeleton_create(); s.skeleton_allocate_data(skeleton, 2, false);
        r.instances_[id] = {1, RID{id}, skeleton, {}}; s.instance_attach_skeleton(RID{id}, skeleton);
    }
    auto make = [](float a, float b) { auto p = std::make_shared<SkinPosePalette<Transform3D>>();
        p->palette.resize(2); p->palette[0].fill(a); p->palette[1].fill(b); return p; };
    auto x = make(1, 2); auto y = make(3, 4);
    std::uint64_t shared_idle_group = 1;
    auto begin = [&] { r.shared_idle_plans_.clear(); r.shared_idle_batch_.clear(); };
    auto plan = [&](int id) { r.prepare_shared_idle_surface(1, id, 1); };
    auto attached = [&](int id) {
        const auto& instance = r.instances_.at(id);
        assert(instance.skeleton.is_valid());
        assert(s.attached.at(instance.rid.id) == instance.skeleton.id);
    };
    auto bind_asset = [&](int id, int asset, auto p) {
        const auto result = r.bind_skin_pose(id, asset, r.resources_.at(asset), p, shared_idle_group);
        assert(result);
        attached(id);
    };
    auto bind = [&](int id, auto p) { bind_asset(id, 1, p); };
    begin(); plan(1); plan(2); plan(3); bind(1, x); bind(2, x); bind(3, x);
    // Hull batching composes with sharing. A fresh pose is a change for batching; an equal
    // palette in a new object is not; a changed palette is. Batching off never marks.
    assert(r.marks.size() == (hull_batching::enabled ? 3U : 0U));
    if (hull_batching::enabled) for (int id = 1; id <= 3; ++id) assert(r.marks.at(id) == 1);
    auto same = make(1, 2);
    begin(); plan(1); plan(2); plan(3); bind(1, same); bind(2, same); bind(3, same);
    if (hull_batching::enabled) for (int id = 1; id <= 3; ++id) assert(r.marks.at(id) == 1);
    // One still-attached surface misses the next update. The other updates the shared skeleton.
    begin(); plan(1); plan(3); bind(1, y); bind(3, y);
    if (hull_batching::enabled) { assert(r.marks.at(1) == 2 && r.marks.at(3) == 2 && r.marks.at(2) == 1); }
    assert(r.instances_.at(1).skeleton == r.instances_.at(3).skeleton);
    // Bind the absent surface independently to the same palette it last retained.
    const auto old_group = r.instances_.at(1).skeleton;
    shared_idle_group = 0; bind(2, x);
    assert(s.attached.at(2) != old_group.id);
    attached(1); attached(3);
    assert(s.skeletons.at(r.instances_.at(2).skeleton.id) == x->palette);
    assert(s.skeletons.at(r.instances_.at(1).skeleton.id) == y->palette);

    // Different surfaces use disjoint bones. Both must get their exact matrices
    // when their usage union changes, including a newly used unchanged bone.
    r.resources_[2] = {{}, {0, 1}};
    r.resources_[1].skin_used_bones = {1, 0};
    r.instances_.at(2).asset_id = 2;
    shared_idle_group = 1; begin(); plan(1); r.prepare_shared_idle_surface(1, 2, 2);
    bind(1, x); bind_asset(2, 2, x);
    auto shared = r.instances_.at(1).skeleton;
    assert(shared == r.instances_.at(2).skeleton);
    assert(s.attached.at(1) == shared.id && s.attached.at(2) == shared.id);
    assert(s.skeletons.at(shared.id) == x->palette);
    // Hidden then visible is represented by a collapsed and restored matrix.
    auto hidden = make(1, 2); std::fill_n(hidden->palette[1].begin(), 12, 0.0F);
    // Only bone 1 moves. Surface 1 uses bone 0 alone: it must not be marked for batching. Surface 2 uses bone 1.
    const int marks_unused = hull_batching::enabled ? r.marks.at(1) : 0, marks_used = hull_batching::enabled ? r.marks.at(2) : 0;
    begin(); plan(1); r.prepare_shared_idle_surface(1, 2, 2);
    bind(1, hidden); bind_asset(2, 2, hidden);
    assert(s.skeletons.at(shared.id) == hidden->palette);
    if (hull_batching::enabled) assert(r.marks.at(1) == marks_unused && r.marks.at(2) == marks_used + 1);
    begin(); plan(1); r.prepare_shared_idle_surface(1, 2, 2);
    bind(1, x); bind_asset(2, 2, x);
    assert(s.skeletons.at(shared.id) == x->palette);
    if (hull_batching::enabled) assert(r.marks.at(1) == marks_unused && r.marks.at(2) == marks_used + 2);
    // Hiding a surface does not stop its shared pose from updating; showing it
    // again must expose the current bone output, not its last visible pose.
    r.set_instance_visible(2, false);
    assert(!s.visibility.at(2));
    begin(); plan(1); r.prepare_shared_idle_surface(1, 2, 2);
    bind(1, y); bind_asset(2, 2, y);
    r.set_instance_visible(2, true);
    assert(s.visibility.at(2));
    assert(s.skeletons.at(r.instances_.at(2).skeleton.id) == y->palette);
    begin(); plan(1); r.prepare_shared_idle_surface(1, 2, 2);
    bind(1, x); bind_asset(2, 2, x);

    // The replacement funnel releases one reference without freeing the other.
    const auto old_attachment = s.attached.at(2);
    r.release_instance_skeleton(&s, r.instances_.at(2));
    assert(!r.instances_.at(2).skeleton.is_valid());
    assert(s.attached.at(2) == old_attachment); // replacement reattaches next
    attached(1);
    assert(s.skeletons.at(shared.id) == x->palette);
    // The replacement constructor must restore the retained pose even if its
    // bytes have not changed since the last update.
    r.bind_instance_skin(s, r.instances_.at(2), r.resources_.at(2), &r.skin_poses_.at(2));
    attached(2);
    auto replacement = r.instances_.at(2).skeleton;
    assert(s.skeletons.at(replacement.id) == x->palette);
    shared_idle_group = 0; r.resources_[2].skin_used_bones = {1, 1};
    auto replacement_pose = make(7, 8);
    bind_asset(2, 2, replacement_pose);
    assert(s.skeletons.at(replacement.id) == replacement_pose->palette);
    assert(s.skeletons.at(shared.id) == x->palette);

    // A live turret or clip writes a separate pose after the shared idle pose.
    shared_idle_group = 1; begin(); plan(1); r.prepare_shared_idle_surface(1, 2, 2);
    bind(1, x); bind_asset(2, 2, x);
    assert(r.instances_.at(1).skeleton == r.instances_.at(2).skeleton);
    auto independent = make(11, 12);
    auto& pending = r.skin_poses_.at(2);
    pending.palette = independent->palette; pending.prepared.reset();
    skin_palette_changes(x->palette, independent->palette, r.skin_changes_, false);
    shared_idle_group = 0;
    bind_asset(2, 2, nullptr);
    assert(r.instances_.at(1).skeleton != r.instances_.at(2).skeleton);
    attached(1); attached(2);
    assert(s.attached.at(1) != s.attached.at(2));
    assert(s.skeletons.at(r.instances_.at(1).skeleton.id) == x->palette);
    assert(s.skeletons.at(r.instances_.at(2).skeleton.id) == independent->palette);
    // Rejoin, then clear only this instance and preserve its sibling's pose.
    shared_idle_group = 1; begin(); plan(1); r.prepare_shared_idle_surface(1, 2, 2);
    bind(1, x); bind_asset(2, 2, x);
    const auto before_clear = r.instances_.at(1).skeleton;
    r.clear_skin_pose(2);
    attached(1); attached(2);
    assert(s.attached.at(2) != before_clear.id);
    assert(!r.skin_poses_.contains(2));
    assert(s.skeletons.at(r.instances_.at(2).skeleton.id) == std::vector<Transform3D>(2));
    assert(s.skeletons.at(r.instances_.at(1).skeleton.id) == x->palette);

    // A newly introduced hardpoint uses a bone absent from the old union;
    // the pose itself has not changed, but that bone must still be uploaded.
    r.resources_[3] = {{}, {1, 0}}; r.resources_[4] = {{}, {1, 0}};
    for (int id = 4; id <= 5; ++id) {
        auto skel = s.skeleton_create(); s.skeleton_allocate_data(skel, 2, false);
        r.instances_[id] = {static_cast<sim::AssetId>(id - 1), RID{id}, skel, {}};
        s.instance_attach_skeleton(RID{id}, skel);
    }
    auto newly_used = make(1, 0); // equals the group's default cached bytes for bone 1
    shared_idle_group = 2; begin(); r.prepare_shared_idle_surface(2, 4, 3);
    r.prepare_shared_idle_surface(2, 5, 4);
    bind_asset(4, 3, newly_used);
    bind_asset(5, 4, newly_used);
    auto union_skeleton = r.instances_.at(4).skeleton;
    assert(s.skeletons.at(union_skeleton.id)[1] != newly_used->palette[1]);
    r.resources_[4].skin_used_bones = {0, 1};
    begin(); r.prepare_shared_idle_surface(2, 4, 3); r.prepare_shared_idle_surface(2, 5, 4);
    bind_asset(4, 3, newly_used);
    bind_asset(5, 4, newly_used);
    assert(s.skeletons.at(union_skeleton.id) == newly_used->palette);
    // The batch owns a temporary reference. Drop it before retiring members.
    begin();
    r.remove_instance(&s, r.instances_.find(4));
    // Batching forgets the piece while its instance and the shared skeleton still exist, then both are released.
    assert(r.forgot.size() == 1 && r.forgot[0].entity == 4 && r.forgot[0].rid_live && r.forgot[0].skeleton_live);
    assert(!s.attached.contains(4));
    assert(s.skeletons.contains(union_skeleton.id));
    attached(5);
    r.remove_instance(&s, r.instances_.find(5));
    assert(r.forgot.size() == 2 && r.forgot[1].entity == 5 && r.forgot[1].rid_live && r.forgot[1].skeleton_live);
    assert(!s.attached.contains(5));
    assert(!s.skeletons.contains(union_skeleton.id));

    // PF idle skin: the renderer reports a placement as soon as one of its instances stops holding its idle palette.
    r.resources_[5] = {{}, {1, 1}};
    for (int id = 10; id <= 12; ++id) {
        auto skel = s.skeleton_create(); s.skeleton_allocate_data(skel, 2, false);
        r.instances_[id] = {5, RID{id}, skel, {}}; s.instance_attach_skeleton(RID{id}, skel);
    }
    // Distinct placements reported since the last call (one cause can report an instance and its retained pose).
    const auto reported = [&] { auto list = r.idle_disturbed_; r.idle_disturbed_.clear();
        std::sort(list.begin(), list.end()); list.erase(std::unique(list.begin(), list.end()), list.end()); return list; };
    const std::vector<std::uint64_t> seven{7};
    r.idle_disturbed_.clear();
    shared_idle_group = 7; begin();
    for (int id = 10; id <= 12; ++id) r.prepare_shared_idle_surface(7, id, 5);
    for (int id = 10; id <= 12; ++id) { bind_asset(id, 5, x); assert(r.instances_.at(id).idle_group == 7); }
    assert(reported().empty());
    // A live pose written over one instance reports the placement once, then stays quiet.
    shared_idle_group = 0; bind_asset(11, 5, y);
    assert(r.instances_.at(11).idle_group == 0 && reported() == seven);
    bind_asset(11, 5, x); assert(reported().empty());
    // Clearing a pose, retiring an instance and an idle bind that lands nowhere all report.
    r.clear_skin_pose(12); assert(r.instances_.at(12).idle_group == 0 && reported() == seven);
    // Retiring an instance reports nothing: its retained pose still is the placement's palette.
    r.remove_instance(&s, r.instances_.find(10)); assert(reported().empty());
    assert(r.skin_poses_.at(10).idle_group == 7);
    shared_idle_group = 7;
    // An idle bind to an entity without an instance keeps the pose and reports nothing; the instance built
    // from that pose later starts out holding the placement's palette.
    const auto missing = r.bind_skin_pose(99, 5, r.resources_.at(5), x, 7);
    assert(missing && reported().empty() && r.skin_poses_.at(99).idle_group == 7);
    r.instances_[99] = {5, RID{99}, RID{}, {}};
    r.bind_instance_skin(s, r.instances_.at(99), r.resources_.at(5), &r.skin_poses_.at(99));
    assert(r.instances_.at(99).idle_group == 7 && s.skeletons.at(r.instances_.at(99).skeleton.id) == x->palette);
    // Entities that never get an instance take the short path: the pose is retained and marked, nothing is applied.
    const auto instances_before = r.instances_.size();
    assert(r.bind_skin_pose(97, 5, r.resources_.at(5), x, 7) && r.skin_poses_.at(97).idle_group == 7);
    assert(r.bind_skin_pose(97, 5, r.resources_.at(5), y, 7) && r.skin_poses_.at(97).prepared == y);
    assert(reported().empty() && r.instances_.size() == instances_before);
    assert(r.bind_skin_pose(97, 5, r.resources_.at(5), x, 0) && r.skin_poses_.at(97).idle_group == 0 && reported() == seven);
    // A billboard surface without an instance is not short-pathed or marked: it is reported like one with an
    // instance, and the instance built later does not count as holding the palette either.
    r.resources_[5].billboard_modes = {1};
    assert(r.bind_skin_pose(96, 5, r.resources_.at(5), x, 7) && r.skin_poses_.at(96).idle_group == 0 && reported() == seven);
    r.instances_[96] = {5, RID{96}, RID{}, {}};
    r.bind_instance_skin(s, r.instances_.at(96), r.resources_.at(5), &r.skin_poses_.at(96));
    assert(r.instances_.at(96).idle_group == 0 && reported().empty());
    r.resources_[5].billboard_modes.clear();
    // A non-idle write clears the pose's and the instance's mark and reports the placement.
    shared_idle_group = 0; bind_asset(99, 5, y);
    assert(r.skin_poses_.at(99).idle_group == 0 && r.instances_.at(99).idle_group == 0 && reported() == seven);
    shared_idle_group = 7;
    // A billboard surface never counts as holding the idle palette, so its placement re-uploads every sample.
    r.resources_[5].billboard_modes = {1};
    begin(); bind_asset(11, 5, x);
    assert(r.instances_.at(11).idle_group == 0 && reported() == seven);
    r.resources_[5].billboard_modes.clear();
    begin(); bind_asset(11, 5, x);
    assert(r.instances_.at(11).idle_group == 7 && reported().empty());
    // Re-binding the same instance under another placement reports the old one.
    shared_idle_group = 8; begin(); bind_asset(11, 5, x);
    assert(r.instances_.at(11).idle_group == 8 && reported() == seven);

    // Direct reference releases also free the group exactly at the last owner.
    const auto group_one = r.instances_.at(1).skeleton;
    r.release_instance_skeleton(&s, r.instances_.at(1));
    assert(s.skeletons.contains(group_one.id));
    attached(3);
    r.release_instance_skeleton(&s, r.instances_.at(3));
    assert(!s.skeletons.contains(group_one.id));

    // Skeleton-free rigid piece: no skeleton, the one bone moves the instance transform, an unchanged bone
    // writes nothing, a bone elsewhere writes nothing, and clearing the pose returns the bone to rest.
    {
        r.resources_[40] = {{}, {0, 1}}; r.resources_[40].rigid_bone = 1;
        r.instances_[140] = {40, RID{140}, RID{}, {}};
        auto& piece = r.instances_.at(140);
        piece.object_transform.value[12] = 100;
        shared_idle_group = 0;
        const auto skeletons_before = s.skeletons.size();
        r.bind_instance_skin(s, piece, r.resources_.at(40), nullptr, nullptr, true);
        assert(piece.rigid_bone == 1 && !piece.skeleton.is_valid() && s.skeletons.size() == skeletons_before);
        auto writes = [&] { return s.transform_writes[140]; };
        auto m = make(0, 5);  // bone 1 moves
        assert(r.bind_skin_pose(140, 40, r.resources_.at(40), m, 0));
        assert(piece.bone_transform == m->palette[1] && writes() == 1);
        assert(s.transforms.at(140) == GodotRenderer::Impl::world_transform(piece) && !(piece.bone_transform == Transform3D()));
        // Same pose again: no write. A pose that moves only bone 0: no write.
        assert(r.bind_skin_pose(140, 40, r.resources_.at(40), make(0, 5), 0) && writes() == 1);
        assert(r.bind_skin_pose(140, 40, r.resources_.at(40), make(9, 5), 0) && writes() == 1);
        // A moved bone: one write, and the engine instance carries object x bone.
        auto moved = make(0, 6);
        assert(r.bind_skin_pose(140, 40, r.resources_.at(40), moved, 0));
        assert(writes() == 2 && piece.bone_transform == moved->palette[1]);
        assert(s.transforms.at(140) == GodotRenderer::Impl::world_transform(piece));
        // A batched member's hidden engine instance is resynced when it leaves, not on each pose.
        piece.batch_group = 3;
        auto moved_again = make(0, 7);
        assert(r.bind_skin_pose(140, 40, r.resources_.at(40), moved_again, 0));
        assert(writes() == 2 && piece.bone_transform == moved_again->palette[1]);
        piece.batch_group = 0;
        r.clear_skin_pose(140);
        assert(piece.bone_transform == Transform3D() && writes() == 3);
        assert(s.transforms.at(140) == GodotRenderer::Impl::world_transform(piece) && s.skeletons.size() == skeletons_before);
        // A retained pose is applied when the instance is built (replacement funnel).
        assert(r.bind_skin_pose(140, 40, r.resources_.at(40), moved, 0));
        piece.rigid_bone = -1; piece.bone_transform = Transform3D();
        r.bind_instance_skin(s, piece, r.resources_.at(40), &r.skin_poses_.at(140), nullptr, true);
        assert(piece.rigid_bone == 1 && piece.bone_transform == moved->palette[1] && !piece.skeleton.is_valid());
        // Without the switch the same resource gets a skeleton.
        piece.rigid_bone = -1; piece.bone_transform = Transform3D();
        r.bind_instance_skin(s, piece, r.resources_.at(40), &r.skin_poses_.at(140), nullptr, false);
        assert(piece.rigid_bone == -1 && piece.skeleton.is_valid());
    }
}
