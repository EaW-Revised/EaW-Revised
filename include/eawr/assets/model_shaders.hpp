#pragma once

// Header-only model shader scan: which shader program each mesh of a model names, and whether the mesh is stored visible,
// read from the chunk headers alone. The geometry, textures and parameters are never read, so a whole mod's models can be
// swept for a load check without decoding them (docs/load-notes.md, shader routes). The reader is positional so a caller
// can seek inside a loose file or an archive entry instead of reading the asset whole.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

namespace eawr::assets {

struct MeshShaderName final {
    std::string shader;
    // The mesh's stored visibility flag (a visible mesh is one a renderer draws, before any bone or variant gating).
    bool visible{};
};

// Fills `out` with the bytes at `offset` and returns true, or returns false when the range cannot be read.
// A non-owning view of a callable `bool(std::uint64_t offset, std::span<std::byte> out)` that outlives the scan call.
class PositionalReader final {
public:
    template <class F, class = std::enable_if_t<!std::is_same_v<std::decay_t<F>, PositionalReader>>>
    PositionalReader(const F& callable) noexcept // NOLINT(google-explicit-constructor): lets a lambda be passed directly
        : object_(&callable), call_([](const void* object, const std::uint64_t offset, const std::span<std::byte> out) {
              return (*static_cast<const F*>(object))(offset, out);
          }) {}
    [[nodiscard]] bool operator()(const std::uint64_t offset, const std::span<std::byte> out) const { return call_(object_, offset, out); }

private:
    const void* object_;
    bool (*call_)(const void*, std::uint64_t, std::span<std::byte>);
};

struct ModelShaderScan final {
    std::vector<MeshShaderName> meshes;
    // False when the chunk structure stopped making sense (a truncated or malformed file); `meshes` then holds what was
    // read before the fault.
    bool complete{true};
};

// One entry per submesh, in file order. A model with no mesh chunk yields an empty list. Bounded: no more than
// `max_meshes` mesh chunks and 256 bytes of any shader name are read.
[[nodiscard]] ModelShaderScan scan_model_shaders(std::uint64_t size, const PositionalReader& read,
                                                 std::size_t max_meshes = 4096);

// The same over a byte buffer.
[[nodiscard]] ModelShaderScan scan_model_shaders(std::span<const std::byte> bytes);

} // namespace eawr::assets
