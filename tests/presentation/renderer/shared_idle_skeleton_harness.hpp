// Execute the renderer's real bind and skeleton lifetime code with an in-memory RenderingServer.
#include <algorithm>
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
    int next{100};
    static RenderingServer* get_singleton() { static RenderingServer s; return &s; }
    RID skeleton_create() { RID r{++next}; skeletons[r.id] = {}; return r; }
    void skeleton_allocate_data(RID r, int n, bool) { skeletons.at(r.id).resize(n); }
    int skeleton_get_bone_count(RID r) { return static_cast<int>(skeletons.at(r.id).size()); }
    Transform3D skeleton_bone_get_transform(RID r, int n) { return skeletons.at(r.id).at(n); }
    void skeleton_bone_set_transform(RID r, int n, const Transform3D& m) { skeletons.at(r.id).at(n) = m; }
    void instance_attach_skeleton(RID i, RID r) { attached[i.id] = r.id; }
    void instance_set_visible(RID i, bool visible) { visibility[i.id] = visible; }
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
namespace hull_batching { inline bool enabled = false; }
enum class ResourceKind { gpu_proxy };
namespace render_churn {
enum class Event { freed };
inline int owner(int, bool = false) { return 0; }
inline void record(int, Event) {}
}
namespace render_categories {
struct Owner { int categories{}; }; inline bool fighter_experiment() { return false; }
inline std::string ablation{"off"}; inline bool engine_visible(int, bool requested) { return requested; }
inline bool authored_fighter(int) { return false; } inline void begin_fighter_frame(std::uint64_t) {}
struct Work { double binding_cpu_ms{}; int bindings{}; }; inline Work fighter_work;
}
struct Engine { static Engine* get_singleton() { static Engine e; return &e; } std::uint64_t get_process_frames() { return 0; } };
struct GodotRenderer { struct Impl {
    struct SharedIdleSkeleton { RID rid; std::uint64_t group{}; std::vector<animation::Matrix> palette;
        std::vector<std::uint8_t> valid_bones; ~SharedIdleSkeleton(); };
    struct SharedIdlePlan { std::size_t surfaces{}; std::vector<std::uint8_t> used; };
    struct Instance { sim::AssetId asset_id{}; RID rid, skeleton; std::shared_ptr<SharedIdleSkeleton> shared_idle_skeleton; bool reserved_proxy{}, visible{true};
        bool applied_visible{true}, band_hidden{}, group_hidden{}, part_hidden{}, batch_hidden{}; int category_role{}; };
    struct Resource { std::vector<int> billboard_modes; std::vector<std::uint8_t> skin_used_bones;
        bool skinned{true}; std::size_t bone_count{2}; };
    struct PendingPose { sim::AssetId asset_id{}; std::vector<animation::Matrix> palette;
        std::shared_ptr<const SkinPosePalette<Transform3D>> prepared;
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
    SkinPoseWork skin_work_{}; bool skin_profile_{}, skin_batch_active_{true};
    void prepare_shared_idle_surface(std::uint64_t, sim::EntityId, sim::AssetId);
    static void release_instance_skeleton(RenderingServer*, Instance&);
    static void detach_shared_idle(RenderingServer&, Instance&);
    core::Result<void> bind_skin_pose(sim::EntityId, sim::AssetId, const Resource&, std::shared_ptr<const SkinPosePalette<Transform3D>>, std::uint64_t);
    static bool apply_skin_pose(RenderingServer&, const Instance&, const PendingPose&,
        std::span<const std::uint8_t> = {}, SkinPoseWork* = nullptr);
    static void bind_instance_skin(RenderingServer&, Instance&, const Resource&, const PendingPose*);
    void clear_skin_pose(sim::EntityId);
    void set_instance_visible(sim::EntityId, bool);
__VISIBILITY__
    void refresh_billboards(RenderingServer&, sim::EntityId, Instance&, const Resource&) {}
    void batch_mark(sim::EntityId) {}
    void batch_forget(RenderingServer*, sim::EntityId, Instance&) {}
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
    // One still-attached surface misses the next update. The other updates the shared skeleton.
    begin(); plan(1); plan(3); bind(1, y); bind(3, y);
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
    begin(); plan(1); r.prepare_shared_idle_surface(1, 2, 2);
    bind(1, hidden); bind_asset(2, 2, hidden);
    assert(s.skeletons.at(shared.id) == hidden->palette);
    begin(); plan(1); r.prepare_shared_idle_surface(1, 2, 2);
    bind(1, x); bind_asset(2, 2, x);
    assert(s.skeletons.at(shared.id) == x->palette);
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
    assert(!s.attached.contains(4));
    assert(s.skeletons.contains(union_skeleton.id));
    attached(5);
    r.remove_instance(&s, r.instances_.find(5));
    assert(!s.attached.contains(5));
    assert(!s.skeletons.contains(union_skeleton.id));

    // Direct reference releases also free the group exactly at the last owner.
    const auto group_one = r.instances_.at(1).skeleton;
    r.release_instance_skeleton(&s, r.instances_.at(1));
    assert(s.skeletons.contains(group_one.id));
    attached(3);
    r.release_instance_skeleton(&s, r.instances_.at(3));
    assert(!s.skeletons.contains(group_one.id));
}
