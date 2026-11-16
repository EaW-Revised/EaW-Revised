"""The loss knob of the fake TURN server (tests/net/gns_turn_server.py), checked without GNS.

Two servers play two providers ("own" and "cloudflare"); each relays packets from a peer to a client and from the client to the peer
through the server's own data path. A seeded loss knob drops a share of them, can be changed mid-run, and never touches the control
path (allocations and permissions are set up directly here). The relay-switch design relies on this rig: one provider degrades in
the middle of a session while the other stays clean.
"""
import os
import socket
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gns_turn_server import Allocation, TurnServer  # noqa: E402

N = 3000


def rig(loss, seed):
    server = TurnServer("127.0.0.1", allocation_lifetime=3600, permission_lifetime=3600, loss_pct=loss, seed=seed)
    client = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    client.bind(("127.0.0.1", 0))
    client.settimeout(0.01)
    relay = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    relay.bind(("127.0.0.1", 0))
    allocation = Allocation(relay, client.getsockname(), 3600)
    allocation.permissions["127.0.0.1"] = time.monotonic() + 3600
    return server, client, allocation


def received(client):
    count = 0
    while True:
        try:
            client.recvfrom(2048)
            count += 1
        except socket.timeout:
            return count


def peer_to_client(server, client, allocation, packets):
    count = 0
    for i in range(packets):
        server._from_peer(allocation, b"x" * 20 + bytes([i % 256]), ("127.0.0.1", 40000))
        if i % 25 == 24:  # drain as we go: a loopback socket buffer would otherwise drop datagrams by itself
            count += received(client)
    return count + received(client)


def main():
    # No loss: everything arrives.
    server, client, allocation = rig(0.0, 1)
    assert peer_to_client(server, client, allocation, 500) == 500, "no loss: every packet arrives"
    # 30% loss: close to 30% disappear, and the count is repeatable for a seed.
    results = []
    for _ in range(2):
        server, client, allocation = rig(30.0, 7)
        results.append(peer_to_client(server, client, allocation, N))
        assert server.dropped_by_loss + results[-1] == N, "every packet is either relayed or counted as lost"
    assert results[0] == results[1], "the same seed gives the same losses"
    share = 100.0 * (N - results[0]) / N
    assert 26.0 <= share <= 34.0, f"30% knob dropped {share:.1f}%"
    # The knob changes while the server runs: clean, then lossy, then clean again.
    server, client, allocation = rig(0.0, 3)
    first = peer_to_client(server, client, allocation, 500)
    server.set_loss(40.0)
    second = peer_to_client(server, client, allocation, N)
    server.set_loss(0.0)
    third = peer_to_client(server, client, allocation, 500)
    assert first == 500 and third == 500, "a provider that is clean before and after a lossy stretch loses nothing there"
    assert 0.52 * N <= second <= 0.68 * N, f"40% knob delivered {100.0 * second / N:.1f}%"
    # The other direction (client -> peer through the relay socket) is lossy too, and 100% drops everything.
    peer = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    peer.bind(("127.0.0.1", 0))
    peer.settimeout(0.01)
    allocation.relay.close()
    allocation.relay = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    allocation.relay.bind(("127.0.0.1", 0))
    server.set_loss(100.0)
    from gns_turn_server import DATA, XOR_PEER, _attribute, _xor_address  # noqa: E402
    # _send reads the peer address from XOR-PEER-ADDRESS and the payload from DATA: build the message the way a client would.
    from gns_turn_server import _attributes  # noqa: E402
    wire = b"\0" * 20 + _xor_address(XOR_PEER, peer.getsockname()) + _attribute(DATA, b"payload")
    server.allocations[allocation.client] = allocation
    for _ in range(50):
        server._send(_attributes(wire), allocation.client)
    assert received(peer) == 0, "100% loss drops the client -> peer direction"
    server.set_loss(0.0)
    for _ in range(50):
        server._send(_attributes(wire), allocation.client)
    assert received(peer) == 50, "no loss: the client -> peer direction relays everything"
    print("fake TURN loss knob contracts passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
