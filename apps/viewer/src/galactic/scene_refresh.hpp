#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace eawr::viewer::galactic {

// The world (0) stays live. Each nonworld pass consumes its predecessor's
// filtered colour, so changing an upstream pass also changes every successor.
class SceneRefresh final {
public:
    void invalidate(const std::size_t layer) noexcept {
        if (layer == 0 || layer >= dirty_.size()) return;
        for (std::size_t index = layer; index < dirty_.size(); ++index) dirty_[index] = true;
    }
    void invalidate_all() noexcept { invalidate(1); }

    // A removed owner still contributes pixels to the retained texture until
    // its layer is redrawn. Cover both live work and the first frame after any
    // teardown (including an in-game load), without dirtying upstream passes.
    void observe_dynamic_owners(const std::array<bool, 4>& owners) noexcept {
        for (std::size_t layer = 1; layer < owners.size(); ++layer)
            if (owners[layer] || dynamic_owners_[layer]) invalidate(layer);
        dynamic_owners_ = owners;
    }

    // Called once just before a rendered frame. Hidden owners retain pending
    // work; returning to the galaxy refreshes every cached texture before use.
    [[nodiscard]] std::array<bool, 4> begin_frame(const bool visible) noexcept {
        if (!visible) {
            visible_ = false;
            return {};
        }
        if (!visible_) invalidate_all();
        visible_ = true;
        std::array<bool, 4> refresh{true, false, false, false};
        for (std::size_t index = 1; index < dirty_.size(); ++index) {
            refresh[index] = dirty_[index];
            if (refresh[index]) {
                ++requests_[index];
                valid_[index] = true;
                dirty_[index] = false;
            }
        }
        return refresh;
    }

    [[nodiscard]] bool valid(const std::size_t layer) const noexcept { return valid_[layer]; }
    [[nodiscard]] std::uint64_t requests(const std::size_t layer) const noexcept { return requests_[layer]; }

private:
    bool visible_{};
    std::array<bool, 4> dirty_{false, true, true, true};
    std::array<bool, 4> valid_{};
    std::array<std::uint64_t, 4> requests_{};
    std::array<bool, 4> dynamic_owners_{};
};

} // namespace eawr::viewer::galactic
