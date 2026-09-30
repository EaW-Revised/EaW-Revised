// Isolated synthetic Godot exercise for the fog-stub-v1 texture adapter. It
// drives the real GodotFogBackend through fog::TextureCache, reads textures
// back from the RenderingServer, renders the synthetic nearest shader through
// an orthographic top-down SubViewport and writes a JSON report. Every grid is
// invented here; no game asset, viewer or production material is involved.

#include "fog_adapter.hpp"

#include "eawr/presentation/fog/fog.hpp"
#include "eawr/sim/snapshot.hpp"

#include <gdextension_interface.h>
#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/material.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/plane_mesh.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>
#include <godot_cpp/classes/viewport_texture.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/godot.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace godot;

namespace {

namespace fog = eawr::presentation::fog;
namespace sim_fog = eawr::sim::fog;
using eawr::presentation::godot_backend::GodotFogBackend;

constexpr std::int64_t one = std::int64_t{1} << 24;
// Orthographic top-down view: 200x80 pixels, 20 pixels per source unit,
// centred on source (0.5, 1.0). Screen right is +X, screen up is source +Y.
constexpr int view_width = 200;
constexpr int view_height = 80;
constexpr double pixels_per_unit = 20.0;
constexpr double centre_x = 0.5;
constexpr double centre_y = 1.0;
constexpr int settle_frames = 4;
constexpr int frame_budget = 2000;

[[nodiscard]] std::string escape(const std::string& text) {
    std::string out;
    for (const char c : text) {
        if (c == '"' || c == '\\') out.push_back('\\');
        if (c == '\n') {
            out += "\\n";
            continue;
        }
        out.push_back(c);
    }
    return out;
}

[[nodiscard]] std::string bytes_json(const std::vector<std::uint8_t>& bytes) {
    std::string out = "[";
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i != 0) out += ",";
        out += std::to_string(bytes[i]);
    }
    return out + "]";
}

[[nodiscard]] std::uint8_t srgb_byte(const std::uint8_t linear) {
    const double value = linear / 255.0;
    const double encoded = value <= 0.0031308 ? 12.92 * value : 1.055 * std::pow(value, 1.0 / 2.4) - 0.055;
    return static_cast<std::uint8_t>(std::lround(std::clamp(encoded, 0.0, 1.0) * 255.0));
}

// Constant-albedo control through the same unshaded SubViewport path: 64
// levels per frame in 3-pixel columns, so the output transfer of this host
// is measured instead of assumed.
constexpr std::string_view calibration_program = R"GODOT(
shader_type spatial;
render_mode unshaded, cull_disabled;
uniform float eawr_level_offset = 0.0;
void fragment() {
    float level = min(eawr_level_offset + floor(FRAGCOORD.x / 3.0), 255.0);
    ALBEDO = vec3(level / 255.0);
}
)GODOT";
constexpr int calibration_levels_per_frame = 64;

sim_fog::FogGridDesc base_desc(const std::uint64_t revision) {
    return sim_fog::FogGridDesc{
        .team_id = 0, .width = 3, .height = 2,
        .origin_x_raw = -5 * one / 2, .origin_y_raw = one / 4,
        .cell_x_raw = 2 * one, .cell_y_raw = 3 * one / 4,
        .encoding = sim_fog::encoding_linear_u8_attenuation, .revision = revision,
    };
}

const std::vector<std::uint8_t> base_cells{10, 60, 110, 160, 210, 255};

} // namespace

class EawrFogProbe final : public Node {
    GDCLASS(EawrFogProbe, Node)

protected:
    static void _bind_methods() {}

public:
    void _ready() override;
    void _process(double delta) override;

private:
    struct Step {
        std::string name;
        std::function<void()> act;
        // Set when the step renders: the grid consumers should show, or
        // nullptr for fully dark (unbound).
        bool renders{};
        std::function<const sim_fog::FogGrid*()> expected;
        int calibration_offset{-1}; // >= 0: a calibration frame, not a fog render
    };

