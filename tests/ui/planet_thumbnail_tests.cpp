#include "../../apps/viewer/src/galactic/planet_thumbnail_cells.hpp"
#include "ui_test_support.hpp"

#include <cmath>
#include <set>
#include <string>

// PS-08: the live picture atlas binds at most its cells, keeps a row's cell while it stays on
// screen, holds a bounded set of resident models, never drops one a row shows, and never retries
// a failed model (that row keeps the placeholder).
int main() {
    using namespace eawr;
    using Cells = viewer::galactic::PlanetThumbnailCells;
    using test::ui::expect;
    const auto model = [](int index) { return "data/art/models/p" + std::to_string(index) + ".alo"; };

    Cells cells(3, 4);
    cells.begin(1);
    expect(cells.model(model(1)) == Cells::Model::absent, "an unseen model is absent (the caller starts its load)");
    cells.set(model(1), Cells::Model::loading);
    expect(cells.model(model(1)) == Cells::Model::loading, "a load in flight is pending: the row shows the placeholder");
    for (int index = 1; index <= 6; ++index) cells.set(model(index), Cells::Model::ready);

    const auto a = cells.bind(10, model(1)), b = cells.bind(11, model(2)), c = cells.bind(12, model(3));
    expect(a && b && c && std::set<std::size_t>{*a, *b, *c}.size() == 3, "onscreen rows take distinct cells");
    expect(!cells.bind(13, model(4)), "a row beyond the cells gets none (placeholder), the atlas never grows");
    expect(cells.bind(10, model(1)) == a, "asking again in the same frame returns the same cell");
    const auto generation = cells.generation();

    cells.begin(2);
    expect(cells.bind(11, model(2)) == b, "a row that stays on screen keeps its cell across frames");
    expect(cells.generation() == generation, "an unchanged row does not change the bindings (no resubmission)");
    const auto d = cells.bind(13, model(4));
    expect(d && *d != *b, "a new row takes a cell no row used this frame");
    expect(cells.shown(*b) && cells.shown(*d), "this frame's rows are shown");
    std::size_t shown = 0;
    for (std::size_t index = 0; index < cells.cell_count(); ++index) shown += cells.shown(index) ? 1 : 0;
    expect(shown == 2, "rows that scrolled away are not drawn");
    expect(cells.generation() != generation, "a rebinding changes the generation");

    // Residency: six ready models, cap four; the two oldest unused go, the shown ones never do.
    const auto evicted = cells.evict();
    expect(cells.count(Cells::Model::ready) == 4, "ready models are bounded by the resident cap");
    for (const auto& path : evicted) {
        expect(path != model(2) && path != model(4), "a model a row shows this frame is never evicted");
        expect(cells.model(path) == Cells::Model::absent, "an evicted model is absent again (reloaded on demand)");
    }
    expect(evicted.size() == 2, "eviction stops at the cap");

    cells.set(model(7), Cells::Model::failed);
    cells.begin(3);
    expect(cells.model(model(7)) == Cells::Model::failed, "a failed model stays failed: no retry loop, the disc stays");
    static_cast<void>(cells.evict());
    expect(cells.model(model(7)) == Cells::Model::failed, "eviction never forgets a failure");

    // Invalidation: a cleared set returns every ready model to free and forgets all bindings.
    const auto ready_before = cells.count(Cells::Model::ready);
    const auto freed = cells.clear();
    expect(freed.size() == ready_before, "clear returns every resident model for release");
    expect(cells.count(Cells::Model::ready) == 0 && cells.model(model(7)) == Cells::Model::absent, "clear forgets everything");
    for (std::size_t index = 0; index < cells.cell_count(); ++index) expect(!cells.shown(index), "clear unbinds every cell");

    // The cap is at least the cell count, so every onscreen row can stay resident.
    Cells tight(4, 1);
    tight.begin(1);
    for (int index = 1; index <= 4; ++index) { tight.set(model(index), Cells::Model::ready); expect(tight.bind(20 + index, model(index)).has_value(), "bind"); }
    expect(tight.evict().empty(), "rows on screen this frame are all kept even with a smaller requested cap");

    // A row whose planet now draws another model (destroyed) keeps its cell but changes the generation.
    tight.begin(2);
    const auto before = tight.generation();
    tight.set(model(9), Cells::Model::ready);
    const auto kept = tight.bind(21, model(1));
    const auto changed = tight.bind(21, model(9));
    expect(kept == changed && tight.model_of(*changed) == model(9) && tight.generation() != before,
           "a model change on the same row rebinds in place");

    // #7568: a member's resident identity carries the map's surface alternate.
    using viewer::galactic::thumbnail_model_key;
    expect(thumbnail_model_key(model(1), 0) == model(1), "the normal alternate keeps the plain model path");
    expect(thumbnail_model_key(model(1), 2) != model(1) && thumbnail_model_key(model(1), 2) != thumbnail_model_key(model(1), 1),
           "a grid-shell (or camera) alternate is a different resident, so discovery rebinds the row");

    // Framing: the extent's centre and largest half-extent; a lone turning model frames about its pivot.
    using viewer::galactic::thumbnail_extent;
    const auto close_to = [](float a, float b) { return std::abs(a - b) < 1.0e-5F; };
    const auto system = thumbnail_extent({-4.0F, -2.0F, -1.0F}, {6.0F, 2.0F, 1.0F}, false);
    expect(close_to(system.centre[0], 1.0F) && close_to(system.radius, 5.0F), "a still or composite extent frames on its own centre");
    const auto body = thumbnail_extent({-1.0F, -1.0F, -1.0F}, {1.5F, 1.0F, 1.0F}, true);
    expect(close_to(body.centre[0], 0.0F) && close_to(body.radius, 1.5F), "a turning body holding its pivot is centred on the pivot");
    const auto offset = thumbnail_extent({2.0F, -1.0F, -1.0F}, {4.0F, 1.0F, 1.0F}, true);
    expect(close_to(offset.centre[0], 0.0F) && close_to(offset.radius, std::sqrt(18.0F)),
           "a turning model off its pivot frames its whole swing (farthest corner from the pivot)");
    expect(offset.radius >= 4.0F, "every idle pose about the pivot stays inside the cell");
    return test::ui::failures() ? 1 : 0;
}
