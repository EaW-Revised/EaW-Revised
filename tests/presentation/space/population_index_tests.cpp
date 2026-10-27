#include "../../support/check.hpp"
#include "eawr/presentation/space/population_index.hpp"
#include "eawr/presentation/skin_pose.hpp"

#include <atomic>
#include <bit>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <thread>
#include <vector>

namespace {
namespace space = eawr::presentation::space;
namespace math = eawr::sim::math;
using eawr::sim::EntityId;

eawr::testing::Checker local_checker;
int& failures = local_checker.failures;
void expect(const bool condition, const char* message) {
    if (!condition) local_checker.check_before_print(condition, message, std::cerr, "FAIL: ");
}

struct Piece final {
    eawr::sim::RenderInstance instance;
    std::optional<std::size_t> live;
    math::Mat3x4 local{math::identity_matrix()};
    bool shield{};
    bool attached{};
};
struct Clip final { std::size_t ship{}; };
const auto hull = [](const Piece& piece) { return !piece.shield && !piece.attached; };

void test_linear_frame_budget() {
    // Interleaved surfaces exercise the index independently of composition order.
    for (const std::size_t ships : {64U, 128U, 256U, 512U}) {
        std::vector<Piece> pieces;
        std::vector<Clip> clips;
        for (std::size_t surface = 0; surface < 7; ++surface) {
            for (std::size_t ship = 0; ship < ships; ++ship) {
                pieces.push_back({{1 + 13 * pieces.size(), 1, {}}, ship, {}, surface == 6, surface == 5});
                if (surface < 2) clips.push_back({ship});
            }
        }
        pieces.push_back({{999999, 1, {}}, std::nullopt}); // a static map piece
        space::PopulationIndex index;
        index.rebuild(ships, pieces, clips, hull);
        expect(index.work().rebuilt_pieces == 7 * ships + 1, "composition indexes each piece once");
        for (unsigned frame = 0; frame < 8; ++frame) {
            const auto before = index.work();
            for (std::size_t ship = 0; ship < ships; ++ship) {
                expect(index.entities(ship).size() == 7, "fog includes hull attachment and shell surfaces");
                expect(index.entities(ship, true).size() == 5, "light flash excludes attached models and shells");
                expect(index.pieces(ship).size() == 7 && index.clips(ship).size() == 2,
                       "piece and clip requests visit only the selected ship");
            }
            const auto after = index.work();
            expect(after.entity_visits - before.entity_visits == 12 * ships
                   && after.piece_visits - before.piece_visits == 7 * ships
                   && after.clip_visits - before.clip_visits == 2 * ships,
                   "a frame scans linear work at every fleet size, never ships times population");
            expect(after.rebuilt_pieces == before.rebuilt_pieces, "frames never rebuild membership");
        }
        expect(index.entities(ships).empty() && index.clips(ships).empty() && index.pieces(ships).empty(),
               "unknown ships have no neighbouring membership");
        expect(index.opacity_changed(0, 1.0F) && !index.opacity_changed(0, 1.0F),
               "first opacity initializes potentially reused entity IDs, then settled ships skip renderer calls");
        expect(index.opacity_changed(0, .25F) && !index.opacity_changed(0, .25F)
               && index.opacity_changed(0, .5F) && index.opacity_changed(0, 1.0F)
               && !index.opacity_changed(0, 1.0F), "fade changes propagate once including restoration to opaque");
        expect(index.work().opacity_changes == 4, "settled opacity has no per-piece update work");

        const EntityId survivor = index.entities(ships - 1).front();
        index.reset_opacity(0);
        expect(index.opacity_changed(0, 1.0F), "explicit retirement forgets cached opacity for reused slots");
        expect(index.opacity_changed(ships - 1, .5F), "survivor starts fading");
        std::erase_if(pieces, [](const Piece& piece) { return piece.live == 0; });
        std::erase_if(clips, [](const Clip& clip) { return clip.ship == 0; });
        index.rebuild(ships, pieces, clips, hull);
        expect(index.entities(0).empty() && index.clips(0).empty() && index.pieces(0).empty(),
               "retirement removes every borrowed piece and clip index");
        expect(index.entities(ships - 1).front() == survivor && !index.opacity_changed(ships - 1, .5F),
               "compaction preserves survivor IDs and already-applied opacity");
        for (const auto piece : index.pieces(ships - 1)) expect(pieces[piece].live == ships - 1, "compacted indices are current");
        for (const auto clip : index.clips(ships - 1)) expect(clips[clip].ship == ships - 1, "compacted clip indices are current");
        index.clear();
        index.rebuild(ships, pieces, clips, hull);
        expect(index.opacity_changed(1, 1.0F), "recompose explicitly restores opaque state for reused IDs");
        expect(index.opacity_changed(ships - 1, .5F), "release and recompose invalidate opacity");
    }
}

void test_attachment_marks() {
    const std::vector<std::vector<unsigned>> states{{0, 0}, {}, {0, 0, 0}};
    space::AttachmentMarks marks;
    marks.configure(states);
    for (unsigned frame = 0; frame < 8; ++frame) {
        marks.clear();
        marks.mark(2, 1); marks.mark(0, 0); marks.mark(2, 1); marks.mark(0, 0); marks.mark(2, 2);
        expect(marks.count() == 3, "nonadjacent surfaces count one attachment per decision and hardpoint");
        marks.clear();
        marks.mark(2, 2);
        expect(marks.count() == 1, "hidden and destroyed attachments leave no stale marks next frame");
    }
}

class Executor final : public eawr::presentation::particles::StepExecutor {
public:
    explicit Executor(const std::size_t workers) : workers_(workers) {}
    bool run(const std::size_t count, const std::function<void(std::size_t)>& task) const override {
        ++runs;
        std::atomic<std::size_t> next{};
        const auto work = [&] {
            for (auto i = next.fetch_add(1); i < count; i = next.fetch_add(1)) { task(i); ++tasks; }
        };
        std::vector<std::thread> threads;
        for (std::size_t i = 1; i < workers_; ++i) threads.emplace_back(work);
        work();
        for (auto& thread : threads) thread.join();
        return !fail;
    }
    bool fail{};
    mutable std::size_t runs{};
    mutable std::atomic<std::size_t> tasks{};
private:
    std::size_t workers_{};
};

void test_parallel_visibility_and_palettes() {
    using namespace eawr::presentation;
    namespace animation = eawr::presentation::animation;
    constexpr std::size_t count = 513; // uneven final visibility batch
    std::vector<std::vector<animation::BonePose>> source(count);
    std::vector<SkinPosePalette<animation::Matrix>> reference(count);
    const auto identity = [](const animation::Matrix& matrix) { return matrix; };
    for (std::size_t unit = 0; unit < count; ++unit) {
        source[unit].resize(1 + unit % 31);
        for (std::size_t bone = 0; bone < source[unit].size(); ++bone) {
            auto& row = source[unit][bone];
            row.skin_asset = row.model_asset = animation::Player::identity_matrix();
            row.skin_asset[12] = static_cast<float>(unit * 17 + bone);
            row.model_asset[13] = static_cast<float>(bone * 3);
            row.model_asset[1] = unit % 2 ? -0.0F : 0.0F;
            row.visible = (unit + bone) % 5 != 0;
        }
        store_skin_pose(source[unit], reference[unit].palette, reference[unit].model_transforms, identity);
    }
    const auto exact = [](const auto& left, const auto& right) {
        if (left.size() != right.size()) return false;
        for (std::size_t bone = 0; bone < left.size(); ++bone)
            if (std::bit_cast<std::array<std::uint32_t, 16>>(left[bone])
                != std::bit_cast<std::array<std::uint32_t, 16>>(right[bone])) return false;
        return true;
    };
    const auto visible = [](const std::size_t index) { return index % 7 != 1 && index % 11 != 3; };
    std::vector<std::uint8_t> expected;
    expect(space::prepare_population_visibility(count, expected, nullptr, visible), "serial visibility prepares");
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        Executor executor(workers);
        std::vector<std::uint8_t> shown;
        expect(space::prepare_population_visibility(count, shown, &executor, visible)
            && shown == expected && executor.tasks == 9, "visibility is byte-identical with one bounded task per range");
        std::vector<std::shared_ptr<const SkinPosePalette<animation::Matrix>>> palettes(count);
        std::vector<SkinPoseWork> work(count);
        std::vector<std::size_t> converted(count, 0);
        const auto prepare = [&](const std::size_t index) {
            const auto convert = [&](const animation::Matrix& matrix) { ++converted[index]; return matrix; };
            palettes[index] = prepare_skin_palette<animation::Matrix>(source[index], convert, &work[index]);
        };
        expect(executor.run(count, prepare), "independent palettes prepare on all worker counts");
        bool identical = true;
        for (std::size_t unit = 0; unit < count; ++unit) {
            identical = identical && palettes[unit] && work[unit].builds == 1 && work[unit].validations == 1
                && converted[unit] == source[unit].size() // model values only; skin conversion stays changed-only
                && exact(palettes[unit]->palette, reference[unit].palette)
                && exact(palettes[unit]->model_transforms, reference[unit].model_transforms);
        }
        expect(identical, "worker palettes retain exact serial bytes including hidden bones and signed zero");
        // Reuse and retire input storage; published palettes must own all their bytes.
        const auto retained = palettes[0];
        auto replacement = source[0];
        replacement[0].visible = !replacement[0].visible;
        palettes[0] = prepare_skin_palette<animation::Matrix>(replacement, identity);
        replacement.clear();
        expect(retained && exact(retained->palette, reference[0].palette), "published palettes outlive mutable input samples");
        auto bad = source[1];
        bad[0].model_asset[3] = std::numeric_limits<float>::infinity();
        expect(!prepare_skin_palette<animation::Matrix>(bad, identity), "non-finite worker palette rejects before publication");
        executor.fail = true;
        expect(!space::prepare_population_visibility(count, shown, &executor, visible), "visibility executor failure is explicit");
        executor.fail = false;
        expect(space::prepare_population_visibility(0, shown, &executor, visible) && shown.empty(), "retirement clears stale visibility rows");
    }
}