    void check(bool condition, const std::string& message);
    sim_fog::FogGrid make_grid(const sim_fog::FogGridDesc& desc, const std::vector<std::uint8_t>& cells);
    sim_fog::FogGridSet make_set(std::vector<sim_fog::FogGrid> grids);
    void submit(const sim_fog::FogGridSet& set, fog::StreamTeam key, fog::SubmitAction expected, const std::string& label);
    void reject(const sim_fog::FogGridSet& set, fog::StreamTeam key, std::string_view code, const std::string& label);
    void expect_readback(const std::vector<std::uint8_t>& cells, std::uint32_t width, std::uint32_t height, const std::string& label);
    void verify_render(const Step& step);
    void record_calibration(const Step& step);
    void record_stage(const std::string& name, const std::string& extra = {});
    void build_steps();
    void release_resources();
    void finish();

    SubViewport* viewport_{};
    MeshInstance3D* surface_{};
    Ref<Shader> shader_;
    Ref<ShaderMaterial> material_a_;
    Ref<ShaderMaterial> material_b_;
    Ref<Shader> calibration_shader_;
    Ref<ShaderMaterial> calibration_material_;
    // Reverse member destruction releases the cache before its backend and
    // before the material Refs whose RIDs the cache unbinds.
    std::unique_ptr<GodotFogBackend> backend_;
    std::unique_ptr<fog::TextureCache> cache_;
    std::array<int, 256> transfer_{};
    fog::ConsumerId consumer_a_{};
    fog::ConsumerId consumer_b_{};
    std::vector<Step> steps_;
    std::size_t index_{};
    int wait_{};
    int frames_{};
    int frame_budget_{frame_budget};
    bool finished_{};
    bool test_frame_budget_exhaustion_{};
    bool test_frame_budget_ready_{};
    bool cleanup_order_verified_{};
    std::string report_path_;
    std::vector<std::string> failures_;
    std::vector<std::string> stages_;
    std::vector<std::string> renders_;
    std::optional<sim_fog::FogGrid> current_;
    std::optional<sim_fog::FogGrid> team7_;
    std::shared_ptr<const eawr::sim::RenderSnapshot> snapshot_;
    std::uint64_t texture_memory_baseline_{};
    std::uint64_t texture_memory_with_fog_{};
    std::uint64_t texture_memory_after_teardown_{};
};

void EawrFogProbe::check(const bool condition, const std::string& message) {
    if (condition) return;
    failures_.push_back(message);
    UtilityFunctions::printerr(String("EAWR fog probe check failed: ") + String(message.c_str()));
}

sim_fog::FogGrid EawrFogProbe::make_grid(const sim_fog::FogGridDesc& desc, const std::vector<std::uint8_t>& cells) {
    auto created = sim_fog::FogGrid::create(desc, cells);
    if (!created) {
        UtilityFunctions::printerr(String("EAWR fog probe fixture rejected: ") + String(created.error().message.c_str()));
        std::abort();
    }
    return std::move(created).value();
}

sim_fog::FogGridSet EawrFogProbe::make_set(std::vector<sim_fog::FogGrid> grids) {
    auto created = sim_fog::FogGridSet::create(std::move(grids));
    if (!created) std::abort();
    return std::move(created).value();
}

void EawrFogProbe::submit(
    const sim_fog::FogGridSet& set, const fog::StreamTeam key,
    const fog::SubmitAction expected, const std::string& label) {
    const auto result = cache_->submit(set, key);
    check(result.has_value(), label + ": submit failed");
    if (result) {
        check(result.value() == expected, label + ": action " + std::string(fog::to_string(result.value()))
            + " != " + std::string(fog::to_string(expected)));
    }
}

void EawrFogProbe::reject(
    const sim_fog::FogGridSet& set, const fog::StreamTeam key,
    const std::string_view code, const std::string& label) {
    const auto result = cache_->submit(set, key);
    check(!result.has_value(), label + ": submit unexpectedly succeeded");
    if (!result) check(result.error().code == code, label + ": code " + result.error().code);
}

void EawrFogProbe::expect_readback(
    const std::vector<std::uint8_t>& cells, const std::uint32_t width,
    const std::uint32_t height, const std::string& label) {
    const auto texture = cache_->texture({1, 0});
    const auto readback = backend_->read_back(texture);
    check(readback.has_value(), label + ": texture readback failed");
    if (!readback) return;
    check(readback->width == width && readback->height == height, label + ": readback dimensions");
    check(readback->r8 && !readback->mipmaps, label + ": readback is R8 without mipmaps");
    check(readback->bytes == cells, label + ": readback bytes " + bytes_json(readback->bytes));
}

