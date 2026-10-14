# Public server setup plan (owner checklist)

This is the plan for the project's own public online server. It is a plan only: nothing here
has been rented, provisioned or deployed yet. Community operators who host their own server
follow [relay-server.md](relay-server.md) instead. That guide covers coturn, and coturn stays
supported for them.

Chosen setup (owner decision, 2026-10-14):

- The **introduction server** runs in a Docker container on a small **Hetzner** cloud server.
  It is our `eawr_relay_server`, which lets players find each other: invitation codes and,
  later, the room list, passwords and lobby chat. The server has a public IPv4 address and
  no tunnel.
- **Cloudflare Realtime TURN** relays game traffic for players who cannot connect directly.
  We don't run coturn for the public server.
- A domain name with a free **Let's Encrypt** certificate, so players need no certificate
  fingerprint.

Who does what:

- **[Owner]** means it needs your account, a payment or a decision.
- **[Claude]** means a coordinator or worker can do it.
- **[Code]** means it waits for code that has not merged yet:
  - optional publisher token (legacy EAWR-3603)
  - room list (legacy EAWR-3622)

## 0. Before you start

- [Owner] A Hetzner Cloud account with a payment method, and a Cloudflare account that
  manages the domain you want to use (for example `example.org`).
- [Owner] Pick the public host name, for example `play.example.org`. This guide uses that
  name, and `203.0.113.10` stands for the server's IPv4 address.
- [Claude] Tag a release of the repository that contains the relay server. You build the image
  from that tag (step 3).

## 1. Cloudflare: TURN key and DNS

1. [Owner] Cloudflare dashboard → **Realtime** → **TURN Server** → **Create**. Name it
   `eawr-public`. Cloudflare shows two values:
   - **Turn Token ID**, the key ID;
   - **API Token**, the key's secret.

   Copy both into a password manager now. The API token is shown only once. Treat it like a
   password: it can mint unlimited relay credentials, which bills against your account.
2. [Owner] Optional: in the Cloudflare **Billing** notifications, set an alert on Realtime usage.
   Cloudflare TURN costs $0.05 per GB sent to players. The first 1,000 GB each month is free
   (the free allowance is shared with Cloudflare's SFU). Check Cloudflare's current pricing. Our traffic is small, so we expect to
   stay free.
3. [Owner] DNS → add an **A record**:
   - name `play`
   - value `203.0.113.10`
   - proxy status **DNS only** (grey cloud)

   The introduction server speaks its own TLS protocol on TCP 27020. Cloudflare's proxy (orange
   cloud) and Cloudflare Tunnel only carry HTTP(S), so they would break it.
4. [Owner] Add an AAAA record as well, but only if the server has IPv6 and you want players to use it.

The server reads the TURN key at runtime. You will paste the two values into its secrets file
in step 5. They never go into the repository, a build or a chat message.

## 2. Hetzner: create the server

1. [Owner] Hetzner Cloud Console → **Add server**:
   - **Location:** Falkenstein, Nuremberg or Helsinki. EU locations include about 20 TB of
     traffic per month.
   - **Image:** Ubuntu 24.04.
   - **Type:** the smallest shared-vCPU plan with 2 vCPU and 4 GB (CX22/CX23 class, about €4–5 a
     month at the time of writing; check the console).
   - **Networking:** public IPv4 on. Public IPv6 is optional.
   - **SSH key:** add your public key. Don't use password login.
   - **Name:** `eawr-public`.
2. [Owner] Hetzner **Firewall** → create `eawr-public` and attach it to the server.

   | Direction | Protocol | Port | Source | Purpose |
   | --- | --- | --- | --- | --- |
   | In | TCP | 22 | your home IP only | SSH administration |
   | In | TCP | 27020 | any | introduction server (players) |
   | In | TCP | 80 | any | Let's Encrypt HTTP challenge only (see step 4) |
   | In | ICMP | — | any | optional, ping |

   Open no other inbound ports. The TURN relay runs on Cloudflare, not on this server.
3. [Owner] Write down the IPv4 address and put it in the DNS record from step 1.3.

## 3. Server software

Log in with `ssh root@203.0.113.10`, then:

```sh
apt-get update && apt-get -y upgrade
apt-get -y install docker.io git certbot unattended-upgrades
systemctl enable --now docker
dpkg-reconfigure -plow unattended-upgrades   # security updates for the OS
adduser --system --group --uid 10001 relay   # same UID/GID as the container user
mkdir -p /srv/eawr/config /srv/eawr/src
git clone https://github.com/<public repo> /srv/eawr/src
cd /srv/eawr/src && git checkout <release tag>     # [Claude] gives you the tag
docker build -f apps/relay_server/Dockerfile -t eawr-relay:<release tag> .
```

The build takes a few minutes on a 2-vCPU server. Later updates repeat only the last two lines
(step 8).

## 4. Certificate (Let's Encrypt)

```sh
certbot certonly --standalone -d play.example.org --agree-tos -m <your e-mail> --non-interactive
```

- Certbot answers the HTTP challenge on port 80 itself. Nothing else needs to run on port 80.
- Certificates live in `/etc/letsencrypt/live/play.example.org/`.
- They renew automatically through the `certbot.timer` that the package installs.

Copy the certificate to where the container can read it, and restart the container after each
renewal:

```sh
cat > /etc/letsencrypt/renewal-hooks/deploy/eawr-relay.sh <<'EOF'
#!/bin/sh
install -o relay -g relay -m 0400 /etc/letsencrypt/live/play.example.org/fullchain.pem /srv/eawr/config/cert.pem
install -o relay -g relay -m 0400 /etc/letsencrypt/live/play.example.org/privkey.pem  /srv/eawr/config/key.pem
docker restart eawr-relay 2>/dev/null || true
EOF
chmod 0755 /etc/letsencrypt/renewal-hooks/deploy/eawr-relay.sh
/etc/letsencrypt/renewal-hooks/deploy/eawr-relay.sh   # run once now
```

