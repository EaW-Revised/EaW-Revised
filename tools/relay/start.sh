#!/bin/sh
set -eu
umask 077
relay_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
relay_root=$(CDPATH= cd -- "$relay_directory/../.." && pwd)
relay_binary=${EAWR_RELAY_BINARY:-"$relay_root/out/relay/eawr_relay_server"}
relay_config=${1:-"$relay_root/out/relay/relay.local.conf"}
relay_log_directory=${EAWR_RELAY_LOG_DIR:-"$relay_root/out/relay/logs"}
case "$relay_binary" in /*) ;; *) relay_binary="$(pwd)/$relay_binary" ;; esac
case "$relay_config" in /*) ;; *) relay_config="$(pwd)/$relay_config" ;; esac
case "$relay_log_directory" in /*) ;; *) relay_log_directory="$(pwd)/$relay_log_directory" ;; esac
mkdir -p "$relay_log_directory"
if [ ! -x "$relay_binary" ] || [ ! -f "$relay_config" ]; then
    echo 'Build the standalone relay and create relay.local.conf as described in docs/deploy/relay-server.md.' >&2
    exit 1
fi
cd "$(dirname -- "$relay_config")"
# Foreground process: Ctrl-C stops it. No logon task or host firewall mutation.
exec "$relay_binary" --config "$relay_config" >> "$relay_log_directory/relay.log" 2>&1