void EawrFogProbe::record_stage(const std::string& name, const std::string& extra) {
    const auto& s = cache_ ? cache_->stats() : fog::CacheStats{};
    std::ostringstream out;
    out << "{\"stage\":\"" << escape(name) << "\",\"uploads\":" << s.uploads << ",\"upload_bytes\":" << s.upload_bytes
        << ",\"creates\":" << s.creates << ",\"recreates\":" << s.recreates << ",\"updates\":" << s.updates
        << ",\"destroys\":" << s.destroys << ",\"binds\":" << s.binds << ",\"unbinds\":" << s.unbinds
        << ",\"metadata_only\":" << s.metadata_only << ",\"reselects\":" << s.reselects
        << ",\"rejected\":" << s.rejected << ",\"live_textures\":" << (backend_ ? backend_->live_textures() : 0)
        << extra << "}";
    stages_.push_back(out.str());
}

void EawrFogProbe::verify_render(const Step& step) {
    Ref<Image> image = viewport_->get_texture()->get_image();
    check(image.is_valid() && image->get_width() == view_width && image->get_height() == view_height,
          step.name + ": viewport capture size");
    if (!image.is_valid()) return;
    image->convert(Image::FORMAT_RGBA8);
    const PackedByteArray data = image->get_data();
    const sim_fog::FogGrid* grid = step.expected ? step.expected() : nullptr;

    int compared = 0;
    int skipped = 0;
    int mismatches = 0;
    int max_error = 0;
    std::string first_mismatch;
    for (int py = 0; py < view_height; ++py) {
        for (int px = 0; px < view_width; ++px) {
            const double sx = centre_x + (px + 0.5 - view_width / 2.0) / pixels_per_unit;
            const double sy = centre_y - (py + 0.5 - view_height / 2.0) / pixels_per_unit;
            std::uint8_t expected = 0;
            if (grid != nullptr) {
                // Skip pixel centres within one pixel of a cell boundary.
                const auto mapping = fog::mapping_for(grid->desc());
                const double cell_w = mapping.extent_x / mapping.width;
                const double cell_h = mapping.extent_y / mapping.height;
                const double fx = (sx - mapping.origin_x) / cell_w;
                const double fy = (sy - mapping.origin_y) / cell_h;
                // Only the grid's own lines: k in [0, width] and [0, height].
                const double line_x = std::clamp(std::round(fx), 0.0, static_cast<double>(mapping.width));
                const double line_y = std::clamp(std::round(fy), 0.0, static_cast<double>(mapping.height));
                const double near_x = std::abs(fx - line_x) * cell_w;
                const double near_y = std::abs(fy - line_y) * cell_h;
                if (near_x < 1.0 / pixels_per_unit || near_y < 1.0 / pixels_per_unit) {
                    ++skipped;
                    continue;
                }
                expected = static_cast<std::uint8_t>(transfer_[fog::attenuation_at(*grid, {sx, sy})]);
            }
            const std::size_t offset = (static_cast<std::size_t>(py) * view_width + px) * 4;
            const int r = data[static_cast<std::int64_t>(offset)];
            const int g = data[static_cast<std::int64_t>(offset + 1)];
            const int b = data[static_cast<std::int64_t>(offset + 2)];
            const int error = std::max({std::abs(r - expected), std::abs(g - expected), std::abs(b - expected)});
            max_error = std::max(max_error, error);
            ++compared;
            if (error > 1) {
                if (mismatches == 0) {
                    first_mismatch = "pixel " + std::to_string(px) + "," + std::to_string(py) + " rgb "
                        + std::to_string(r) + "," + std::to_string(g) + "," + std::to_string(b)
                        + " expected " + std::to_string(expected);
                }
                ++mismatches;
            }
        }
    }
    check(mismatches == 0, step.name + ": " + std::to_string(mismatches) + " pixel mismatches; first " + first_mismatch);
    check(compared > view_width * view_height / 2, step.name + ": too few compared pixels");

    if (grid != nullptr) {
        // Orientation and bounds are only provable when every cell renders a
        // distinct value that also differs from the dark outside.
        std::vector<int> outputs{transfer_[0]};
        for (const std::uint8_t cell : grid->cells()) outputs.push_back(transfer_[cell]);
        std::sort(outputs.begin(), outputs.end());
        check(std::adjacent_find(outputs.begin(), outputs.end()) == outputs.end(),
              step.name + ": cell values do not render distinctly from each other and from dark");
    }

    // Cell-centre samples for the report (row y of the grid, column x).
    std::string samples = "[";
    if (grid != nullptr) {
        const auto mapping = fog::mapping_for(grid->desc());
        for (std::uint32_t y = 0; y < mapping.height; ++y) {
            for (std::uint32_t x = 0; x < mapping.width; ++x) {
                const double sx = mapping.origin_x + (x + 0.5) * mapping.extent_x / mapping.width;
                const double sy = mapping.origin_y + (y + 0.5) * mapping.extent_y / mapping.height;
                const int px = static_cast<int>(std::floor((sx - centre_x) * pixels_per_unit + view_width / 2.0));
                const int py = static_cast<int>(std::floor((centre_y - sy) * pixels_per_unit + view_height / 2.0));
                const std::size_t offset = (static_cast<std::size_t>(py) * view_width + px) * 4;
                if (samples.size() > 1) samples += ",";
                samples += "{\"cell\":[" + std::to_string(x) + "," + std::to_string(y) + "],\"pixel\":["
                    + std::to_string(px) + "," + std::to_string(py) + "],\"cell_byte\":"
                    + std::to_string(*grid->cell(x, y)) + ",\"expected_output\":"
                    + std::to_string(transfer_[*grid->cell(x, y)]) + ",\"measured_r\":"
                    + std::to_string(data[static_cast<std::int64_t>(offset)]) + "}";
            }
        }
    }
    samples += "]";
    renders_.push_back("{\"stage\":\"" + escape(step.name) + "\",\"expected\":\""
        + (grid == nullptr ? std::string("dark") : "team " + std::to_string(grid->team_id()) + " revision "
            + std::to_string(grid->revision()))
        + "\",\"compared\":" + std::to_string(compared) + ",\"skipped_boundary\":" + std::to_string(skipped)
        + ",\"mismatches\":" + std::to_string(mismatches) + ",\"max_error\":" + std::to_string(max_error)
        + ",\"cell_samples\":" + samples + "}");
}

