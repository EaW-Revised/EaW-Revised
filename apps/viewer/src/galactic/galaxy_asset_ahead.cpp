#include "galaxy_asset_ahead.hpp"

#include "galactic_texture_chain.hpp"
#include "eawr/core/gc_load_trace.hpp"
#include "eawr/vfs/vfs.hpp"

#include <algorithm>
#include <utility>

namespace eawr::presentation::godot_backend {
namespace {
// The most estimated payload bytes of decoded models waiting for the upload to take them. A planet model is a few MiB.
constexpr std::uint64_t model_window_bytes = 128ULL << 20U;
} // namespace

GalaxyAssetAhead::GalaxyAssetAhead(const vfs::Vfs& filesystem, std::vector<std::string> model_paths,
                                   std::unique_ptr<TexturePlanner> planner, const bool solid_state)
    : filesystem_(&filesystem), planner_(std::move(planner)), solid_state_(solid_state), model_paths_(std::move(model_paths)) {
    // The ordinal of each distinct model path (ASCII case-insensitive, like the model worker's own list): the step a texture hint carries.
    for (std::size_t index = 0; index < model_paths_.size(); ++index) {
        std::string key = model_paths_[index];
        std::transform(key.begin(), key.end(), key.begin(), [](const char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; });
        step_of_model_.emplace(std::move(key), index);
    }
}

GalaxyAssetAhead::~GalaxyAssetAhead() { finish(); }

void GalaxyAssetAhead::start() noexcept {
    if (started_) return;
    started_ = true;
    try {
        scene::TexturePrefetchOptions options;
        options.solid_state = solid_state_;
        textures_ = std::make_unique<scene::TexturePrefetch>(*filesystem_, options);
    } catch (...) {
        textures_.reset(); // no threads: the upload decodes and chains its own textures
    }
    try {
        scene::OrderedModelAheadListener* const listener = this; // the base is private: convert here, not inside make_unique
        models_ = std::make_unique<scene::OrderedModelAhead>(*filesystem_, model_paths_, listener, model_window_bytes);
        models_->start();
    } catch (...) {
        models_.reset();
    }
}

void GalaxyAssetAhead::model_ready(const std::size_t index, const std::string&, const scene::AheadModel& decoded) noexcept {
    if (!textures_ || !planner_ || !decoded.model) return;
    try {
        for (const TextureWant& want : planner_->wants(decoded.model.value()))
            textures_->offer(want.logical_path, index, want.processing);
    } catch (...) {
        // An offer is a hint: the upload asks for what it needs and decodes it itself when nobody offered it.
    }
}

scene::AheadModel GalaxyAssetAhead::take_model(const std::string_view logical_path) {
    // The upload reached this model, so the hints of the models before it are done with: an unrequested hint (a declared texture
    // the upload skipped) must not sit on the byte cap until finish() and throttle the readers.
    if (textures_) {
        std::string key(logical_path);
        std::transform(key.begin(), key.end(), key.begin(), [](const char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; });
        if (const auto found = step_of_model_.find(key); found != step_of_model_.end()) textures_->advance(found->second);
    }
    if (models_) return models_->take(logical_path);
    return scene::read_hash_decode_model(*filesystem_, logical_path);
}

core::Result<assets::Texture> GalaxyAssetAhead::take_texture(const std::string_view logical_path,
                                                             const scene::TextureProcessing processing) {
    if (textures_) return textures_->acquire(logical_path, processing);
    auto loaded = assets::load_texture(*filesystem_, logical_path);
    if (loaded) apply_galactic_chain(loaded.value(), processing);
    return loaded;
}

void GalaxyAssetAhead::finish() {
    if (models_) models_->finish();
    if (textures_) textures_->finish();
    if (reported_) return;
    reported_ = true;
    if (models_) {
        const auto stats = models_->stats();
        core::gc_load::trace.mark("gc.upload_models.ahead.models planned=" + std::to_string(stats.planned)
            + " decoded=" + std::to_string(stats.decoded) + " ready=" + std::to_string(stats.served_ready)
            + " inflight=" + std::to_string(stats.served_inflight) + " inline=" + std::to_string(stats.served_inline)
            + " unrequested=" + std::to_string(stats.unrequested) + " decode_ms=" + std::to_string(stats.decode_ms)
            + " wait_ms=" + std::to_string(stats.wait_ms) + " held_bytes_peak=" + std::to_string(stats.held_bytes_peak));
    }
    if (textures_) {
        const auto stats = textures_->stats();
        core::gc_load::trace.mark("gc.upload_models.ahead.textures readers=" + std::to_string(stats.readers)
            + " planned=" + std::to_string(stats.planned) + " decoded=" + std::to_string(stats.decoded)
            + " ready=" + std::to_string(stats.served_ready) + " inflight=" + std::to_string(stats.served_inflight)
            + " unstarted=" + std::to_string(stats.served_unstarted) + " unplanned=" + std::to_string(stats.unplanned)
            + " unrequested=" + std::to_string(stats.unrequested) + " decode_ms=" + std::to_string(stats.reader_decode_ms)
            + " wait_ms=" + std::to_string(stats.wait_ms) + " chained=" + std::to_string(stats.chained)
            + " chain_ms=" + std::to_string(stats.chain_ms) + " chained_by_loader=" + std::to_string(stats.chained_by_loader));
    }
}

} // namespace eawr::presentation::godot_backend
