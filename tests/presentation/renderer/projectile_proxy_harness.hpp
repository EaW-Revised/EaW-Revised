// Engine-free stand-ins execute the production renderer proxy transaction.
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <set>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>
#include "eawr/presentation/presentation_budget.hpp"
#include "eawr/presentation/proxy_reservation.hpp"
#include "eawr/presentation/render_churn.hpp"

namespace eawr::presentation::godot_backend {
namespace sim { using EntityId = std::uint64_t; using AssetId = std::uint64_t; }
struct RID { int value{}; bool is_valid() const { return value != 0; } };
struct Transform3D { float values[12]{}; };
struct RenderingServer {
    enum { SHADOW_CASTING_SETTING_OFF };
    std::set<int> live;
    int serial{}, fail_on{};
    static RenderingServer* get_singleton() { static RenderingServer server; return &server; }
    RID instance_create() { return create(); }
    RID skeleton_create() { return create(); }
    RID create() { ++serial; if (serial == fail_on) return {}; live.insert(serial); return {serial}; }
    void free_rid(RID rid) { assert(live.erase(rid.value) == 1); }
    void instance_set_base(RID, RID) {}
    void instance_set_scenario(RID, RID) {}
    void instance_set_visible(RID, bool) {}
    void instance_geometry_set_cast_shadows_setting(RID, int) {}
    std::uint64_t mesh_get_surface_count(RID) { return 1; }
};
class GodotRenderer {
public:
    using ProxyOutcome = presentation::ProxyOutcome;
    using ProxyResult = presentation::ProxyResult;
    struct ProxyInstance { sim::EntityId entity{}, source{}; sim::AssetId asset{}; };
    struct Impl {
        struct Instance {
            sim::AssetId asset_id{}; RID rid{}, skeleton{}; bool visible{}, reserved_proxy{};
            std::uint64_t category_surfaces{};
            render_categories::Role category_role{render_categories::Role::unknown};
            bool category_casts{true};
        };
        using InstanceMap = std::unordered_map<sim::EntityId, Instance>;
        struct Resource { RID mesh{1000}; bool skinned{}; std::size_t bone_count{}; };
        struct Pending { sim::AssetId asset_id{}; };
        RID scenario_{2000};
        InstanceMap instances_;
        std::map<sim::AssetId, Resource> resources_;
        std::map<sim::EntityId, Pending> skin_poses_;
        std::map<sim::EntityId, render_categories::Role> category_roles_;
        std::map<sim::EntityId, int> light_scales_, colorizations_, opacities_;
        std::set<sim::AssetId> non_casting_;
        struct Reservations {
            PayloadBudget fallback;
            PresentationBudget* budget{&fallback};
            std::unordered_map<sim::EntityId, std::size_t> charges;
        } proxy_;
        void copy_instance_pose(sim::EntityId source, sim::EntityId target, float) {
            if (const auto role = category_roles_.find(source); role != category_roles_.end())
                category_roles_.insert_or_assign(target, role->second);
            skin_poses_[target] = {};
            light_scales_[target] = 1;
            colorizations_[target] = 1;
            opacities_[target] = 1;
        }
        static void bind_instance_skin(RenderingServer& server, Instance& instance, const Resource& resource, const Pending*) {
            if (resource.skinned && resource.bone_count) instance.skeleton = server.skeleton_create();
        }
        InstanceMap::iterator remove_instance(RenderingServer*, InstanceMap::iterator);
        void forget_instance_pose(sim::EntityId entity);
    };
    std::unique_ptr<Impl> impl_{std::make_unique<Impl>()};
    ProxyResult reserve_proxy_instances(std::span<const ProxyInstance>, const std::function<bool()>& = {});
    bool set_proxy_budget(PresentationBudget*);
    ResourceBudgetSnapshot proxy_budget() const;
};

__FUNCTIONS__
} // namespace eawr::presentation::godot_backend