void EawrFogProbe::record_calibration(const Step& step) {
    Ref<Image> image = viewport_->get_texture()->get_image();
    check(image.is_valid(), step.name + ": calibration capture");
    if (!image.is_valid()) return;
    image->convert(Image::FORMAT_RGBA8);
    const PackedByteArray data = image->get_data();
    for (int i = 0; i < calibration_levels_per_frame; ++i) {
        const int level = step.calibration_offset + i;
        if (level > 255) break;
        const std::size_t offset = (static_cast<std::size_t>(view_height / 2) * view_width + 3 * i + 1) * 4;
        const int r = data[static_cast<std::int64_t>(offset)];
        const int g = data[static_cast<std::int64_t>(offset + 1)];
        const int b = data[static_cast<std::int64_t>(offset + 2)];
        check(r == g && g == b, step.name + ": calibration level " + std::to_string(level) + " is not grey");
        transfer_[static_cast<std::size_t>(level)] = r;
    }
}

void EawrFogProbe::build_steps() {
    for (int offset = 0; offset < 256; offset += calibration_levels_per_frame) {
        steps_.push_back({"calibration-" + std::to_string(offset), [this, offset] {
            calibration_material_->set_shader_parameter("eawr_level_offset", static_cast<double>(offset));
            surface_->set_material_override(calibration_material_);
        }, true, {}, offset});
    }

    const fog::StreamTeam key{1, 0};
    const auto dark = [] { return static_cast<const sim_fog::FogGrid*>(nullptr); };
    const auto shown = [this] { return current_ ? &*current_ : nullptr; };

    steps_.push_back({"unbound-consumer", [this] {
        // A current consumer registered before any grid renders dark.
        surface_->set_material_override(material_a_);
        bool monotonic = transfer_[0] == 0 && transfer_[255] == 255;
        for (std::size_t level = 1; level < transfer_.size(); ++level) {
            monotonic = monotonic && transfer_[level] >= transfer_[level - 1];
        }
        check(monotonic, "output transfer is monotonic from 0 to 255");
        check(static_cast<bool>(cache_->add_consumer(consumer_a_)), "consumer A registers");
        record_stage("unbound-consumer");
    }, true, dark});

    steps_.push_back({"first-upload", [this, key] {
        // Baseline after the viewport has rendered, before any fog texture.
        texture_memory_baseline_ = RenderingServer::get_singleton()->get_rendering_info(
            RenderingServer::RENDERING_INFO_TEXTURE_MEM_USED);
        current_ = make_grid(base_desc(1), base_cells);
        submit(make_set({*current_}), key, fog::SubmitAction::created, "first-upload");
        const auto& s = cache_->stats();
        check(s.uploads == 1 && s.upload_bytes == 6 && s.creates == 1 && s.binds == 1, "first upload is one 6-byte create");
        expect_readback(base_cells, 3, 2, "first-upload");
        record_stage("first-upload");
    }, true, shown});

    steps_.push_back({"identical-and-revision-only", [this, key] {
        // Forward+ updates the memory counters once per drawn frame, so the
        // first upload's texture is sampled here, after its frames.
        texture_memory_with_fog_ = RenderingServer::get_singleton()->get_rendering_info(
            RenderingServer::RENDERING_INFO_TEXTURE_MEM_USED);
        const auto before = cache_->stats();
        submit(make_set({make_grid(base_desc(1), base_cells)}), key, fog::SubmitAction::unchanged, "identical");
        check(cache_->stats() == before, "identical resubmit changes no counter");
        current_ = make_grid(base_desc(2), base_cells);
        submit(make_set({*current_}), key, fog::SubmitAction::metadata_only, "revision-only");
        check(cache_->stats().uploads == 1 && cache_->stats().binds == 1, "revision-only bump uploads and binds nothing");
        record_stage("identical-and-revision-only");
    }, false, {}});

    steps_.push_back({"origin-shift", [this, key] {
        auto desc = base_desc(3);
        desc.origin_x_raw += one;
        current_ = make_grid(desc, base_cells);
        submit(make_set({*current_}), key, fog::SubmitAction::metadata_only, "origin-shift");
        check(cache_->stats().uploads == 1 && cache_->stats().binds == 2, "origin shift rebinds uniforms without upload");
        record_stage("origin-shift");
    }, true, shown});

    steps_.push_back({"cell-change", [this, key] {
        current_ = make_grid(base_desc(4), base_cells);
        submit(make_set({*current_}), key, fog::SubmitAction::metadata_only, "origin-restore");
        auto changed = base_cells;
        changed[4] = 35; // still distinct from every other cell and from dark
        current_ = make_grid(base_desc(5), changed);
        submit(make_set({*current_}), key, fog::SubmitAction::updated, "cell-change");
        const auto& s = cache_->stats();
        check(s.uploads == 2 && s.updates == 1 && s.upload_bytes == 12 && s.creates == 1, "one cell change is one 6-byte update");
        expect_readback(changed, 3, 2, "cell-change");
        record_stage("cell-change");
    }, true, shown});

    steps_.push_back({"rejected-revisions", [this, key] {
        const auto texture = cache_->texture(key);
        reject(make_set({make_grid(base_desc(4), base_cells)}), key, fog::diagnostic_codes::revision_rollback, "rollback");
        auto divergent = current_->cells();
        std::vector<std::uint8_t> bytes(divergent.begin(), divergent.end());
        bytes[0] = 11;
        reject(make_set({make_grid(base_desc(5), bytes)}), key, fog::diagnostic_codes::revision_conflict, "equal-revision");
        check(cache_->texture(key) == texture && cache_->stats().uploads == 2, "rejections upload nothing");
        expect_readback({current_->cells().begin(), current_->cells().end()}, 3, 2, "rejected-revisions");
        record_stage("rejected-revisions");
    }, true, shown});

    steps_.push_back({"missing-team", [this, key] {
        auto other = base_desc(1);
        other.team_id = 7;
        reject(make_set({make_grid(other, base_cells)}), key, fog::diagnostic_codes::missing_team, "missing-team");
        check(!cache_->active() && cache_->live_textures() == 1, "missing team unbinds but keeps the cached texture");
        record_stage("missing-team");
    }, true, dark});

    steps_.push_back({"backend-failure", [this, key] {
        submit(make_set({*current_}), key, fog::SubmitAction::reselected, "reselect");
        const auto uploads = cache_->stats().uploads;
        auto wide = base_desc(6);
        wide.width = 5; // the backend models a 4-texel device limit
        reject(make_set({make_grid(wide, std::vector<std::uint8_t>(10, 200))}), key,
               fog::diagnostic_codes::backend_failure, "backend-failure");
        check(cache_->stats().uploads == uploads && backend_->live_textures() == 1, "failed create leaves one texture");
        check(cache_->accepted(key)->revision() == 5 && cache_->active() == key, "failed create keeps accepted grid and binding");
        expect_readback({current_->cells().begin(), current_->cells().end()}, 3, 2, "backend-failure");
        record_stage("backend-failure", ",\"failure_cause\":\"" + escape(backend_->failure_cause()) + "\"");
    }, true, shown});

    steps_.push_back({"recreate", [this, key] {
        auto desc = base_desc(7);
        desc.width = 4;
        desc.height = 3;
        desc.cell_x_raw = 3 * one / 2;
        desc.cell_y_raw = one / 2;
        const std::vector<std::uint8_t> cells{15, 30, 45, 60, 75, 90, 105, 120, 135, 150, 165, 180};
        const auto old = cache_->texture(key);
        current_ = make_grid(desc, cells);
        submit(make_set({*current_}), key, fog::SubmitAction::recreated, "recreate");
        const auto& s = cache_->stats();
        check(cache_->texture(key) != old && s.recreates == 1 && s.destroys == 1 && s.upload_bytes == 24,
              "dimension change recreates with 12 bytes and releases the old texture");
        check(backend_->live_textures() == 1, "one live texture after recreation");
        expect_readback(cells, 4, 3, "recreate");
        record_stage("recreate");
    }, true, shown});

    steps_.push_back({"late-consumer", [this] {
        consumer_b_ = backend_->register_material(material_b_->get_rid());
        const auto binds = cache_->stats().binds;
        check(static_cast<bool>(cache_->add_consumer(consumer_b_)), "late consumer B registers");
        check(cache_->stats().binds == binds + 1, "late consumer is bound immediately");
        surface_->set_material_override(material_b_);
        record_stage("late-consumer");
    }, true, shown});

    steps_.push_back({"team-switch", [this] {
        auto seven = base_desc(1);
        seven.team_id = 7;
        seven.width = 2;
        seven.height = 3;
        seven.origin_x_raw = -one;
        seven.origin_y_raw = 2 * one / 5;
        seven.cell_x_raw = 3 * one / 2;
        seven.cell_y_raw = 3 * one / 10;
        team7_ = make_grid(seven, {30, 90, 150, 200, 240, 120});
        snapshot_ = std::make_shared<const eawr::sim::RenderSnapshot>(
            std::uint64_t{12}, std::vector<eawr::sim::RenderInstance>{}, make_set({*current_, *team7_}));
        submit(snapshot_->fog_grids(), {1, 7}, fog::SubmitAction::created, "team-7");
        record_stage("team-7");
    }, true, [this] { return team7_ ? &*team7_ : nullptr; }});

    steps_.push_back({"team-switch-back", [this, key] {
        const auto uploads = cache_->stats().uploads;
        submit(snapshot_->fog_grids(), key, fog::SubmitAction::reselected, "team-0-again");
        check(cache_->stats().uploads == uploads && cache_->stats().reselects == 2, "switch back reuses the cached texture");
        record_stage("team-switch-back");
    }, true, shown});

    steps_.push_back({"reset", [this] {
        cache_->reset();
        check(cache_->live_textures() == 0 && backend_->live_textures() == 0, "reset releases every texture");
        const auto* retained = snapshot_->fog_grids().find(7);
        check(retained != nullptr && *retained == *team7_, "retained snapshot survives reset");
        record_stage("reset");
    }, true, dark});

    steps_.push_back({"teardown", [this] {
        submit(snapshot_->fog_grids(), {2, 7}, fog::SubmitAction::created, "after-reset");
        check(backend_->live_textures() == 1, "one texture before teardown");
        record_stage("before-teardown");
        release_resources(); // destroy textures and unbind while dependencies are alive
        check(cleanup_order_verified_, "teardown releases the cache before its backend and material Refs");
        const auto* retained = snapshot_->fog_grids().find(0);
        check(retained != nullptr && *retained == *current_, "retained snapshot survives teardown");
    }, true, dark});

    steps_.push_back({"texture-memory", [this] {
        texture_memory_after_teardown_ = RenderingServer::get_singleton()->get_rendering_info(
            RenderingServer::RENDERING_INFO_TEXTURE_MEM_USED);
        check(texture_memory_with_fog_ > texture_memory_baseline_, "fog texture is visible in texture memory");
        check(texture_memory_after_teardown_ == texture_memory_baseline_, "texture memory returns to baseline");
    }, false, {}});
}

