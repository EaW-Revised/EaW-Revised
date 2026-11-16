#pragma once

#include "eawr/assets/assets.hpp"
#include "eawr/core/result.hpp"
#include "eawr/platform/live_game_session.hpp"
#include "eawr/scene/ordered_model_ahead.hpp"
#include "eawr/scene/texture_prefetch.hpp"
#include "eawr/vfs/vfs_fwd.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::presentation::godot_backend {

// Decode-ahead of what the galaxy scene upload reads: its planet, station, fleet and cursor models (one read each, hashed from
// the bytes in hand) and the textures those models' surfaces ask for, with their load-time mip chain. All of it runs on
// read-ahead threads while the preparation's serial stages run, so the upload (which creates the GPU objects, on the loader's
// thread) finds decoded, chained textures instead of reading, decoding and chaining them itself. Presentation only: the upload
// keeps its own order and its own results; a model or texture the read-ahead does not have is decoded by the upload as before.
//
// It starts when the space entry stage has finished (the campaign preparation tells it), so on a spinning disk it never reads
// beside that stage's readers; the model worker is one thread and the texture readers follow the texture read-ahead's
// disk-kind policy.
class GalaxyAssetAhead final : public platform::CampaignPrepareObserver, private scene::OrderedModelAheadListener {
public:
    struct TextureWant final {
        std::string logical_path; // resolved: the file the upload will ask for
        scene::TextureProcessing processing{scene::TextureProcessing::none};
    };
    // The textures a decoded model's surfaces will ask for, in the order the upload asks. Runs on the model worker thread.
    class TexturePlanner {
    public:
        virtual ~TexturePlanner() = default;
        [[nodiscard]] virtual std::vector<TextureWant> wants(const assets::Model& model) const = 0;
    };

    GalaxyAssetAhead(const vfs::Vfs& filesystem, std::vector<std::string> model_paths, std::unique_ptr<TexturePlanner> planner,
                     bool solid_state);
    ~GalaxyAssetAhead() override;
    GalaxyAssetAhead(const GalaxyAssetAhead&) = delete;
    GalaxyAssetAhead& operator=(const GalaxyAssetAhead&) = delete;

    void space_entry_joined() noexcept override { start(); }
    // Starts the texture readers and the model worker. Once; the upload calls it too, so a load that never signals still gets
    // the single read and the chain off the loader's thread (without the head start). Thread contract: every call (this one,
    // space_entry_joined and the upload's) is made on the one thread that runs the galaxy build_view's create, before it returns;
    // `started_` is a plain bool and is not safe against two threads.
    void start() noexcept;
    [[nodiscard]] scene::AheadModel take_model(std::string_view logical_path);
    // What assets::load_texture would return plus the offered chain; the loader's own decode when the read-ahead is not running.
    [[nodiscard]] core::Result<assets::Texture> take_texture(std::string_view logical_path, scene::TextureProcessing processing);
    // Stops every thread and writes one trace line per read-ahead (the counters).
    void finish();

private:
    void model_ready(std::size_t index, const std::string& logical_path, const scene::AheadModel& decoded) noexcept override;

    const vfs::Vfs* filesystem_;
    std::unique_ptr<TexturePlanner> planner_;
    bool solid_state_;
    std::unique_ptr<scene::TexturePrefetch> textures_;
    std::unique_ptr<scene::OrderedModelAhead> models_;
    std::vector<std::string> model_paths_;
    std::map<std::string, std::uint64_t> step_of_model_; // lower-case model path -> its ordinal (the step of its texture hints)
    bool started_{};
    bool reported_{};
};

} // namespace eawr::presentation::godot_backend
