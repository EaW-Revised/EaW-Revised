#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <unordered_set>

namespace eawr::presentation::godot_backend::detail {

// Blend classes of the merged-draw sizing census. Additive streams commute, so they are the
// first candidates; alpha and bump streams depend on draw order; the rest never merge.
enum class BlendGroup : std::uint8_t { additive, alpha, other };

// One frame of census counters; plain sums, so adding backends is adding structs.
struct ParticleBatchCounts final {
    std::uint64_t streams{}, vertices{}, identical{};
    std::array<std::uint64_t, 3> class_streams{}, class_keys{};
};

// Counts, without merging anything, how many draws a design that joins streams sharing one key
// (shader, colour texture, normal texture, render priority) would leave in a frame. A key is
// counted once per frame and per recorder; a new frame number starts the key set over.
class ParticleBatchCensus final {
public:
    [[nodiscard]] static std::uint64_t key(const std::size_t shader, const std::size_t texture,
        const std::size_t normal, const std::int32_t priority) noexcept {
        std::uint64_t value = 0xcbf29ce484222325ULL;
        for (const std::uint64_t part : {static_cast<std::uint64_t>(shader), static_cast<std::uint64_t>(texture),
                 static_cast<std::uint64_t>(normal), static_cast<std::uint64_t>(static_cast<std::uint32_t>(priority))}) {
            value = (value ^ part) * 0x100000001b3ULL;
            value ^= value >> 29;
        }
        return value;
    }

    void record(const std::uint64_t frame, const std::uint64_t stream_key, const BlendGroup kind,
                const std::size_t vertices, const bool identical, ParticleBatchCounts& counts) {
        if (frame != frame_) {
            frame_ = frame;
            keys_.clear();
        }
        const auto index = static_cast<std::size_t>(kind);
        ++counts.streams;
        ++counts.class_streams[index];
        counts.vertices += vertices;
        if (identical) ++counts.identical;
        if (keys_.insert(stream_key).second) ++counts.class_keys[index];
    }

private:
    std::uint64_t frame_{~std::uint64_t{0}};
    std::unordered_set<std::uint64_t> keys_;
};

} // namespace eawr::presentation::godot_backend::detail
