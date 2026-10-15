#include "online_match.hpp"
#include <thread>

namespace eawr::presentation::godot_backend {
void OnlineAdmission::run(OnlineMatchOptions options, bool host, std::string code_to_join) {
    auto fail = [this](core::Diagnostic error) { const std::lock_guard lock(mutex); failure = std::move(error); close_confirmed = true; };
    { const std::lock_guard lock(mutex); expires = std::chrono::steady_clock::now() + options.invitation_lifetime + options.service.timeout; }
    const bool public_invitation = options.service.public_alias;
    net::NetworkGnsIntroduction service(std::move(options.service));
    std::shared_ptr<net::GnsSignallingEndpoint> endpoint;
    if (host) {
        auto result = service.publish(options.identity, options.invitation_lifetime);
        if (!result) { fail(result.error()); return; }
        auto introduction = std::move(result).value(); endpoint = std::move(introduction.endpoint);
        const std::lock_guard lock(mutex);
        bridge_code = introduction.join_code;
        bridge_owner = introduction.publisher_ownership;
        code = public_invitation && options.short_invitation && introduction.room_alias.size() == 4 ? introduction.room_alias : std::string{};
        expires = introduction.expires;
    } else {
        auto result = service.join(std::move(code_to_join), options.identity);
        if (!result) { fail(result.error()); return; }
        endpoint = std::move(result).value();
    }
    while (!cancelled.load()) {
        if (!endpoint->rendezvous().remote_identity.empty()) {
            const std::lock_guard lock(mutex);
            close_confirmed = true; // Pairing consumed the invitation.
            if (!cancelled.load()) paired = std::move(endpoint);
            return;
        }
        auto result = endpoint->status();
        if (!result) { fail(result.error()); return; }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    close_confirmed = !host || !endpoint->rendezvous().remote_identity.empty() || static_cast<bool>(endpoint->close_publication());
}
}