void EawrFogProbe::_ready() {
    if (Engine::get_singleton()->is_editor_hint()) return;
    const PackedStringArray arguments = OS::get_singleton()->get_cmdline_user_args();
    for (std::int64_t i = 0; i + 1 < arguments.size(); ++i) {
        if (arguments[i] == String("--eawr-fog-report")) report_path_ = arguments[i + 1].utf8().get_data();
    }
    for (std::int64_t i = 0; i < arguments.size(); ++i) {
        if (arguments[i] == String("--eawr-fog-test-frame-budget")) test_frame_budget_exhaustion_ = true;
    }

    viewport_ = memnew(SubViewport);
    viewport_->set_size(Vector2i(view_width, view_height));
    viewport_->set_update_mode(SubViewport::UPDATE_ALWAYS);
    viewport_->set_use_own_world_3d(true);
    add_child(viewport_);

    Camera3D* camera = memnew(Camera3D);
    camera->set_projection(Camera3D::PROJECTION_ORTHOGONAL);
    camera->set_size(view_height / pixels_per_unit);
    camera->set_near(0.1F);
    camera->set_far(100.0F);
    // Looking straight down: screen up is world -Z, which is source +Y.
    camera->set_position(Vector3(static_cast<real_t>(centre_x), 10.0F, static_cast<real_t>(-centre_y)));
    camera->set_rotation_degrees(Vector3(-90.0F, 0.0F, 0.0F));
    viewport_->add_child(camera);
    camera->make_current();

    shader_.instantiate();
    const auto code = GodotFogBackend::synthetic_nearest_shader();
    shader_->set_code(String::utf8(code.data(), static_cast<int>(code.size())));
    material_a_.instantiate();
    material_a_->set_shader(shader_);
    material_b_.instantiate();
    material_b_->set_shader(shader_);
    calibration_shader_.instantiate();
    calibration_shader_->set_code(
        String::utf8(calibration_program.data(), static_cast<int>(calibration_program.size())));
    calibration_material_.instantiate();
    calibration_material_->set_shader(calibration_shader_);

    Ref<PlaneMesh> plane;
    plane.instantiate();
    plane->set_size(Vector2(12.0F, 6.0F));
    surface_ = memnew(MeshInstance3D);
    surface_->set_mesh(plane);
    surface_->set_position(Vector3(static_cast<real_t>(centre_x), 0.0F, static_cast<real_t>(-centre_y)));
    surface_->set_material_override(material_a_);
    viewport_->add_child(surface_);

    backend_ = std::make_unique<GodotFogBackend>(4);
    cache_ = std::make_unique<fog::TextureCache>(*backend_);
    consumer_a_ = backend_->register_material(material_a_->get_rid());
    build_steps();
    set_process(true);
}

