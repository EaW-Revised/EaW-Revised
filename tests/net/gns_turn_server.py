"""A small RFC 5766 TURN server whose permissions and allocations really expire (#3912).

Only what GNS's native ICE client uses over UDP: Allocate, Refresh, CreatePermission, Send and Data indications and
the Binding response, with long-term credentials. Unlike a test double that remembers a permission forever, a
permission here stops forwarding `permission_lifetime` seconds after it was last created, as coturn does after 300 s.
Everything else (channels, TCP, IPv6) is out of scope: the client under test never asks for it.

With `nonce_lifetime` the server also rotates its nonce like coturn does: a Refresh or CreatePermission that still
carries the previous nonce gets `438 Stale Nonce` with the new one, and only a request repeated with it succeeds.
"""
import hashlib
import hmac
import select
import socket
import struct
import threading
import time

MAGIC = 0x2112A442
BINDING_REQUEST, BINDING_SUCCESS = 0x0001, 0x0101
ALLOCATE_REQUEST, ALLOCATE_SUCCESS, ALLOCATE_ERROR = 0x0003, 0x0103, 0x0113
REFRESH_REQUEST, REFRESH_SUCCESS, REFRESH_ERROR = 0x0004, 0x0104, 0x0114
SEND_INDICATION, DATA_INDICATION = 0x0016, 0x0017
PERMISSION_REQUEST, PERMISSION_SUCCESS, PERMISSION_ERROR = 0x0008, 0x0108, 0x0118
USERNAME, INTEGRITY, ERROR_CODE, REALM, NONCE = 0x0006, 0x0008, 0x0009, 0x0014, 0x0015
XOR_MAPPED, XOR_RELAYED, XOR_PEER, DATA, LIFETIME = 0x0020, 0x0016, 0x0012, 0x0013, 0x000D


def _pad(size):
    return (4 - size % 4) % 4


def _attribute(kind, body):
    return struct.pack("!HH", kind, len(body)) + body + b"\0" * _pad(len(body))


def _xor_address(kind, address):
    host, port = address
    cookie = struct.pack("!I", MAGIC)
    ip = bytes(a ^ b for a, b in zip(socket.inet_aton(host), cookie))
    return _attribute(kind, struct.pack("!BBH", 0, 1, port ^ (MAGIC >> 16)) + ip)


def _decode_xor_address(body):
    if len(body) < 8 or body[1] != 1:
        return None
    port = struct.unpack_from("!H", body, 2)[0] ^ (MAGIC >> 16)
    ip = bytes(a ^ b for a, b in zip(body[4:8], struct.pack("!I", MAGIC)))
    return socket.inet_ntoa(ip), port


def _message(kind, transaction, *attributes):
    body = b"".join(attributes)
    return struct.pack("!HHI", kind, len(body), MAGIC) + transaction + body


def _attributes(data):
    result, offset = [], 20
    while offset + 4 <= len(data):
        kind, size = struct.unpack_from("!HH", data, offset)
        offset += 4
        if offset + size > len(data):
            break
        result.append((kind, data[offset:offset + size], offset - 4))
        offset += size + _pad(size)
    return result


class Allocation:
    def __init__(self, relay, client, lifetime):
        self.relay, self.client = relay, client
        self.expires = time.monotonic() + lifetime
        self.permissions = {}  # peer ip -> expiry
        self.permission_requests = 0
        self.refreshes = 0
        self.dropped_without_permission = 0
        self.relayed = 0


