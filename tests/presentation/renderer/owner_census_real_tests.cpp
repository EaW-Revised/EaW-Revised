#include "eawr/assets/geometry_stripped.hpp"
#include "eawr/diagnostics/owner_census.hpp"
#include "eawr/presentation/model_upload.hpp"
#include "eawr/scene/scene.hpp"
#include "eawr/vfs/vfs.hpp"
#include <array>
#include <iostream>
#include <memory>
#include <stdexcept>

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    using Census = eawr::diagnostics::OwnerCensus;
    const auto require = [](bool condition) { if (!condition) throw std::runtime_error("real owner contract"); };
    const std::array mounts{eawr::vfs::MountSpec{"fixture", argv[1], "data", {}}};
    auto mounted = eawr::vfs::Vfs::mount(mounts);
    require(static_cast<bool>(mounted));
    auto& files = mounted.value();
    eawr::scene::VfsAssetCache::enable_owner_census(files, true);
    eawr::scene::VfsAssetCache cache(files);
    eawr::presentation::ModelUpload recipe, duplicate;
    std::weak_ptr<const eawr::assets::Model> lifetime;
    const auto emit = [&](const char* name, bool renderer) {
        Census census;
        cache.owner_census(census);
        if (renderer) {
            census.model(recipe.model.get(), Census::renderer);
            census.model(duplicate.model.get(), Census::renderer);
        }
        std::cout << '"' << name << "\":";
        census.write(std::cout);
    };
    {
        auto loaded = cache.shared_model("data/art/models/owner.alo");
        require(static_cast<bool>(loaded));
        lifetime = loaded.value();
        recipe.model = loaded.value();
        duplicate.model = loaded.value();
        require(recipe.valid() && duplicate.model.get() == recipe.model.get());
    }
    std::cout << '{';
    emit("lru", false);
    std::cout << ',';
    emit("shared", true);
    cache.set_model_cache_budget(0);
    require(!lifetime.expired());
    std::cout << ',';
    emit("renderer", true);
    {
        eawr::scene::VfsAssetCache raw(files);
        require(raw.model("data/art/models/owner.alo") == recipe.model.get());
        Census census;
        cache.owner_census(census);
        census.model(recipe.model.get(), Census::renderer);
        census.model(duplicate.model.get(), Census::renderer);
        // Exercise the actual raw-wrapper role edge used by UnitEmitters.
        census.emitter_raw_wrapper(&raw);
        std::cout << ",\"raw\":";
        census.write(std::cout);
        recipe.model.reset();
        duplicate.model.reset();
        require(!lifetime.expired());
    }
    require(lifetime.expired());
    Census released;
    cache.owner_census(released);
    std::cout << ",\"released\":";
    released.write(std::cout);
    // #4645: the emitter metadata of a real decoded model keeps what the planners read and pins no decode.
    {
        eawr::assets::Model metadata;
        std::weak_ptr<const eawr::assets::Model> decoded_lifetime;
        std::size_t bones = 0, meshes = 0, proxies = 0;
        {
            auto loaded = cache.shared_model("data/art/models/owner.alo");
            require(static_cast<bool>(loaded));
            const auto& full = *loaded.value();
            require(!full.meshes.empty() && !full.meshes[0].submeshes.empty());
            metadata = eawr::assets::geometry_stripped_copy(full);
            bones = full.bones.size(); meshes = full.meshes.size(); proxies = full.proxies.size();
            require(metadata.bones.size() == bones && metadata.meshes.size() == meshes
                    && metadata.proxies.size() == proxies);
            for (std::size_t index = 0; index < meshes; ++index) {
                require(metadata.meshes[index].name == full.meshes[index].name
                        && metadata.meshes[index].bone == full.meshes[index].bone
                        && metadata.meshes[index].submeshes.empty());
            }
            require(eawr::assets::geometry_stripped(metadata) && !eawr::assets::geometry_stripped(full));
            decoded_lifetime = loaded.value();
        }
        cache.set_model_cache_budget(0);
        require(decoded_lifetime.expired());
    }
    std::cout << ",\"lifetime_expired\":true}\n";
}
