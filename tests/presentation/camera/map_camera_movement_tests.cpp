#include "map_camera_movement_support.hpp"

namespace eawr_map_camera_test {

input::RawEvent ctrl(input::RawEvent event) {
    event.modifiers = input::modifier::ctrl;
    return event;
}
input::RawEvent pointer(const float relative_x, const float relative_y) {
    input::RawEvent event = motion(640.0F, 360.0F, relative_x);
    event.relative_y = relative_y;
    return event;
}


} // namespace eawr_map_camera_test
