#!/usr/bin/env bash
# Prepares the dedicated release signing host (see README.md in this folder). Run it ONCE as root on a fresh
# Debian 12 or Ubuntu 24.04 container or VM that does nothing else, from a checkout of this repository:
#
#   OPERATOR_CIDR=<network allowed to reach ssh> DROP_PUBKEY_FILE=<public key file of the release operator> \
#   RUNNER_VERSION=<actions runner version> RUNNER_SHA256=<its sha256 from the runner release page> \
#   bash tools/release/signing_host/setup-signing-host.sh
#
# It creates no credential and asks for none. It does not register the GitHub runner: the repository owner does that
# step himself on the console (README, "Register the runner"), because the registration token is a credential.
# Re-running is safe (every step checks first). `selfcheck` prints the state without changing anything.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$HERE/../../.." && pwd)"
SIGN_USER=eawr-signer
DROP_USER=eawr-drop
BASE=/srv/eawr-signing
DROP=$BASE/drop
TOOLS_DIR=/opt/eawr-signing
RUNNER_DIR=/opt/actions-runner

die() { echo "setup-signing-host: $*" >&2; exit 1; }
need_root() { [ "$(id -u)" -eq 0 ] || die "run as root"; }

config_value() { python3 - "$REPO_ROOT/config/signing-tools.json" "$1" <<'PY'
import json, sys
node = json.load(open(sys.argv[1], encoding="utf-8"))
for key in sys.argv[2].split("."):
    node = node[key]
print(node)
PY
}

selfcheck() {
  local ok=0
  echo "== signing host self-check"
  command -v java >/dev/null && java -version 2>&1 | head -1 || { echo "MISSING java"; ok=1; }
  command -v osslsigncode >/dev/null && echo "osslsigncode: present" || { echo "MISSING osslsigncode"; ok=1; }
  local jar="$TOOLS_DIR/jsign-$(config_value jsign.version).jar"
  if [ -f "$jar" ] && [ "$(sha256sum "$jar" | cut -d' ' -f1)" = "$(config_value jsign.sha256)" ]; then echo "jsign: pinned jar ok"; else echo "jsign: jar missing or not the pinned one"; ok=1; fi
  local guard="$TOOLS_DIR/job-guard.sh"
  if [ -f "$guard" ] && [ "$(stat -c '%U:%G:%a' "$guard")" = "root:root:755" ]; then echo "job guard: installed (root-owned 0755)"; else echo "job guard: MISSING or not root-owned 0755: $guard"; ok=1; fi
  if [ -f "$guard" ]; then
    if cmp -s "$guard" "$HERE/job-guard.sh"; then echo "job guard: same as the repository copy"; else echo "job guard: differs from the repository copy (reinstall it by rerunning the setup)"; fi
  fi
  if grep -qxF "ACTIONS_RUNNER_HOOK_JOB_STARTED=$guard" "$RUNNER_DIR/.env" 2>/dev/null; then echo "job guard hook: set in $RUNNER_DIR/.env"; else echo "job guard hook: NOT set in $RUNNER_DIR/.env"; ok=1; fi
  for d in inbox outbox work; do ls -ld "$DROP/$d" 2>/dev/null || { echo "MISSING $DROP/$d"; ok=1; }; done
  echo "sudoers entries for $SIGN_USER: $(grep -rl "$SIGN_USER" /etc/sudoers /etc/sudoers.d 2>/dev/null | wc -l) (expected 0)"
  echo "az config for $SIGN_USER: $(test -e "/home/$SIGN_USER/.azure" && echo PRESENT || echo none) (expected none)"
  echo "listening sockets:"; ss -ltnH 2>/dev/null | awk '{print "  " $4}'
  return $ok
}

if [ "${1:-}" = selfcheck ]; then selfcheck; exit $?; fi

need_root
: "${OPERATOR_CIDR:?set OPERATOR_CIDR to the network that may reach ssh (for example a single /32)}"
: "${DROP_PUBKEY_FILE:?set DROP_PUBKEY_FILE to the ssh public key file of the release operator}"
: "${RUNNER_VERSION:?set RUNNER_VERSION (a version from the actions/runner releases page)}"
: "${RUNNER_SHA256:?set RUNNER_SHA256 (the sha256 shown for actions-runner-linux-x64 on that release page)}"
[[ "$OPERATOR_CIDR" =~ ^[0-9a-fA-F:.]+/[0-9]+$ ]] || die "OPERATOR_CIDR looks wrong"
[ -s "$DROP_PUBKEY_FILE" ] || die "DROP_PUBKEY_FILE is empty or missing"
grep -qE '^(ssh-ed25519|ecdsa-sha2-nistp[0-9]+|sk-ssh-ed25519@openssh.com) ' "$DROP_PUBKEY_FILE" || die "DROP_PUBKEY_FILE is not a public key"
grep -q 'PRIVATE KEY' "$DROP_PUBKEY_FILE" && die "that file holds a PRIVATE key: never copy one here"

echo "== packages"
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends openjdk-17-jre-headless python3 git curl ca-certificates \
  osslsigncode openssh-server nftables unattended-upgrades