class TurnServer(threading.Thread):
    def __init__(self, host, allocation_lifetime=6, permission_lifetime=6, username="eawr-turn-test",
                 password="eawr-turn-secret", realm="eawr.test", blackhole_after=None, nonce_lifetime=None):
        super().__init__(daemon=True)
        self.host = host
        self.allocation_lifetime, self.permission_lifetime = allocation_lifetime, permission_lifetime
        self.realm = realm
        self.nonce_lifetime = nonce_lifetime  # seconds between nonce rotations; None = one nonce for ever
        self.started = time.monotonic()
        self.stale_nonce_replies = 0
        self.key = hashlib.md5(f"{username}:{realm}:{password}".encode()).digest()
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.socket.bind((host, 0))
        self.port = self.socket.getsockname()[1]
        self.allocations = {}
        self.by_relay = {}
        self.blackhole_after = blackhole_after  # seconds of relayed traffic before the path dies for good
        self.first_relayed = None
        self.blackhole_from = None
        self.stopping = threading.Event()
        self.lock = threading.Lock()

    def stop(self):
        self.stopping.set()
        self.join(timeout=5)
        self.socket.close()
        for allocation in list(self.allocations.values()):
            allocation.relay.close()

    def statistics(self):
        with self.lock:
            return [{"client": a.client, "relay_port": a.relay.getsockname()[1], "permission_requests": a.permission_requests,
                     "refreshes": a.refreshes, "dropped": a.dropped_without_permission, "relayed": a.relayed}
                    for a in self.allocations.values()]

    @property
    def nonce(self):
        epoch = 0 if self.nonce_lifetime is None else int((time.monotonic() - self.started) / self.nonce_lifetime)
        return f"eawr-test-nonce-{epoch:04d}".encode()

    # -- long-term credentials (RFC 5389, section 10.2) -------------------------------------------------------------
    def _authorised(self, data, attributes, transaction, client, error_kind, check_nonce=False):
        found = {kind: (body, offset) for kind, body, offset in attributes}
        integrity = found.get(INTEGRITY)
        if USERNAME in found and REALM in found and NONCE in found and integrity:
            prefix = bytearray(data[:integrity[1]])
            struct.pack_into("!H", prefix, 2, integrity[1] - 20 + 24)
            expected = hmac.new(self.key, bytes(prefix), hashlib.sha1).digest()
            if hmac.compare_digest(expected, integrity[0]):
                if not check_nonce or found[NONCE][0] == self.nonce:
                    return True
                self.stale_nonce_replies += 1
                self.socket.sendto(_message(error_kind, transaction, _attribute(ERROR_CODE, struct.pack("!HBB", 0, 4, 38) + b"Stale Nonce"),
                                            _attribute(REALM, self.realm.encode()), _attribute(NONCE, self.nonce)), client)
                return False
        challenge = _message(error_kind, transaction, _attribute(ERROR_CODE, struct.pack("!HBB", 0, 4, 1) + b"Unauthorized"),
                             _attribute(REALM, self.realm.encode()), _attribute(NONCE, self.nonce))
        self.socket.sendto(challenge, client)
        return False

    def _error(self, kind, transaction, client, code, text):
        self.socket.sendto(_message(kind, transaction, _attribute(
            ERROR_CODE, struct.pack("!HBB", 0, code // 100, code % 100) + text)), client)

    # -- requests ----------------------------------------------------------------------------------------------------
    def _allocate(self, data, attributes, transaction, client):
        if not self._authorised(data, attributes, transaction, client, ALLOCATE_ERROR):
            return
        if client in self.allocations and self.allocations[client].expires > time.monotonic():
            return self._error(ALLOCATE_ERROR, transaction, client, 437, b"Allocation Mismatch")
        relay = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        relay.bind((self.host, 0))
        allocation = Allocation(relay, client, self.allocation_lifetime)
        with self.lock:
            self.allocations[client] = allocation
            self.by_relay[relay] = allocation
        self.socket.sendto(_message(ALLOCATE_SUCCESS, transaction, _xor_address(XOR_RELAYED, relay.getsockname()),
            _xor_address(XOR_MAPPED, client), _attribute(LIFETIME, struct.pack("!I", self.allocation_lifetime))), client)

    def _refresh(self, data, attributes, transaction, client):
        if not self._authorised(data, attributes, transaction, client, REFRESH_ERROR, check_nonce=True):
            return
        allocation = self.allocations.get(client)
        if allocation is None or allocation.expires <= time.monotonic():
            return self._error(REFRESH_ERROR, transaction, client, 437, b"No Allocation")
        allocation.expires = time.monotonic() + self.allocation_lifetime
        allocation.refreshes += 1
        self.socket.sendto(_message(REFRESH_SUCCESS, transaction, _attribute(
            LIFETIME, struct.pack("!I", self.allocation_lifetime))), client)

    def _permission(self, data, attributes, transaction, client):
        if not self._authorised(data, attributes, transaction, client, PERMISSION_ERROR, check_nonce=True):
            return
        allocation = self.allocations.get(client)
        if allocation is None or allocation.expires <= time.monotonic():
            return self._error(PERMISSION_ERROR, transaction, client, 437, b"No Allocation")
        for kind, body, _ in attributes:
            if kind == XOR_PEER and (peer := _decode_xor_address(body)):
                allocation.permissions[peer[0]] = time.monotonic() + self.permission_lifetime
        allocation.permission_requests += 1
        self.socket.sendto(_message(PERMISSION_SUCCESS, transaction), client)

    def _permitted(self, allocation, ip):
        now = time.monotonic()
        if self.blackhole_after is not None and self.first_relayed is not None and now - self.first_relayed >= self.blackhole_after:
            self.blackhole_from = self.blackhole_from or now
            return False
        if allocation.expires <= now or allocation.permissions.get(ip, 0) <= now:
            return False
        self.first_relayed = self.first_relayed or now
        return True

    def _send(self, attributes, client):
        allocation = self.allocations.get(client)
        peer = next((_decode_xor_address(body) for kind, body, _ in attributes if kind == XOR_PEER), None)
        payload = next((body for kind, body, _ in attributes if kind == DATA), None)
        if allocation is None or peer is None or payload is None:
            return
        if not self._permitted(allocation, peer[0]):
            allocation.dropped_without_permission += 1
            return
        allocation.relay.sendto(payload, peer)

    def _from_peer(self, allocation, payload, peer):
        if not self._permitted(allocation, peer[0]):
            allocation.dropped_without_permission += 1
            return
        allocation.relayed += 1
        self.socket.sendto(_message(DATA_INDICATION, struct.pack("!III", 0, 0, 0), _xor_address(XOR_PEER, peer),
                                    _attribute(DATA, payload)), allocation.client)

    def run(self):
        while not self.stopping.is_set():
            with self.lock:
                sockets = [self.socket, *self.by_relay]
            ready, _, _ = select.select(sockets, [], [], 0.05)
            for sock in ready:
                try:
                    data, source = sock.recvfrom(65535)
                except OSError:
                    continue
                if sock is not self.socket:
                    self._from_peer(self.by_relay[sock], data, source)
                    continue
                if len(data) < 20 or struct.unpack_from("!I", data, 4)[0] != MAGIC:
                    continue
                kind, transaction, attributes = struct.unpack_from("!H", data)[0], data[8:20], _attributes(data)
                if kind == BINDING_REQUEST:
                    self.socket.sendto(_message(BINDING_SUCCESS, transaction, _xor_address(XOR_MAPPED, source)), source)
                elif kind == ALLOCATE_REQUEST:
                    self._allocate(data, attributes, transaction, source)
                elif kind == REFRESH_REQUEST:
                    self._refresh(data, attributes, transaction, source)
                elif kind == PERMISSION_REQUEST:
                    self._permission(data, attributes, transaction, source)
                elif kind == SEND_INDICATION:
                    self._send(attributes, source)


def lan_address():
    """A non-loopback IPv4 address of this host, or None. GNS never asks a TURN server to permit loopback peers."""
    probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        probe.connect(("192.0.2.1", 9))  # TEST-NET-1: selects a route, sends nothing
        address = probe.getsockname()[0]
    except OSError:
        return None
    finally:
        probe.close()
    return None if address.startswith("127.") or address == "0.0.0.0" else address