void test_exact_parallel_compose() {
    std::vector<std::optional<math::Mat3x4>> transforms(8, math::identity_matrix());
    transforms[3].reset(); // hidden ship retains its previous piece transforms
    for (std::size_t ship = 0; ship < transforms.size(); ++ship) {
        if (!transforms[ship]) continue;
        transforms[ship]->rows[0][0] = math::Fixed::from_raw(-math::Fixed::scale);
        transforms[ship]->rows[0][3] = math::Fixed::from_raw(static_cast<std::int64_t>(ship) * 17);
    }
    std::vector<Piece> initial(513);
    std::vector<eawr::sim::RenderInstance> expected;
    for (std::size_t index = 0; index < initial.size(); ++index) {
        auto& piece = initial[index];
        piece.instance = {1 + index * 11, 1, math::identity_matrix()};
        if (index % 9 != 8) piece.live = index % 8;
        piece.local.rows[0][3] = math::Fixed::from_raw(static_cast<std::int64_t>(index) * 31);
        auto result = piece.instance;
        if (piece.live && transforms[*piece.live]) {
            result.fixed_transform = math::compose(*transforms[*piece.live], piece.local).value();
        }
        expected.push_back(result);
    }
    const auto immutable = transforms;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        Executor executor(workers);
        auto pieces = initial;
        std::vector<std::uint8_t> errors;
        const auto identity = [](const math::Mat3x4& value) { return value; };
        expect(space::compose_population_pieces(std::span<Piece>(pieces), transforms, errors, &executor, identity),
               "parallel exact piece composition completes");
        expect(executor.runs == 1 && executor.tasks == 9, "partition covers all pieces once including the last partial batch");
        for (std::size_t index = 0; index < pieces.size(); ++index) {
            expect(pieces[index].instance == expected[index] && errors[index] == 0,
                   "worker count preserves every raw Q24 transform and static or hidden piece");
        }
        expect(transforms == immutable, "composition never writes ship transforms used by effects and picking");
        const auto* scratch = errors.data();
        pieces.erase(pieces.begin() + 2, pieces.end());
        expect(space::compose_population_pieces(std::span<Piece>(pieces), transforms, errors, &executor, identity)
               && errors.data() == scratch && executor.runs == 1, "small frames retain scratch and stay inline");
        pieces.resize(513, initial.front());
        transforms[0] = math::identity_matrix();
        transforms[0]->rows[0][0] = math::Fixed::from_raw(std::numeric_limits<std::int64_t>::max());
        pieces[0].live = 0;
        pieces[0].local.rows[0][0] = math::Fixed::from_raw(2 * math::Fixed::scale);
        expect(space::compose_population_pieces(std::span<Piece>(pieces), transforms, errors, &executor, identity)
               && errors[0] == 1, "overflow is staged for ordered main-thread diagnostics");
        executor.fail = true;
        expect(!space::compose_population_pieces(std::span<Piece>(pieces), transforms, errors, &executor, identity),
               "executor failure reaches the caller");
        transforms = immutable;
    }
}
} // namespace

int main() {
    test_linear_frame_budget();
    test_attachment_marks();
    test_exact_parallel_compose();
    test_parallel_visibility_and_palettes();
    if (failures) return 1;
    std::cout << "population piece index contracts passed\n";
}