echo "== users and directories"
id "$SIGN_USER" >/dev/null 2>&1 || useradd --system --create-home --shell /bin/bash --user-group "$SIGN_USER"
passwd -l "$SIGN_USER" >/dev/null
id "$DROP_USER" >/dev/null 2>&1 || useradd --system --no-create-home --home-dir / --shell /usr/sbin/nologin "$DROP_USER"
usermod -aG "$SIGN_USER" "$DROP_USER"      # the drop user reads outbox files through its group
install -d -m 0755 -o root -g root "$BASE" "$DROP"            # sshd needs a root-owned chroot
install -d -m 2750 -o "$DROP_USER" -g "$SIGN_USER" "$DROP/inbox"   # operator writes, the runner only reads
install -d -m 2750 -o "$SIGN_USER" -g "$DROP_USER" "$DROP/outbox"  # runner writes, the operator only reads
install -d -m 0700 -o "$SIGN_USER" -g "$SIGN_USER" "$DROP/work"
mkdir -p /etc/ssh/authorized_keys.d
{ echo -n 'restrict '; head -n1 "$DROP_PUBKEY_FILE"; } > /etc/ssh/authorized_keys.d/$DROP_USER
chmod 0644 /etc/ssh/authorized_keys.d/$DROP_USER

echo "== sshd (key only; the drop user can only use sftp inside the drop folder)"
cat > /etc/ssh/sshd_config.d/50-eawr-signing.conf <<EOF
PasswordAuthentication no
KbdInteractiveAuthentication no
PermitRootLogin no
X11Forwarding no
AllowAgentForwarding no
Match User $DROP_USER
    ChrootDirectory $DROP
    ForceCommand internal-sftp -u 0027
    AuthorizedKeysFile /etc/ssh/authorized_keys.d/%u
    AllowTcpForwarding no
    PermitTunnel no
    PermitTTY no
EOF
sshd -t
systemctl enable --now ssh >/dev/null 2>&1 || systemctl enable --now sshd >/dev/null 2>&1 || true
systemctl reload ssh 2>/dev/null || systemctl reload sshd 2>/dev/null || true

echo "== pinned signing tools"
install -d -m 0755 -o root -g root "$TOOLS_DIR"
JAR="$TOOLS_DIR/jsign-$(config_value jsign.version).jar"
if [ ! -f "$JAR" ] || [ "$(sha256sum "$JAR" | cut -d' ' -f1)" != "$(config_value jsign.sha256)" ]; then
  curl -fsSL -o "$JAR.part" "$(config_value jsign.url)"
  [ "$(sha256sum "$JAR.part" | cut -d' ' -f1)" = "$(config_value jsign.sha256)" ] || { rm -f "$JAR.part"; die "jsign download does not match the pinned SHA-256"; }
  chmod 0444 "$JAR.part"; mv "$JAR.part" "$JAR"
fi

echo "== job guard (the runner refuses every job except Sign release, started by hand from main)"
install -m 0755 -o root -g root "$HERE/job-guard.sh" "$TOOLS_DIR/job-guard.sh"

echo "== inbound firewall (ssh from $OPERATOR_CIDR only; no other inbound)"
cat > /etc/nftables.conf <<EOF
#!/usr/sbin/nft -f
flush ruleset
table inet filter {
  chain input {
    type filter hook input priority 0; policy drop;
    iif lo accept
    ct state established,related accept
    ip saddr $OPERATOR_CIDR tcp dport 22 accept
    ip6 saddr $OPERATOR_CIDR tcp dport 22 accept
    icmp type echo-request accept
  }
  chain forward { type filter hook forward priority 0; policy drop; }
}
EOF
# an IPv6 CIDR in the ip rule (or the reverse) makes nft refuse the file: keep only the matching family
if [[ "$OPERATOR_CIDR" == *:* ]]; then sed -i '/ip saddr/d' /etc/nftables.conf; else sed -i '/ip6 saddr/d' /etc/nftables.conf; fi
nft -c -f /etc/nftables.conf
systemctl enable --now nftables >/dev/null 2>&1 || true
nft -f /etc/nftables.conf

echo "== automatic security updates"
dpkg-reconfigure -f noninteractive unattended-upgrades >/dev/null 2>&1 || true

echo "== GitHub Actions runner files (not registered)"
if [ ! -x "$RUNNER_DIR/config.sh" ]; then
  install -d -m 0755 -o "$SIGN_USER" -g "$SIGN_USER" "$RUNNER_DIR"
  tarball="actions-runner-linux-x64-${RUNNER_VERSION}.tar.gz"
  curl -fsSL -o "/tmp/$tarball" "https://github.com/actions/runner/releases/download/v${RUNNER_VERSION}/$tarball"
  [ "$(sha256sum "/tmp/$tarball" | cut -d' ' -f1)" = "$RUNNER_SHA256" ] || { rm -f "/tmp/$tarball"; die "runner download does not match RUNNER_SHA256"; }
  runuser -u "$SIGN_USER" -- tar -xzf "/tmp/$tarball" -C "$RUNNER_DIR"
  rm -f "/tmp/$tarball"
fi

echo "== job guard hook in the runner environment"
GUARD_ENV="$RUNNER_DIR/.env"
if [ -d "$RUNNER_DIR" ]; then
  { grep -v '^ACTIONS_RUNNER_HOOK_JOB_STARTED=' "$GUARD_ENV" 2>/dev/null || true; echo "ACTIONS_RUNNER_HOOK_JOB_STARTED=$TOOLS_DIR/job-guard.sh"; } > "$GUARD_ENV.new"
  if [ -f "$GUARD_ENV" ]; then cat "$GUARD_ENV.new" > "$GUARD_ENV"; else install -m 0644 -o "$SIGN_USER" -g "$SIGN_USER" "$GUARD_ENV.new" "$GUARD_ENV"; fi
  rm -f "$GUARD_ENV.new"
fi

echo
selfcheck || true
cat <<'EOF'

Next (README, "Register the runner"): the repository owner opens this machine's console and runs, as the runner user:
  cd /opt/actions-runner && ./config.sh --url <repository url> --name eawr-signer --labels eawr-signing --work _work
and types the one-hour registration token at the prompt, then (as root) ./svc.sh install eawr-signer && ./svc.sh start
EOF
