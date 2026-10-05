#pragma once
#include "eawr/presentation/ui/command_sink.hpp"
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::presentation::ui {
enum class CursorTarget : std::uint8_t { none, enemy, friendly, terrain, space_position };
struct CursorInput final {
    bool dragging{};
    bool camera_pan{};
    bool camera_rotate{};
    bool hud{};
    bool own_selection{};
    bool movable_selection{};
    bool selectable{};
    bool hostile{};
    bool passable{true};
    bool placing{};
    bool placement_valid{};
    bool repair{};
    bool ctrl{};
    bool alt{};
    OrderMode mode{OrderMode::none};
    CursorTarget ability{CursorTarget::none};
    bool ability_valid{};
};
// CU-04..11: pure resolution of already evaluated input, with no world scans.
[[nodiscard]] std::string_view battle_cursor(const CursorInput& input) noexcept;
// Retain diagnostic transitions only. Comparing an unchanged ID never constructs a string,
// including after the bounded history fills; callers may pass temporary views.
class CursorHistory final {
public:
    struct Sample { std::uint64_t frame{}; std::string id; std::string hover; };
    void record(std::uint64_t frame, std::string_view id, std::string_view hover);
    [[nodiscard]] const std::vector<Sample>& samples() const noexcept { return samples_; }
private:
    std::string id_;
    std::string hover_;
    std::vector<Sample> samples_;
};
// CU-02: wall time since this pointer was installed; no simulation clock.
[[nodiscard]] std::size_t cursor_frame(double seconds, std::uint32_t delay, std::size_t frames) noexcept;
}
