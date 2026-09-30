#include "renderer_test_support.hpp"

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

} // namespace eawr_renderer_test
