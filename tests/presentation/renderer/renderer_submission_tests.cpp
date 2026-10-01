#include "renderer_test_support.hpp"

#include <map>
#include <set>

namespace eawr_renderer_test {

// Missing-asset waits are reported when they start or change asset, then stay
// silent across churn until the entity recovers or leaves the scene.
void missing_asset_wait_contracts() {
    eawr::presentation::godot_backend::detail::MissingAssetWaits waits;
    waits.begin();
    check(waits.wait(7, 404) && waits.wait(8, 405), "a new wait must be reported");
    waits.end();
    for (int frame = 0; frame < 200; ++frame) {
        waits.begin();
        check(!waits.wait(7, 404) && !waits.wait(8, 405),
            "an unchanged wait must not be reported again during churn");
        waits.end();
    }
    check(waits.size() == 2, "two entities must still be waiting");
    waits.begin();
    check(waits.wait(7, 406), "a wait for a different missing asset must be reported");
    waits.end();
    check(waits.size() == 1, "an entity absent from a submit must stop waiting");
    waits.begin();
    check(!waits.wait(7, 406), "the changed wait must then be quiet");
    check(waits.wait(8, 405), "a returning entity's wait must be reported again");
    waits.end();
    waits.begin();
    waits.end();
    check(waits.size() == 0, "an empty submit must end every wait");
}

void pass_order_contracts() {
    using namespace eawr;

    check(presentation::render_pass_order[0] == presentation::RenderPass::opaque,
        "opaque pass must be first");
    check(presentation::render_pass_order[1] == presentation::RenderPass::alpha_tested,
        "alpha-tested pass must follow opaque");
    check(presentation::render_pass_order[2] == presentation::RenderPass::transparent,
        "transparent pass must follow depth-writing passes");
    check(presentation::render_pass_order[3] == presentation::RenderPass::post,
        "post pass must be last");
    check(presentation::render_pass_priority(presentation::RenderPass::opaque)
            < presentation::render_pass_priority(presentation::RenderPass::alpha_tested)
            && presentation::render_pass_priority(presentation::RenderPass::alpha_tested)
                < presentation::render_pass_priority(presentation::RenderPass::transparent)
            && presentation::render_pass_priority(presentation::RenderPass::transparent)
                < presentation::render_pass_priority(presentation::RenderPass::post),
        "every public pass must have a distinct engine submission priority");
    std::vector<presentation::godot_backend::detail::RoutedTransform> distinguishable_draws{
        {{.entity_id = 30, .asset_id = 3}, presentation::RenderPass::transparent},
        {{.entity_id = 40, .asset_id = 4}, presentation::RenderPass::post},
        {{.entity_id = 20, .asset_id = 2}, presentation::RenderPass::alpha_tested},
        {{.entity_id = 10, .asset_id = 1}, presentation::RenderPass::opaque},
    };
    presentation::godot_backend::detail::order_pass_submissions(distinguishable_draws);
    check(distinguishable_draws[0].transform.entity_id == 10
            && distinguishable_draws[1].transform.entity_id == 20
            && distinguishable_draws[2].transform.entity_id == 30
            && distinguishable_draws[3].transform.entity_id == 40,
        "four distinguishable draws must execute through the public pass sequence");
}

void snapshot_adapter_contracts() {
    using namespace eawr;

    sim::math::Mat3x4 matrix{};
    matrix.rows[0] = {fixed(1), fixed(0), fixed(0), fixed(10)};
    matrix.rows[1] = {fixed(0), fixed(1), fixed(0), fixed(-3)};
    matrix.rows[2] = {fixed(0), fixed(0), fixed(1), fixed(7)};
    const auto snapshot = std::make_shared<const sim::RenderSnapshot>(
        42, std::vector<sim::RenderInstance>{{17, 91, matrix}});
    const auto converted = presentation::adapt_snapshot(*snapshot);
    check(converted.size() == 1, "one immutable snapshot entity must adapt once");
    check(converted[0].entity_id == 17 && converted[0].asset_id == 91,
        "stable entity and asset IDs must survive adaptation");
    check(converted[0].column_major[0] == 1.0F
            && converted[0].column_major[5] == 1.0F
            && converted[0].column_major[10] == 1.0F
            && converted[0].column_major[15] == 1.0F,
        "Q24 identity basis must become a float identity basis");
    check(converted[0].column_major[12] == 10.0F
            && converted[0].column_major[13] == -3.0F
            && converted[0].column_major[14] == 7.0F,
        "Q24 translation must convert only at the presentation boundary");
    check(snapshot->instances()[0].fixed_transform == matrix,
        "snapshot adaptation must not mutate simulation data");
}

// #888: the kept pass order is the order_pass_submissions order, rebuilt only
// when the pieces or the uploads change, and a frame sends only the pieces
// that moved. Counts, never wall-clock time.
void submission_plan_contracts() {
    using namespace eawr;
    namespace detail = presentation::godot_backend::detail;
    using presentation::RenderPass;

    // Pass of an asset: a fixed mix of the four passes; asset 0 is not uploaded.
    const auto pass_of = [](const sim::AssetId asset) -> std::optional<RenderPass> {
        if (asset == 0) return std::nullopt;
        return presentation::render_pass_order[asset % 4];
    };
    const auto matrix_at = [](const std::int64_t x) {
        sim::math::Mat3x4 matrix{};
        matrix.rows[0] = {fixed(1), fixed(0), fixed(0), fixed(x)};
        matrix.rows[1] = {fixed(0), fixed(1), fixed(0), fixed(0)};
        matrix.rows[2] = {fixed(0), fixed(0), fixed(1), fixed(0)};
        return matrix;
    };

    // The linear order equals the reference stable sort on a shuffled list
    // with repeated entities and a missing asset.
    std::vector<sim::RenderInstance> instances;
    std::uint64_t state = 12345U;
    for (std::uint32_t index = 0; index < 1000; ++index) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        const auto entity = static_cast<sim::EntityId>((state >> 33U) % 700U);
        const auto asset = static_cast<sim::AssetId>((state >> 13U) % 9U);
        instances.push_back({entity, asset, matrix_at(index)});
    }
    detail::SubmitWork work;
    detail::SubmissionOrder order;
    check(order.plan(instances, 1, pass_of, work), "the first snapshot must build the order");
    std::vector<detail::RoutedTransform> reference;
    for (const sim::RenderInstance& instance : instances) {
        reference.push_back({{.entity_id = instance.entity_id, .asset_id = instance.asset_id},
            pass_of(instance.asset_id).value_or(RenderPass::post)});
    }
    detail::order_pass_submissions(reference);
    const auto pieces = order.pieces();
    bool same = pieces.size() == reference.size();
    for (std::size_t index = 0; same && index < pieces.size(); ++index) {
        const sim::RenderInstance& source = instances[pieces[index].source];
        same = source.entity_id == reference[index].transform.entity_id
            && source.asset_id == reference[index].transform.asset_id && pieces[index].pass == reference[index].pass
            && pieces[index].uploaded == (source.asset_id != 0);
    }
    check(same, "the linear pass order must equal order_pass_submissions, ties by snapshot order");

