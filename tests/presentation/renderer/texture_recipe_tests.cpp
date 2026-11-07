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
    // #5989: a texture whose mip chain was generated after loading is verified against its file's authored levels, and its
    // CPU snapshot is freed like any other; a later read gives the authored levels back and never regenerates anything.
    assets::Texture chained = original;
    chained.width = chained.height = 4;
    chained.mips.clear();
    chained.mips.push_back({4, 4, 16, std::vector<std::byte>(16, std::byte{42})});
    chained.mips.push_back({2, 2, 8, std::vector<std::byte>(8, std::byte{21})});
    chained.mips.push_back({1, 1, 4, std::vector<std::byte>(4, std::byte{10})});
    chained.source.authored_levels = 1;
    assets::Texture authored = chained;
    authored.mips.resize(1);
    authored.source.authored_levels = 0;
    assets::Texture file = authored;
    unsigned chained_reads{};
    const TextureLoader load_file = [&](const assets::Source&) -> std::optional<assets::Texture> {
        ++chained_reads;
        return file;
    };
    TextureRecipe generated_chain(chained, load_file, texture_item(chained));
    check(generated_chain.retained() != nullptr, "before upload the chained snapshot remains");
    generated_chain.uploaded();
    check(!generated_chain.retained() && chained_reads == 1, "a chained texture whose authored levels match the file frees its CPU snapshot");
    const auto readmitted = generated_chain.read();
    check(readmitted && readmitted->mips.size() == 1 && readmitted->mips[0].bytes == authored.mips[0].bytes && chained_reads == 2,
        "re-admission reads the authored levels alone, one file read and no generation");
    file.mips[0].bytes[0] = std::byte{3};
    check(!generated_chain.read(), "changed file content still fails the restore");
    file = authored;
    file.mips[0].bytes[0] = std::byte{3};
    TextureRecipe changed(chained, load_file, texture_item(chained));
    changed.uploaded();
    check(changed.retained(), "a chained texture whose file differs in its authored levels keeps its snapshot");
    file = authored;
    file.mips.push_back({2, 2, 8, std::vector<std::byte>(8, std::byte{21})});
    TextureRecipe longer(chained, load_file, texture_item(chained));
    longer.uploaded();
    check(longer.retained(), "a file with more levels than were authored keeps the snapshot");
    // A generated texture that does not name its authored levels is a transformed one: it keeps its snapshot.
    file = authored;
    assets::Texture unmarked = chained;
    unmarked.source.authored_levels = 0;
    TextureRecipe plain_generated(unmarked, load_file, texture_item(unmarked));
    plain_generated.uploaded();
    check(plain_generated.retained(), "without the marker a texture the file does not reproduce keeps its snapshot");
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
