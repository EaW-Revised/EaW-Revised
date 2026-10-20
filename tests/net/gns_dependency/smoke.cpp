#include "../../support/check.hpp"

#include <iostream>

#if EAWR_HAS_GNS
#include <steam/steamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>
#include "../../../src/net/platform_socket.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <exception>
#include <thread>

namespace {
HSteamNetConnection incoming = k_HSteamNetConnection_Invalid;
void changed(SteamNetConnectionStatusChangedCallback_t* event) {
    if (event->m_info.m_hListenSocket != k_HSteamListenSocket_Invalid
        && event->m_info.m_eState == k_ESteamNetworkingConnectionState_Connecting) {
        incoming = event->m_hConn;
        if (SteamNetworkingSockets()->AcceptConnection(incoming) != k_EResultOK)
            SteamNetworkingSockets()->CloseConnection(incoming, 0, "smoke accept failed", false);
    }
}
void expect(bool ok, const char* reason) {
    if (!ok) eawr::testing::require(ok, reason);
}
struct Runtime {
    Runtime() {
        SteamNetworkingErrMsg reason{};
        expect(GameNetworkingSockets_Init(nullptr, reason), reason);
    }
    ~Runtime() { GameNetworkingSockets_Kill(); }
};
void loopback() {
    Runtime runtime;
    auto* sockets = SteamNetworkingSockets();
    SteamNetworkingIPAddr address{};
    expect(address.ParseString("127.0.0.1:0"), "parse loopback address");
    SteamNetworkingConfigValue_t callback;
    callback.SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,
        reinterpret_cast<void*>(changed));
    auto listener = k_HSteamListenSocket_Invalid;
    // The pinned GNS API requires a nonzero listen port. Probe the OS's UDP
    // ephemeral allocator, release the probe and retry a raced bind at most 8 times.
    for (unsigned attempt = 0; attempt < 8 && listener == k_HSteamListenSocket_Invalid; ++attempt) {
        const auto probe = eawr::net::socket::udp_open("127.0.0.1", 0);
        expect(probe.handle != eawr::net::socket::invalid && probe.port != 0, "probe ephemeral UDP port");
        address.m_port = probe.port;
        eawr::net::socket::close(probe.handle);
        listener = sockets->CreateListenSocketIP(address, 1, &callback);
    }
    expect(listener != k_HSteamListenSocket_Invalid, "create listener");
    expect(sockets->GetListenSocketAddress(listener, &address) && address.m_port != 0, "bound ephemeral port");
    const auto outgoing = sockets->ConnectByIPAddress(address, 1, &callback);
    expect(outgoing != k_HSteamNetConnection_Invalid, "create connection");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    bool sent{}, received{}, echoed{};
    constexpr std::array<unsigned char, 4> payload{0, 1, 6, 255};
    while (!echoed) {
        expect(std::chrono::steady_clock::now() < deadline, "reliable loopback deadline");
        sockets->RunCallbacks();
        SteamNetConnectionInfo_t info{};
        if (!sent && sockets->GetConnectionInfo(outgoing, &info)
            && info.m_eState == k_ESteamNetworkingConnectionState_Connected) {
            expect(sockets->SendMessageToConnection(outgoing, payload.data(),
                static_cast<uint32>(payload.size()), k_nSteamNetworkingSend_Reliable, nullptr) == k_EResultOK,
                "send reliable message");
            sent = true;
        }
        if (incoming != k_HSteamNetConnection_Invalid && !received) {
            SteamNetworkingMessage_t* message{};
            if (sockets->ReceiveMessagesOnConnection(incoming, &message, 1) == 1) {
                const bool same = message->m_cbSize == static_cast<int>(payload.size())
                    && std::equal(payload.begin(), payload.end(), static_cast<unsigned char*>(message->m_pData));
                message->Release();
                expect(same, "whole reliable payload");
                expect(sockets->SendMessageToConnection(incoming, payload.data(),
                    static_cast<uint32>(payload.size()), k_nSteamNetworkingSend_Reliable, nullptr) == k_EResultOK,
                    "send reliable echo");
                received = true;
            }
        }
        SteamNetworkingMessage_t* message{};
        if (sockets->ReceiveMessagesOnConnection(outgoing, &message, 1) == 1) {
            const bool same = message->m_cbSize == static_cast<int>(payload.size())
                && std::equal(payload.begin(), payload.end(), static_cast<unsigned char*>(message->m_pData));
            message->Release();
            expect(same, "whole reliable echo");
            echoed = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    sockets->CloseConnection(outgoing, 0, "smoke complete", false);
    sockets->CloseConnection(incoming, 0, "smoke complete", false);
    sockets->CloseListenSocket(listener);
}
}
#endif

int main() {
#if EAWR_HAS_GNS
    try {
        loopback();
        std::cout << "GNS static dependency: reliable UDP loopback roundtrip passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
#else
    std::cout << "GNS disabled: dependency smoke skipped\n";
    return 77;
#endif
}
