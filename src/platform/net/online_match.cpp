#include "online_match.hpp"
#include <thread>

namespace eawr::presentation::godot_backend {
void OnlineAdmission::run(OnlineMatchOptions options, bool host, std::string alias, std::optional<net::OnlineRoomAdmission> admission) {
    auto fail = [this](core::Diagnostic error) { const std::lock_guard lock(mutex); failure = std::move(error); close_confirmed = true; };
    { const std::lock_guard lock(mutex); expires = std::chrono::steady_clock::now() + options.service.timeout; }
    const bool public_alias = options.service.public_alias;
    std::shared_ptr<net::NetworkGnsRoom> joined;
    if (host) {
        const auto give_up = std::chrono::steady_clock::now() + options.invitation_lifetime;
        std::optional<net::OnlineHostedRoom> hosted;
        if (!options.hosted_room) {
            core::Diagnostic error; error.code = "EAWR-NET-1530"; error.message = "This online match has no room source to host on.";
            fail(std::move(error)); return;
        }
        while (!cancelled.load() && !(hosted = options.hosted_room())) {
            if (options.hosting_failure) if (auto error = options.hosting_failure()) { fail(std::move(*error)); return; }
            if (std::chrono::steady_clock::now() >= give_up) {
                core::Diagnostic error; error.code = "EAWR-NET-1528"; error.message = "The room was not published in time.";
                fail(std::move(error)); return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (cancelled.load()) { close_confirmed = true; return; }
        auto opened = net::NetworkGnsRoom::open(options.service, hosted->publication, hosted->principal, options.identity, public_alias);
        if (!opened) { fail(opened.error()); return; }
        joined = std::move(opened).value();
        const auto shown = joined->alias();
        if (options.require_short_code && public_alias && options.short_invitation && shown.size() != 4) {
            core::Diagnostic error; error.code = std::string(online_match_errors::no_room_code);
            error.message = "This online service issues no room codes, so this room cannot be joined by code.";
            fail(std::move(error)); return;
        }
        const std::lock_guard lock(mutex);
        code = public_alias && options.short_invitation && shown.size() == 4 ? shown : std::string{};
        expires = joined->lease().expires;
    } else {
        auto attached = admission ? net::NetworkGnsRoom::attach(options.service, *admission, options.identity)
            : net::NetworkGnsRoom::attach_alias(options.service, std::move(alias), options.identity);
        if (!attached) { fail(attached.error()); return; }
        joined = std::move(attached).value();
    }
    const std::lock_guard lock(mutex);
    close_confirmed = true; // The service session is owned by the room member from here on.
    if (!cancelled.load()) room = std::move(joined);
}
}