void EawrFogProbe::release_resources() {
    if (!cache_ && !backend_) return;

    const bool materials_alive = shader_.is_valid() && material_a_.is_valid() && material_b_.is_valid()
        && calibration_shader_.is_valid() && calibration_material_.is_valid();
    const bool backend_alive_for_cache = !cache_ || backend_ != nullptr;
    cache_.reset();
    const bool textures_released = !backend_ || backend_->live_textures() == 0;
    cleanup_order_verified_ = materials_alive && backend_alive_for_cache && textures_released && !cache_;
    backend_.reset();
}

void EawrFogProbe::_process(double) {
    if (finished_ || Engine::get_singleton()->is_editor_hint()) return;
    // Move the deadline after the late consumer has rendered, so the isolated
    // runtime test exercises cleanup with both material bindings registered.
    if (test_frame_budget_exhaustion_ && test_frame_budget_ready_ && cache_ && cache_->live_textures() != 0) {
        frame_budget_ = frames_;
        test_frame_budget_exhaustion_ = false;
    }
    if (++frames_ > frame_budget_) {
        check(false, "frame budget exhausted");
        finish();
        return;
    }
    if (wait_ > 0) {
        if (--wait_ > 0) return;
        if (steps_[index_].calibration_offset >= 0) {
            record_calibration(steps_[index_]);
        } else {
            verify_render(steps_[index_]);
        }
        if (test_frame_budget_exhaustion_ && steps_[index_].name == "late-consumer") {
            test_frame_budget_ready_ = true;
        }
        ++index_;
    }
    while (index_ < steps_.size()) {
        steps_[index_].act();
        if (steps_[index_].renders) {
            wait_ = settle_frames;
            return;
        }
        ++index_;
    }
    finish();
}