Let's Encrypt certificates last 90 days. Renewal runs about every 60 days.

## 5. Configuration and secrets

`/srv/eawr/config/relay.local.conf` must be owned by `relay`, mode `0400`:

```text
listen_address=0.0.0.0
port=27020
certificate_file=cert.pem
key_file=key.pem
# Public community server: no publisher token (owner answer, legacy #3461). [Code] key name from the publisher-token change (legacy #3603).
require_access_token=0
# Cloudflare Realtime TURN for relayed game traffic. The key ID and API token come from the
# environment file below, never from this file.
turn_provider=cloudflare
```

Optional keys, shown with their defaults: `cloudflare_turn_calls_per_minute=30` (at most this many
outbound Cloudflare calls in any rolling minute, pairings and revokes together) and
`cloudflare_turn_timeout_ms=4000` (how long one call may take; 500..30000).

`/srv/eawr/config/turn.env` must be owned by root, mode `0400`. It is read by Docker, not
mounted into the container:

```text
EAWR_RELAY_CLOUDFLARE_TURN_KEY_ID=<Turn Token ID from step 1.1>
EAWR_RELAY_CLOUDFLARE_TURN_API_TOKEN=<API Token from step 1.1>
```

These are the final names (they are the config keys `cloudflare_turn_key_id` and
`cloudflare_turn_api_token`, upper-cased with the `EAWR_RELAY_` prefix). The server refuses to
start, with `EAWR-NET-1555`, if `turn_provider=cloudflare` lacks either value, if the key ID contains
anything but letters, digits, `-` and `_`, or if `turn_uri` / `turn_secret` (the coturn keys) are also
set. Without a TURN provider, players behind strict NATs cannot connect and everyone else can.

## 6. First start and health check

```sh
docker run -d --name eawr-relay --restart unless-stopped \
  -p 27020:27020/tcp \
  --env-file /srv/eawr/config/turn.env \
  --mount type=bind,source=/srv/eawr/config,target=/config,readonly \
  --memory 512m --cpus 1.5 \
  eawr-relay:<release tag>
docker ps                          # STATUS shows "healthy" after about 30 s
docker logs --tail 50 eawr-relay   # sanitized; no secrets, codes or addresses
docker exec eawr-relay eawr_relay_server --health --config /config/relay.local.conf
```

`--restart unless-stopped` restarts the server after a crash or a reboot. The built-in
HEALTHCHECK runs every 30 seconds.

## 7. Smoke test from a second network

[Owner] with [Claude] guiding. Use two PCs on different networks, for example home Wi-Fi and a
phone hotspot.

1. Health from outside the server, on either PC:
   `eawr_relay_server --health --address <server IPv4> --port 27020 --certificate-name play.example.org`.
   It uses the system CA store and verifies the certificate chain and the DNS name;
   leave `--ca-file` and `--certificate-sha256` unset. It must print `healthy`.
   An untrusted certificate or wrong name fails with curated diagnostics.
2. In the game on both PCs: Online → the service is `play.example.org:27020`. Host on PC 1 and
   join on PC 2 with the 4-character code. Expect the lobby, Ready, Start and the battle.
3. TURN check: repeat with "relay only" enabled in the game's network options. The
   match still connects, and Cloudflare's Realtime dashboard shows a small amount of TURN traffic.
4. Failure checks:
   - a wrong code says the invitation is not available;
   - stopping the container (`docker stop eawr-relay`) makes a new Host fail with the service
     message, and a running match keeps going;
   - after `docker start eawr-relay` hosting works again.

Record the date, the release tag and the results on the release issue.

## 8. Updates

[Claude] announces a new release tag and says whether players need the matching game build. The
build identity must match, otherwise joins are refused with a build-mismatch message.

[Owner] on the server:

```sh
cd /srv/eawr/src && git fetch --tags && git checkout <new tag>
docker build -f apps/relay_server/Dockerfile -t eawr-relay:<new tag> .
docker stop eawr-relay && docker rm eawr-relay
# run the step 6 docker run command with eawr-relay:<new tag>
```

Updating drops open invitations and lobby rooms; running matches are not affected. Update when
few people are hosting. OS security updates install automatically (step 3). Reboot after kernel
updates: `reboot`, and the container comes back on its own.

## 9. Rollback

Keep the previous image tag. To go back:

```sh
docker stop eawr-relay && docker rm eawr-relay
# step 6 docker run command with eawr-relay:<previous tag>
```

If the TURN key may have leaked: Cloudflare dashboard → Realtime → TURN Server → delete the key,
create a new one (step 1.1), update `turn.env` and restart the container. If the server itself
is compromised: delete it in Hetzner, create a new one from step 2, and point DNS at the new
address.

## 10. Monthly cost (estimate)

| Item | Cost |
| --- | --- |
| Hetzner 2 vCPU / 4 GB server with IPv4 | about €4–5 |
| Cloudflare TURN | €0 up to 1,000 GB/month, then $0.05/GB |
| Domain | your existing domain; nothing extra |
| Let's Encrypt | free |

## Open items

- Optional publisher token and 4-character codes (legacy EAWR-3603). Public room list, passwords and chat
  (legacy EAWR-3622) plus a later UI slice.
- Ticket E: sessions longer than 2 hours and credential refresh.
- No container image is published. You build it from a release tag on the server (step 3).
