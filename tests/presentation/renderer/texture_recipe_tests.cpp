#include "../../support/check.hpp"
#include "texture_recipe.hpp"
#include "eawr/assets/memory.hpp"

#include <iostream>

int main() {
    using namespace eawr;
    using namespace presentation::godot_backend::detail;
    testing::Checker checks;
    const auto check = [&](const bool value, const char* message) { checks.check(value, message, std::cerr); };
    assets::Texture original;
    original.source.logical_path = "data/art/textures/test.dds";
    original.width = original.height = 1;
    original.mips.push_back({1, 1, 4, std::vector<std::byte>(4, std::byte{42})});
    auto disk = original;
    unsigned reads{};
    bool available = true;
    const TextureLoader load = [&](const assets::Source& source) -> std::optional<assets::Texture> {
        ++reads;
        check(source.logical_path == original.source.logical_path, "reload must use the original logical path");
        if (!available) return std::nullopt;
        return disk;
    };
    TextureRecipe recipe(original, load, texture_item(original));
    check(recipe.retained() && reads == 0, "before successful upload the original snapshot must remain");
    recipe.uploaded();
    check(!recipe.retained() && reads == 1, "verified upload must release every retained CPU mip");
    recipe.uploaded();
    check(reads == 1, "shared upload must not reverify a released recipe");
    const auto restored = recipe.read();
    check(restored && restored->mips[0].bytes == original.mips[0].bytes && reads == 2,
        "eviction must re-decode identical bytes on demand");
    check(!recipe.retained(), "restoration must not retain the transient decoded payload");
    disk.mips[0].bytes[0] = std::byte{3};
    check(!recipe.read(), "changed loose content must fail restore rather than alter its upload identity");
    disk = original;
    disk.mips[0].width = 2;
    check(!recipe.read(), "changed mip metadata must fail restore");
    disk = original;
    disk.has_alpha = !original.has_alpha;
    check(!recipe.read(), "changed alpha metadata must fail restore");
    disk = original;
    disk.source_origin = assets::ImageOrigin::bottom_left;
    check(!recipe.read(), "changed origin metadata must fail restore");
    available = false;
    check(!recipe.read(), "missing content must fail restore cleanly");
    TextureRecipe missing(original, load, texture_item(original));
    missing.uploaded();
    check(missing.retained() && missing.read(), "unresolvable textures must keep a usable fallback snapshot");
    available = true;
    disk = original;
    disk.mips[0].bytes[0] = std::byte{3};
    TextureRecipe transformed(original, load, texture_item(original));
    transformed.uploaded();
    check(transformed.retained() && transformed.read()->mips[0].bytes == original.mips[0].bytes,
        "transformed content must keep its exact upload snapshot");
    TextureRecipe generated(original, {}, texture_item(original));
    generated.uploaded();
    assets::PayloadProbe probe;
    probe.texture(generated.retained());
    probe.texture(generated.retained());
    probe.texture(recipe.retained());
    check(probe.result.textures == 1 && probe.result.texture_capacity.texture_payload_bytes == 4,
        "fallback bytes must remain counted once and reloadable payload must count zero");
    return checks.finish();
}
