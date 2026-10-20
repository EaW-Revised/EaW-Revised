#include "lan_match_internal.hpp"

namespace eawr::presentation::godot_backend::lan_detail {

DriverResult TcpLanMatch::drive(const tactical::TacticalSetup& setup, platform::LiveStepContext& context,
    std::span<const tactical::PlayerCommand> commands, std::uint64_t now) {
    if (auto result = drive_prepare(setup, context, commands)) return *result;
    if (auto result = drive_commit(setup, context, now)) return *result;
    if (auto result = drive_rejoin(setup, context, now)) return *result;
    if (auto result = drive_submit(context, now)) return *result;
    return drive_session(setup, context, now);
}

} // namespace eawr::presentation::godot_backend::lan_detail