void EawrFogProbe::finish() {
    finished_ = true;
    release_resources();
    check(cleanup_order_verified_, "finish releases the cache before its backend and material Refs");
    if (surface_ != nullptr) surface_->set_material_override(Ref<Material>());
    const bool passed = failures_.empty();
    std::ostringstream out;
    out << "{\"schema\":\"eawr-fog-godot-probe-v1\",\"status\":\""
        << (passed ? "fog_godot_probe_passed" : "fog_godot_probe_failed") << "\",";
    out << "\"godot_version\":\"" << escape(String(Engine::get_singleton()->get_version_info()["string"]).utf8().get_data())
        << "\",\"rendering_driver\":\""
        << escape(RenderingServer::get_singleton()->get_current_rendering_driver_name().utf8().get_data())
        << "\",\"rendering_method\":\""
        << escape(RenderingServer::get_singleton()->get_current_rendering_method().utf8().get_data()) << "\",";
    out << "\"view\":{\"width\":" << view_width << ",\"height\":" << view_height << ",\"pixels_per_unit\":"
        << pixels_per_unit << ",\"centre_source\":[" << centre_x << "," << centre_y << "]},";
    out << "\"texture_memory\":{\"baseline\":" << texture_memory_baseline_ << ",\"with_fog\":" << texture_memory_with_fog_
        << ",\"after_teardown\":" << texture_memory_after_teardown_ << "},";
    bool identity = true;
    bool srgb = true;
    out << "\"output_transfer\":[";
    for (std::size_t level = 0; level < transfer_.size(); ++level) {
        out << (level ? "," : "") << transfer_[level];
        identity = identity && transfer_[level] == static_cast<int>(level);
        srgb = srgb && std::abs(transfer_[level] - srgb_byte(static_cast<std::uint8_t>(level))) <= 1;
    }
    out << "],\"output_transfer_identity\":" << (identity ? "true" : "false")
        << ",\"output_transfer_srgb\":" << (srgb ? "true" : "false") << ",";
    out << "\"steps_completed\":" << index_ << ",\"steps_total\":" << steps_.size() << ",\"frames\":" << frames_ << ",";
    out << "\"cleanup_order_verified\":" << (cleanup_order_verified_ ? "true" : "false") << ",";
    out << "\"stages\":[";
    for (std::size_t i = 0; i < stages_.size(); ++i) out << (i ? "," : "") << stages_[i];
    out << "],\"renders\":[";
    for (std::size_t i = 0; i < renders_.size(); ++i) out << (i ? "," : "") << renders_[i];
    out << "],\"failures\":[";
    for (std::size_t i = 0; i < failures_.size(); ++i) out << (i ? "," : "") << "\"" << escape(failures_[i]) << "\"";
    out << "]}\n";

    bool written = false;
    if (!report_path_.empty()) {
        std::ofstream file(report_path_, std::ios::binary);
        file << out.str();
        written = static_cast<bool>(file);
    }
    UtilityFunctions::print(String("EAWR fog probe ") + (passed ? "passed" : "failed") + " after "
        + String::num_int64(frames_) + " frames");
    get_tree()->quit(passed && written ? 0 : 1);
}

namespace {

void initialize_fog_probe(const ModuleInitializationLevel level) {
    if (level == MODULE_INITIALIZATION_LEVEL_SCENE) GDREGISTER_CLASS(EawrFogProbe);
}

void uninitialize_fog_probe(const ModuleInitializationLevel) {}

} // namespace

extern "C" GDExtensionBool GDE_EXPORT eawr_fog_probe_library_init(
    GDExtensionInterfaceGetProcAddress get_proc_address,
    GDExtensionClassLibraryPtr library,
    GDExtensionInitialization* initialization) {
    GDExtensionBinding::InitObject init(get_proc_address, library, initialization);
    init.register_initializer(initialize_fog_probe);
    init.register_terminator(uninitialize_fog_probe);
    init.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);
    return init.init();
}
