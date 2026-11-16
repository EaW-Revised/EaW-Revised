# Own TURN server (coturn) next to Cloudflare

The public relay hands out Cloudflare Realtime TURN credentials ([public-server-setup.md](public-server-setup.md)). Some
internet routes lose a quarter of the packets towards Cloudflare's TURN addresses (141.101.90.0/24) while the relay
host itself is fine. This runbook adds **our own coturn** on the relay host. The relay then sends both servers in the
same credential frames; by default the game **measures which of the two is nearer** (round-trip time, below) and tries that
one first, and a player can force either provider (last section).

**The fallback is failure-based, not loss-based.** The game's network layer allocates on the first server in the list
that *answers* and only moves to the next when that server does not answer or refuses. A server that answers but then
loses or delays packets is not detected, and the game does not switch. For a route that is bad towards one provider
the cure is the order (nearest first, chosen when the game connects) or forcing a provider in the settings, not the automatic
fallback. The order is chosen by distance only, never by a loss prediction (a per-flow black hole cannot be predicted by a probe).
A server that leaves two or more of its three probe requests unanswered counts as unreachable for the order; that is a coarse
demotion, not a loss estimate (three probes cannot tell 0% from 25% loss). **Known limit:** Cloudflare's TURN addresses ignore
unauthenticated requests, so the probe measures `stun.cloudflare.com` (a different prefix), which can look perfect while the TURN
prefix loses packets. The probe is blind to exactly that failure; the in-match route-health monitor (it measures the real flow)
is the answer to it, not the connect-time order.

With the STUN list (docs/gns-transport.md, "STUN") most pairs are direct and TURN carries only the pairs that cannot connect
directly, so our server's bandwidth is the failures' bandwidth, not the whole game's.

Nothing here changes the current online path until step B6. Cloudflare keeps working at every step; rollback is to
delete two config lines and restart the relay (B8).

Who runs what: **Part A** only needs the Hetzner Cloud API (the firewall); **Part B** is a root shell on the relay host.
Secrets are generated on the host into root-only files and are never printed or pasted.

## Part A: firewall (Hetzner Cloud API or console)

Add these rules to the firewall attached to the relay server (rule names in the description):

| Direction | Protocol | Port | Source | Description |
| --- | --- | --- | --- | --- |
| In | UDP | 3478 | any IPv4 | eawr-coturn listen (STUN / TURN) |
| In | UDP | 49160-49360 | any IPv4 | eawr-coturn relay range |

- Open nothing else (coturn listens on IPv4 only here). TCP 3478 and TLS 5349 are **not** needed: the game's native ICE uses UDP TURN only, so no
  certificate is needed for coturn either.
- The existing rules (TCP 22, 27020, 80) stay as they are.
- The 201-port relay range allows about 200 simultaneous allocations (a nine-player room with every pair relayed needs
  at most 72, see relay-server.md; step B2 caps the total at 150).

Check from any other network after Part B (it needs only Python 3):
`python tools/relay/turn_check.py eaw.ernst.work` prints `coturn answered 20/20` and the round-trip time.

## Part B: on the relay host (`ssh root@eaw.ernst.work`)

Paths follow public-server-setup.md: config in `/srv/eawr/config`, container `eawr-relay`.

### B1. Pick the image and the address

```sh
IP=$(curl -s4 https://ifconfig.me)           # the public IPv4 of this host; must equal the DNS A record of eaw.ernst.work
docker pull coturn/coturn:4.6.2
docker image inspect coturn/coturn:4.6.2 --format '{{index .RepoDigests 0}}'    # note the digest; use it in B4 for a pinned start
```

### B2. Create the secret and both config files (no output)

```sh
umask 077
mkdir -p /srv/eawr-turn
SECRET=$(openssl rand -hex 32)                       # 64 characters, the relay wants 32..256
cat > /srv/eawr-turn/coturn.conf <<EOF
listening-port=3478
listening-ip=0.0.0.0
min-port=49160
max-port=49360
fingerprint
use-auth-secret
static-auth-secret=$SECRET
realm=eaw.ernst.work
no-tls
no-dtls
no-tcp
no-tcp-relay
no-cli
no-multicast-peers
no-software-attribute
stale-nonce=600
user-quota=24
total-quota=150
max-bps=2500000
bps-capacity=25000000
simple-log
log-file=stdout
denied-peer-ip=0.0.0.0-0.255.255.255
denied-peer-ip=10.0.0.0-10.255.255.255
denied-peer-ip=100.64.0.0-100.127.255.255
denied-peer-ip=127.0.0.0-127.255.255.255
denied-peer-ip=169.254.0.0-169.254.255.255
denied-peer-ip=172.16.0.0-172.31.255.255
denied-peer-ip=192.0.0.0-192.0.0.255
denied-peer-ip=192.168.0.0-192.168.255.255
denied-peer-ip=198.18.0.0-198.19.255.255
denied-peer-ip=224.0.0.0-255.255.255.255
denied-peer-ip=::1
denied-peer-ip=$IP
EOF
chmod 0400 /srv/eawr-turn/coturn.conf
chown 65534:65534 /srv/eawr-turn/coturn.conf     # the container runs as nobody
printf 'EAWR_RELAY_TURN_FALLBACK_SECRET=%s\n' "$SECRET" > /srv/eawr-turn/turn.env.next
chmod 0400 /srv/eawr-turn/turn.env.next
unset SECRET
```