    // Moving pieces keep the order; a new piece, a different asset or a new upload rebuild it.
    for (sim::RenderInstance& instance : instances) instance.fixed_transform = matrix_at(7);
    check(!order.plan(instances, 1, pass_of, work), "moved pieces must reuse the order");
    check(order.plan(instances, 2, pass_of, work), "a changed upload must rebuild the order");
    instances[10].asset_id = instances[10].asset_id == 5 ? 6 : 5;
    check(order.plan(instances, 2, pass_of, work), "a piece bound to another asset must rebuild the order");
    instances.push_back({900, 1, matrix_at(0)});
    check(order.plan(instances, 2, pass_of, work), "an added piece must rebuild the order");
    check(!order.plan(instances, 2, pass_of, work), "an unchanged snapshot must reuse the order");
    order.invalidate();
    check(order.plan(instances, 2, pass_of, work), "an invalidated order must be rebuilt");
    check(work.orders_built == 5, "five of the seven plans must have built the order");

    // An entity-sorted snapshot needs no sort in any pass bucket.
    std::vector<sim::RenderInstance> sorted;
    for (std::uint32_t entity = 1; entity <= 400; ++entity) {
        sorted.push_back({entity, static_cast<sim::AssetId>(1 + entity % 8), matrix_at(entity)});
    }
    detail::SubmitWork sorted_work;
    detail::SubmissionOrder sorted_order;
    static_cast<void>(sorted_order.plan(sorted, 1, pass_of, sorted_work));
    check(sorted_work.order_sorts == 0, "an entity-ordered snapshot must be split without a sort");

    // A battle frame: 1000 pieces, 40 moving by interpolation, the rest parked.
    std::vector<detail::PlacedPiece> placed(1000);
    std::vector<sim::math::Mat3x4> frame(1000);
    for (std::size_t index = 0; index < frame.size(); ++index) frame[index] = matrix_at(static_cast<std::int64_t>(index));
    std::size_t sent{};
    for (std::size_t index = 0; index < frame.size(); ++index) sent += detail::place_piece(placed[index], false, frame[index]);
    check(sent == 1000, "every new piece must be sent once");
    for (int step = 1; step <= 30; ++step) {
        sent = 0;
        for (std::size_t index = 0; index < 40; ++index) frame[index] = matrix_at(static_cast<std::int64_t>(index) + step);
        for (std::size_t index = 0; index < frame.size(); ++index) {
            sent += detail::place_piece(placed[index], false, frame[index]);
        }
        check(sent == 40, "a frame must send only its 40 moving pieces");
    }
    check(detail::place_piece(placed[500], true, frame[500]), "a piece rebound to another asset must be sent");
    check(!detail::place_piece(placed[500], false, frame[500]), "a rebound piece that stays must not be sent again");
}