int main() {
    using namespace eawr::presentation;
    using namespace eawr::presentation::godot_backend;
    render_categories::enabled = true;
    auto* server = RenderingServer::get_singleton();
    GodotRenderer renderer;
    renderer.impl_->resources_[1] = {{1000}, true, 3};
    renderer.impl_->resources_[2] = {{1001}, false, 0};
    renderer.impl_->category_roles_[100] = render_categories::Role::effect;
    renderer.impl_->non_casting_.insert(2);
    std::vector<GodotRenderer::ProxyInstance> proxies;
    for (std::uint64_t id = 1; id <= 33; ++id) proxies.push_back({id, 100, 1});
    PayloadBudget budget;
    budget.set_limit(ResourceKind::gpu_proxy, 0, true);
    assert(renderer.set_proxy_budget(&budget));
    auto result = renderer.reserve_proxy_instances(proxies);
    assert(result.outcome == ProxyOutcome::refused_budget && result.bytes > 0 && server->live.empty());
    assert(budget.snapshot(ResourceKind::gpu_proxy).reserved_bytes == 0);
    budget.set_limit(ResourceKind::gpu_proxy, 1024 * 1024, true);
    // Refuse after a real instance and skeleton have already been created.
    int attempts = 0;
    result = renderer.reserve_proxy_instances(proxies, [&] { return ++attempts < 3; });
    assert(result.outcome == ProxyOutcome::allocation_failed && server->live.empty());
    assert(renderer.impl_->instances_.empty() && renderer.impl_->skin_poses_.empty());
    assert(renderer.impl_->light_scales_.empty() && renderer.impl_->colorizations_.empty() && renderer.impl_->opacities_.empty());
    assert(budget.snapshot(ResourceKind::gpu_proxy).reserved_bytes == 0);
    // A backend RID failure (separate from budget pressure) rolls back too.
    server->fail_on = server->serial + 4; // second proxy's skeleton
    result = renderer.reserve_proxy_instances(proxies);
    assert(result.outcome == ProxyOutcome::allocation_failed && result.reason == "proxy_skeleton_allocation");
    assert(server->live.empty() && budget.snapshot(ResourceKind::gpu_proxy).reserved_bytes == 0);
    server->fail_on = 0;
    result = renderer.reserve_proxy_instances(proxies);
    assert(result.outcome == ProxyOutcome::reserved && renderer.impl_->instances_.size() == 33);
    assert(server->live.size() == 66 && renderer.impl_->resources_.size() == 2);
    assert(renderer.impl_->instances_.at(1).reserved_proxy);
    assert(renderer.impl_->instances_.at(1).category_role == render_categories::Role::effect);
    assert(renderer.impl_->instances_.at(1).category_casts);
    assert(renderer.proxy_budget().probe_available && renderer.proxy_budget().reserved_bytes == result.bytes);
    assert(!renderer.set_proxy_budget(nullptr));
    const auto held = server->live;
    const auto retained_bytes = result.bytes;
    const auto duplicate = renderer.reserve_proxy_instances(proxies);
    assert(duplicate.outcome == ProxyOutcome::invalid_recipe && server->live == held);
    std::vector<GodotRenderer::ProxyInstance> another{{50, 100, 2}, {51, 100, 2}};
    budget.set_limit(ResourceKind::gpu_proxy, retained_bytes, true);
    result = renderer.reserve_proxy_instances(another);
    assert(result.outcome == ProxyOutcome::refused_budget && server->live == held);
    budget.set_limit(ResourceKind::gpu_proxy, 1024 * 1024, true);
    server->fail_on = server->serial + 2;
    result = renderer.reserve_proxy_instances(another);
    assert(result.outcome == ProxyOutcome::allocation_failed && server->live == held);
    assert(renderer.proxy_budget().reserved_bytes == retained_bytes);
    server->fail_on = 0;
    result = renderer.reserve_proxy_instances(another);
    assert(result.outcome == ProxyOutcome::reserved && server->live.size() == 68);
    assert(renderer.impl_->instances_.at(50).category_role == render_categories::Role::effect);
    assert(!renderer.impl_->instances_.at(50).category_casts);
    // Residency eviction and asset unload both call the production removal
    // funnel directly, bypassing forget_instance_pose. Repeated regrowth must
    // not accumulate a stale charge or silently retain a charged entity.
    const auto instance_owner = static_cast<std::size_t>(render_churn::Owner::instance);
    const auto skeleton_owner = static_cast<std::size_t>(render_churn::Owner::skeleton);
    const auto freed_instances = render_churn::total[instance_owner].freed;
    const auto freed_skeletons = render_churn::total[skeleton_owner].freed;
    for (int cycle = 0; cycle < 2; ++cycle) {
        while (!renderer.impl_->instances_.empty())
            renderer.impl_->remove_instance(server, renderer.impl_->instances_.begin());
        assert(renderer.impl_->proxy_.charges.empty());
        assert(renderer.proxy_budget().reserved_bytes == 0 && server->live.empty());
        result = renderer.reserve_proxy_instances(proxies);
        assert(result.outcome == ProxyOutcome::reserved);
        result = renderer.reserve_proxy_instances(another);
        assert(result.outcome == ProxyOutcome::reserved && server->live.size() == 68);
    }
    assert(render_churn::total[instance_owner].freed - freed_instances == std::uint64_t{70});
    assert(render_churn::total[skeleton_owner].freed - freed_skeletons == std::uint64_t{66});
    for (const auto& proxy : proxies) renderer.impl_->forget_instance_pose(proxy.entity);
    for (const auto& proxy : another) renderer.impl_->forget_instance_pose(proxy.entity);
    assert(server->live.empty() && renderer.proxy_budget().reserved_bytes == 0);
    assert(renderer.set_proxy_budget(nullptr) && !renderer.proxy_budget().probe_available);
}
