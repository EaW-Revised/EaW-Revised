// ASD-01: an animation or model the file system holds but the loaders refuse (an empty file, a file with no root group, a
// file cut short) is a degradation the caller reports once, never an error the caller escalates. These tests cover the
// shared helper the galaxy scene, the battle start and the environment view call: the refusal stays in the parser, the
// helper hands back "no clip" plus one load note, and the degradation ledger counts that note under the mod that supplied
// the file.
#include "../ui/ui_test_support.hpp"
#include "eawr/assets/asset_degrade.hpp"
#include "eawr/core/degradation_ledger.hpp"
#include "eawr/presentation/animation/degraded_clips.hpp"

#include <array>
#include <bit>
#include <string>
#include <cstdint>

namespace {
using namespace eawr;
using eawr::test::ui::expect;
using Bytes = std::vector<std::byte>;

void u8(Bytes& out, const std::uint8_t v) { out.push_back(static_cast<std::byte>(v)); }
void u32(Bytes& out, const std::uint32_t v) { for (unsigned s = 0; s < 32; s += 8) u8(out, (v >> s) & 255U); }
void f32(Bytes& out, const float v) { u32(out, std::bit_cast<std::uint32_t>(v)); }
void add(Bytes& out, const Bytes& value) { out.insert(out.end(), value.begin(), value.end()); }
Bytes chunk(const std::uint32_t type, const Bytes& payload, const bool group = false) {
    Bytes out; u32(out, type); u32(out, static_cast<std::uint32_t>(payload.size()) | (group ? 0x80000000U : 0U)); add(out, payload); return out;
}
void mini(Bytes& out, const std::uint8_t type, const Bytes& payload) { u8(out, type); u8(out, static_cast<std::uint8_t>(payload.size())); add(out, payload); }
Bytes integer(const std::uint32_t v) { Bytes out; u32(out, v); return out; }

// The smallest well-formed ALA: one frame at 30 fps, no tracks (built from the public chunk grammar).
Bytes good_animation() {
    Bytes info; mini(info, 1, integer(1)); Bytes rate; f32(rate, 30.0F); mini(info, 2, rate); mini(info, 3, integer(0));
    Bytes root; add(root, chunk(0x1001, info));
    return chunk(0x1000, root, true);
}
// A well-formed chunk that is not the animation root: "ALA requires one 0x1000 root group".
Bytes no_root_group() { return chunk(0x2000, integer(7), true); }

void str(Bytes& out, const char* v) { while (*v) u8(out, static_cast<std::uint8_t>(*v++)); u8(out, 0); }
// A one-bone model (ROOT), built from the public chunk grammar.
Bytes model_bytes() {
    Bytes count; u32(count, 1); Bytes bone; Bytes name; str(name, "ROOT"); add(bone, chunk(0x203, name));
    Bytes data; u32(data, std::bit_cast<std::uint32_t>(std::int32_t{-1})); u32(data, 1);
    for (int i = 0; i < 12; ++i) f32(data, (i == 0 || i == 4 || i == 8) ? 1.0F : 0.0F);
    add(bone, chunk(0x205, data));
    Bytes skeleton; add(skeleton, chunk(0x201, count)); add(skeleton, chunk(0x202, bone, true));
    Bytes header; mini(header, 1, integer(0)); mini(header, 4, integer(0));
    Bytes connections; add(connections, chunk(0x601, header));
    Bytes file; add(file, chunk(0x200, skeleton, true)); add(file, chunk(0x600, connections, true));
    return file;
}
// Two stored frames (one playable) and no tracks: loads, binds to any model.
Bytes bindable_animation_bytes() {
    Bytes info; mini(info, 1, integer(2)); Bytes rate; f32(rate, 30.0F); mini(info, 2, rate); mini(info, 3, integer(0));
    Bytes root; add(root, chunk(0x1001, info));
    return chunk(0x1000, root, true);
}
// Parses, but its one track names a bone the one-bone model does not have, so it cannot bind to it.
assets::Animation misfit_animation() {
    assets::Animation clip;
    clip.source.logical_path = "data/art/models/w_misfit_idle_00.ala";
    clip.stored_frame_count = 2; clip.playable_frame_count = 1; clip.frames_per_second = 30.0F;
    clip.duration_seconds = 1.0F / 30.0F;
    assets::AnimationTrack track; track.bone_name = "NO_SUCH_BONE"; track.bone_index = 7; track.samples.resize(2);
    clip.tracks.push_back(std::move(track));
    return clip;
}

constexpr std::string_view dir = "data/art/models/";

void refusals_become_one_note() {
    eawr::test::ui::TempTree tree("asset-degrade");
    const auto models = tree.root / "art/models";
    eawr::test::ui::write_bytes(models / "w_empty_idle_00.ala", {});
    eawr::test::ui::write_bytes(models / "w_noroot_idle_00.ala", no_root_group());
    auto truncated = good_animation(); truncated.resize(truncated.size() - 3);
    eawr::test::ui::write_bytes(models / "w_cut_idle_00.ala", truncated);
    eawr::test::ui::write_bytes(models / "w_good_idle_00.ala", good_animation());
    eawr::test::ui::write_bytes(models / "w_empty.alo", {});
    eawr::test::ui::write_bytes(models / "w_planet.alo", model_bytes());
    eawr::test::ui::write_bytes(models / "w_planet_idle_00.ala", bindable_animation_bytes());
    eawr::test::ui::write_bytes(models / "w_planet_empty_idle_00.ala", {});
    const std::array mounts{vfs::MountSpec{"mod", tree.root, "data", {}}};
    auto mounted = vfs::Vfs::mount(mounts);
    expect(static_cast<bool>(mounted), "the degrade fixture VFS mounts");
    if (!mounted) return;
    const vfs::Vfs& filesystem = mounted.value();

    // The parser keeps refusing the files.
    for (const char* name : {"w_empty_idle_00.ala", "w_noroot_idle_00.ala", "w_cut_idle_00.ala"})
        expect(!assets::load_animation(filesystem, std::string(dir) + name), (std::string("the loader still refuses ") + name).c_str());
    const auto direct = assets::load_animation(filesystem, std::string(dir) + "w_noroot_idle_00.ala");
    expect(!direct && !direct.error().code.empty(), "a file with no root group is refused with a diagnostic code");

    // The helper: a good clip comes back with no note.
    auto good = assets::load_animation_or_note(filesystem, std::string(dir) + "w_good_idle_00.ala");
    expect(good.clip && !good.note, "a well-formed clip loads with no note");

    eawr::core::degradation::reset_for_tests();
    eawr::core::degradation::SessionInfo info;
    info.mods = {"workshop:1000001"};
    info.mod_layers = {"mod"};
    eawr::core::degradation::describe_session(info);

    std::vector<eawr::core::LoadNote> notes;
    for (const char* name : {"w_empty_idle_00.ala", "w_noroot_idle_00.ala", "w_cut_idle_00.ala"}) {
        const std::string path = std::string(dir) + name;
        auto result = assets::load_animation_or_note(filesystem, path);
        expect(!result.clip && result.note.has_value(), (std::string("a refused clip comes back as one note: ") + name).c_str());
        if (!result.note) continue;
        const auto& note = *result.note;
        expect(note.code == assets::asset_degraded_code, "the note carries the degraded-asset code");
        expect(note.classification == eawr::core::LoadCategory::unknown_degrade, "the note is class C");
        expect(note.layer.mod_id == "mod", "the note names the layer that supplied the file");
        expect(note.object == path && note.file.find("w_") != std::string::npos, "the note names the file");
        expect(note.tag == "animation" && !note.diagnostic_code.empty() && !note.message.empty(),
               "the note carries the asset kind and the loader's diagnostic");
        notes.push_back(note);
    }
    expect(notes.size() == 3, "three refused clips make exactly three notes");

    // The ledger counts the notes under the mod, one item per file.
    expect(eawr::core::degradation::record_load_notes(notes) == 3, "the ledger takes every note");
    const auto snapshot = eawr::core::degradation::snapshot();
    expect(snapshot.classes.size() == 1 && snapshot.classes[0].code == assets::asset_degraded_code
               && snapshot.classes[0].source == "workshop:1000001" && snapshot.classes[0].objects == 3,
           "the ledger counts three objects in one class under the mod that supplied the files");

    // An empty model is a refusal with its own note, and a missing file names the stock layer.
    const auto model = assets::load_model(filesystem, std::string(dir) + "w_empty.alo");
    expect(!model, "an empty model is refused by the loader");
    if (!model) {
        const auto note = assets::asset_degraded_note(filesystem, std::string(dir) + "w_empty.alo",
            assets::DegradedAsset::model, assets::DegradedStage::load, model.error());
        expect(note.tag == "model" && note.layer.mod_id == "mod", "an empty model gets a model note under its layer");
    }
    const auto missing = assets::load_animation_or_note(filesystem, std::string(dir) + "w_missing_idle_00.ala");
    expect(!missing.clip && missing.note && missing.note->layer.mod_id == "stock",
           "a clip the file system does not hold is a note under the stock layer, not an error");

    // The galaxy scene's sequence for planets that share a model: load the idle clip, bind it to every surface, keep the
    // object. An empty clip: the start goes on, the model is present, the planets play nothing, and the whole sequence
    // costs exactly one note however many planets and surfaces share the file.
    {
        auto planet_model = assets::load_model(filesystem, std::string(dir) + "w_planet.alo");
        expect(static_cast<bool>(planet_model), "the planet model loads");
        if (planet_model) {
            const std::string empty_path = std::string(dir) + "w_planet_empty_idle_00.ala";
            eawr::presentation::animation::DegradedClips degraded;
            std::size_t drawn = 0, animated = 0;
            for (int planet = 0; planet < 4; ++planet) {
                const auto clip = degraded.load(filesystem, empty_path);
                for (int surface = 0; surface < 3; ++surface) {
                    const bool bound = clip && degraded.bind(filesystem, planet_model.value(), *clip, empty_path);
                    animated += bound ? 1U : 0U;
                    if (eawr::presentation::animation::Player::create(planet_model.value(), nullptr)) ++drawn;
                }
            }
            const auto taken = degraded.drain();
            expect(drawn == 12 && animated == 0, "an empty idle clip: every object is still drawn, none animates");
            expect(taken.size() == 1 && taken[0].code == assets::asset_degraded_code && taken[0].tag == "animation"
                       && taken[0].layer.mod_id == "mod" && taken[0].file.find("w_planet_empty_idle_00") != std::string::npos,
                   "an empty idle clip shared by four planets and twelve surfaces costs exactly one note");
            expect(taken.size() == 1 && taken[0].phase == eawr::core::LoadPhase::load, "a refused clip is a load-phase note");

            // The bind stage: a clip that parses but does not fit the model plays nothing and costs one note.
            const assets::Animation misfit = misfit_animation();
            const std::string good_path = std::string(dir) + "w_planet_idle_00.ala";
            for (int surface = 0; surface < 3; ++surface)
                expect(!degraded.bind(filesystem, planet_model.value(), misfit, good_path), "a clip that does not fit binds to nothing");
            const auto bind_notes = degraded.drain();
            expect(bind_notes.size() == 1 && bind_notes[0].message.find("does not bind") != std::string::npos,
                   "three surfaces refusing the same non-fitting clip cost exactly one bind note");

            // A clip that fits binds, with no note.
            const auto fitting = degraded.load(filesystem, good_path);
            expect(fitting && degraded.bind(filesystem, planet_model.value(), *fitting, good_path) && degraded.empty(),
                   "a well-formed fitting clip loads and binds with no note");
        }
    }
    eawr::core::degradation::reset_for_tests();
}
} // namespace

int main() {
    refusals_become_one_note();
    return eawr::test::ui::failures() == 0 ? 0 : 1;
}
