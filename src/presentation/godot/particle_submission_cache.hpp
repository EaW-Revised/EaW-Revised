#pragma once

#include <cstdint>
#include <optional>

namespace eawr::presentation::godot_backend::detail {

struct ParticleCallCounts final {
    std::uint64_t attempted{}, sent{}, skipped{};
};

// Remember actual submitted values, independently of requested culling state.
// Exact equality only: a changed component always reaches the server.
template<class Value>
class ParticleSubmittedValue final {
public:
    template<class Send>
    void apply(const Value& value, const Send& send, ParticleCallCounts* counts = nullptr) {
        if (counts) ++counts->attempted;
        if (last_ && *last_ == value) {
            if (counts) ++counts->skipped;
            return;
        }
        send(value);
        last_ = value;
        if (counts) ++counts->sent;
    }
    void reset() noexcept { last_.reset(); }
private:
    std::optional<Value> last_;
};

template<class Bounds>
struct ParticleSubmissionCache final {
    ParticleSubmittedValue<Bounds> bounds;
    ParticleSubmittedValue<bool> visibility;

    // Resource/surface identity changes invalidate both observations. The first
    // subsequent request is submitted even if its value matches the old owner.
    void reset() noexcept { bounds.reset(); visibility.reset(); }
};

} // namespace eawr::presentation::godot_backend::detail