`turn.env.next` is a new file; B6 appends it to `turn.env`. Check the two files exist and are mode 0400
(`ls -l /srv/eawr/config`), never `cat` them.

What the limits do: `user-quota` allocations per credential (a room mesh of nine uses at most 8 per player),
`total-quota` allocations on the whole server, `max-bps` 2.5 MB/s per session (a 100 MB co-op save in about 40 s),
`bps-capacity` 25 MB/s for the whole server, and `denied-peer-ip` keeps relayed traffic away from loopback, private and
internal addresses (this includes the Docker bridge range 172.16.0.0/12) and from the host's own public address (`$IP`,
set in B1), so a valid credential cannot reach services on the relay host. Run B2 in the shell where `IP` is set.

### B3. Start coturn

```sh
docker run -d --name eawr-coturn --restart unless-stopped --network host \
  --cap-drop ALL --cap-add NET_BIND_SERVICE --memory 256m --cpus 1 \
  --log-opt max-size=10m --log-opt max-file=3 \
  --mount type=bind,source=/srv/eawr-turn/coturn.conf,target=/etc/coturn/turnserver.conf,readonly \
  coturn/coturn:4.6.2 -c /etc/coturn/turnserver.conf --external-ip="$IP"
docker logs --tail 20 eawr-coturn            # "Listener opened on : 0.0.0.0:3478"; no secret is logged
ss -lun | grep ':3478 '                      # coturn listens on UDP 3478
```

`--network host` lets coturn use the relay port range without a port-by-port mapping. The container runs as the
image's unprivileged user. The `turnserver` binary carries a file capability, so `--cap-add NET_BIND_SERVICE` is needed
for it to start (with every capability dropped it fails with "Operation not permitted", checked on 2026-11-11), and
`no-new-privileges` must not be set. The config file must be readable by that user (the `chown` in step B2). The files live in
`/srv/eawr-turn/` on the relay host, next to, not inside, `/srv/eawr/config`.

### B4. Test from outside (before the relay uses it)

On a PC on another network: `python tools/relay/turn_check.py eaw.ernst.work`. Expected: `coturn answered 20/20`.
If it prints `0/20`, Part A is not in place or the DNS name does not point to this host. Nothing else has changed yet.

### B5. Test endpoint (optional but recommended): a second relay on port 27021

The new relay code (this PR) issues both providers. Test it on its own port first, so the live relay on 27020 is
untouched. Build the image at the private commit named in the redeploy instruction the coordinator gives you (the same
steps as `out/coord/mp/relay-redeploy-1020-private.md`, but tag it `eawr-relay:turn-<sha8>`), then:

The test relay lives entirely in its own directory, `/srv/eawr-turn/relay-test/`, as copies of the live files. Nothing under
`/srv/eawr/config` is changed, so the live relay and its files cannot be affected by this step. Never print the env or key
files (the commands below only copy them).

```sh
umask 077
T=/srv/eawr-turn/relay-test
mkdir -p "$T"
cp /srv/eawr/config/relay.local.conf "$T/relay.test.conf"
cp /srv/eawr/config/cert.pem /srv/eawr/config/key.pem "$T/"
printf 'turn_fallback_uri=turn:%s:3478\n' "$IP" >> "$T/relay.test.conf"
sed -i 's/^port=.*/port=27021/' "$T/relay.test.conf"
# environment for the container: the live relay's own variables plus the coturn secret from B2 (read by docker as root)
cat /srv/eawr/config/turn.env /srv/eawr-turn/turn.env.next > "$T/turn.test.env"
chown 10001:10001 "$T/relay.test.conf" "$T/cert.pem" "$T/key.pem"      # the user the relay container runs as
chmod 0400 "$T/relay.test.conf" "$T/cert.pem" "$T/key.pem" "$T/turn.test.env"
docker run -d --name eawr-relay-test --restart unless-stopped \
  -p 27021:27021/tcp --env-file "$T/turn.test.env" \
  --mount type=bind,source="$T",target=/config,readonly \
  --memory 512m --cpus 1 --ulimit nofile=4096:4096 \
  --health-cmd 'eawr_relay_server --health --config /config/relay.test.conf' \
  eawr-relay:turn-<sha8> --config /config/relay.test.conf
docker ps                      # "healthy" after about 30 s
docker exec eawr-relay-test eawr_relay_server --health --config /config/relay.test.conf
```