void submission_presence_contracts() {
    using namespace eawr;
    namespace detail = presentation::godot_backend::detail;
    using presentation::RenderPass;

    // Engine-free resources stand in for RIDs; order, reconciliation and all
    // stamp/count/sweep decisions are the helpers used by production submit.
    struct Instance final {
        sim::AssetId asset_id{};
        std::uint64_t rid{};
        detail::PlacedPiece placement{};
    };
    std::map<sim::EntityId, Instance> instances;
    std::set<std::uint64_t> allocated;
    std::vector<std::uint64_t> freed;
    std::map<sim::AssetId, RenderPass> uploads{{1, RenderPass::opaque}};
    detail::SubmissionOrder order;
    detail::SubmitWork work;
    std::uint64_t serial{};
    std::uint64_t next_rid{};
    const auto remove_instance = [&](const auto current) {
        check(allocated.erase(current->second.rid) == 1, "each removed instance resource must be freed once");
        freed.push_back(current->second.rid);
        return instances.erase(current);
    };
    const auto submit = [&](const std::vector<sim::RenderInstance>& snapshot) {
        static_cast<void>(order.plan(snapshot, 1, [&](const sim::AssetId asset) -> std::optional<RenderPass> {
            const auto found = uploads.find(asset);
            return found == uploads.end() ? std::nullopt : std::optional{found->second};
        }, work));
        detail::SubmissionPresence presence(++serial);
        for (const detail::PlannedPiece& piece : order.pieces()) {
            const sim::RenderInstance& source = snapshot[piece.source];
            auto instance = instances.find(source.entity_id);
            const auto transition = detail::reconcile_instance(
                instance == instances.end() ? std::nullopt : std::optional{instance->second.asset_id},
                piece.uploaded ? std::optional{source.asset_id} : std::nullopt);
            if (!piece.uploaded) {
                if (transition == detail::InstanceTransition::remove) {
                    presence.remove(instance->second.placement);
                    static_cast<void>(remove_instance(instance));
                }
                continue;
            }
            if (transition == detail::InstanceTransition::create) {
                const std::uint64_t rid = ++next_rid;
                allocated.insert(rid);
                instance = instances.emplace(source.entity_id, Instance{source.asset_id, rid, {}}).first;
            } else if (transition == detail::InstanceTransition::replace) {
                instance->second.asset_id = source.asset_id;
            }
            presence.retain(instance->second.placement);
        }
        presence.sweep(instances, remove_instance, work);
    };
    sim::math::Mat3x4 identity{};
    for (std::size_t axis = 0; axis < 3; ++axis) identity.rows[axis][axis] = fixed(1);

    submit({{2, 1, identity}});
    const std::uint64_t absent_rid = instances.at(2).rid;
    submit({{1, 1, identity}, {1, 999, identity}});
    check(instances.empty() && allocated.empty(),
        "removing a stamped duplicate must also sweep the absent entity");
    check(freed == std::vector<std::uint64_t>{2, absent_rid} && work.sweeps == 1,
        "the duplicate and absent entity must each free their instance resource");
    submit({{1, 1, identity}, {1, 999, identity}});
    check(instances.empty() && allocated.empty() && work.sweeps == 1,
        "repeating the missing duplicate must leave no resources and need no stale-instance sweep");

    // Missing assets are post-pass entries. A post-pass upload lets the same
    // entity remove, recreate and remove again in stable snapshot order.
    uploads.emplace(3, RenderPass::post);
    submit({{2, 1, identity}});
    const std::uint64_t stale_rid = instances.at(2).rid;
    const std::size_t freed_before = freed.size();
    submit({{1, 3, identity}, {1, 999, identity}, {1, 3, identity}, {1, 998, identity}});
    check(instances.empty() && allocated.empty() && freed.size() == freed_before + 3,
        "remove/recreate/remove must free both new resources and sweep the absent entity");
    check(freed.back() == stale_rid && work.sweeps == 2,
        "multiple stamped removals must not suppress the absent entity's sweep");

    submit({{1, 1, identity}, {2, 1, identity}});
    submit({{1, 1, identity}, {1, 1, identity}, {2, 1, identity}});
    check(instances.size() == 2 && allocated.size() == 2 && work.sweeps == 2,
        "retained duplicates count once and a fully retained snapshot skips the sweep");
    submit({{1, 999, identity}, {2, 1, identity}});
    check(instances.size() == 1 && instances.contains(2) && work.sweeps == 2,
        "removing an instance not yet stamped must not decrement the retained count");
    submit({});
    check(instances.empty() && allocated.empty() && work.sweeps == 3,
        "an empty snapshot must sweep and free all remaining resources");
}

} // namespace eawr_renderer_test