If the live `relay.local.conf` names the certificate and key with another path than `/config/cert.pem` and `/config/key.pem`,
copy them under the names it uses. Also open TCP 27021 in the firewall (Part A) for the test only. The two-PC test points the
game at port 27021 (`eawr_online_test host|join --port 27021 --turn-provider own` and again with `cloudflare`; the log shows
`ROUTE: relay (own)` / `ROUTE: relay (cloudflare)`). Remove everything afterwards:

```sh
docker rm -f eawr-relay-test
rm -r /srv/eawr-turn/relay-test        # the copies of the config, certificate, key and environment
```

and delete the temporary 27021 firewall rule (Part A).

### B6. Switch the live relay on (only after the clients that understand the lists are released)

Old clients reject a frame that carries a username per server, so this step waits until the game version with the
server list handling is out. Then, with the new relay image deployed on 27020 (the redeploy instruction):

```sh
printf 'turn_fallback_uri=turn:%s:3478\n' "$IP" >> /srv/eawr/config/relay.local.conf
cat /srv/eawr-turn/turn.env.next >> /srv/eawr/config/turn.env && rm /srv/eawr-turn/turn.env.next
docker restart eawr-relay
docker exec eawr-relay eawr_relay_server --health --config /config/relay.local.conf
docker logs --tail 20 eawr-relay                  # "relay ready port=27020"; an EAWR-NET-1555 means a bad value, see B8
```

### B7. Verify live

Two players: one match with "relay only" on shows Cloudflare traffic and ours in the logs. On the host:
`docker logs --since 5m eawr-coturn | grep -c allocation` counts allocations; `docker stats --no-stream eawr-coturn`
shows CPU and memory. A player whose route to our server is bad sets the provider to `cloudflare` (below).

### B8. Rollback

1. Live relay (only if B6 was done): remove the line `turn_fallback_uri=` from `relay.local.conf`, remove
   `EAWR_RELAY_TURN_FALLBACK_SECRET` from `turn.env`, `docker restart eawr-relay`. The relay is back to Cloudflare only.
   The previous relay image stays available for a full rollback.
2. coturn: `docker stop eawr-coturn` (keep the container and `/srv/eawr-turn` for the next try; `docker rm eawr-coturn` and
   `rm -r /srv/eawr-turn` remove it completely). Test relay: `docker rm -f eawr-relay-test`, delete its files.
3. Firewall (Part A): delete the two rules by their descriptions, `eawr-coturn listen` and `eawr-coturn relay range`
   (and the temporary `eawr-relay-test` TCP 27021 rule if it was added). The firewall then holds its four original
   rules again: SSH from the owner's home address only (/32), TCP 27020, TCP 80 and ICMP. Compare with the rule list
   saved before Part A (the Cloud API `get firewall` output saved to a file by the person who added the rules).

## What the relay does with it

- Config keys: `turn_fallback_uri` (one `turn:` URI; use the numeric IPv4 address so clients need no lookup) and `turn_fallback_secret` (32..256 bytes; set
  `EAWR_RELAY_TURN_FALLBACK_SECRET`, never on the command line). Both or neither; only with `turn_provider=cloudflare`.
- Each pairing, room attach and renewal asks Cloudflare as before and adds an HMAC credential for our coturn
  (`username = <expiry>:<random>`, the same scheme as `turn_provider=coturn`). The frame then carries
  `turn:turn.cloudflare.com:3478,turn:turn.cloudflare.com:443,turn:<ip>:3478` with one username and password per server
  (the wire order is fixed, Cloudflare first, because the relay revokes the first username; each client orders the
  servers by its own setting).
- Revoking a session revokes only Cloudflare's credential; coturn's credential expires with its lifetime.
- If our credential cannot be made or would not fit the wire limits, the frame is sent with Cloudflare only.

## Choosing the provider in the game

Setting `network.online_turn_provider` in the player settings file (`auto`, `own` or `cloudflare`),
or `--turn-provider` for `eawr_online_test`:

| Value | The game uses |
| --- | --- |
| `auto` (default) | the nearer of the two first, the other after it (used only if the first does not answer): three STUN binding requests to each, at most 100 ms per server, in the endpoint worker thread; our coturn goes first unless it is more than 15 ms slower than Cloudflare's edge, so a player in Belgium or Germany uses our server and a player in the US or Asia uses Cloudflare's local edge. If neither probe answers the configured order (ours first) stays |
| `own` | only our coturn |
| `cloudflare` | only Cloudflare |

Two players may use different values; each side allocates on its own choice and the two relay addresses talk to each
other. With `own` or `cloudflare` and a service that does not offer that provider, the player has no relay at all (with
"relay only" on, the connection fails with the usual no-relay error).
