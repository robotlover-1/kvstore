#!/usr/bin/env bash
set -euo pipefail

BIN="${BIN:-./kvstore}"
PORT="${PORT:-6392}"
HOST="127.0.0.1"
NC_BIN="${NC_BIN:-nc}"
TIMEOUT_BIN="${TIMEOUT_BIN:-timeout}"

WORKDIR="$(mktemp -d)"
CONF_FILE="$WORKDIR/kvstore.conf"
LOG_FILE="$WORKDIR/server.log"
SERVER_PID=""

cleanup() {
  if [ -n "${SERVER_PID}" ] && kill -0 "${SERVER_PID}" 2>/dev/null; then
    kill "${SERVER_PID}" 2>/dev/null || true
    wait "${SERVER_PID}" 2>/dev/null || true
  fi
  rm -rf "$WORKDIR"
}
trap cleanup EXIT

cat >"$CONF_FILE" <<CONF
bind_ip = $HOST
port = $PORT
role = master
master_host = 127.0.0.1
master_port = 5000
dump_path = $WORKDIR/kvstore.dump
aof_path = $WORKDIR/kvstore.aof
appendfsync = always
autosnap =
mem_backend = libc
net_backend = reactor
log_mode = console
persist_mode = none
sentinel = false
sentinel_master_name = mymaster
sentinel_monitor_host = 127.0.0.1
sentinel_monitor_port = 5000
sentinel_known_slaves =
sentinel_down_after_ms = 5000
sentinel_failover_timeout_ms = 10000
sentinel_quorum = 1
CONF

"$BIN" --config "$CONF_FILE" >"$LOG_FILE" 2>&1 &
SERVER_PID=$!

for _ in $(seq 1 30); do
  if (echo >/dev/tcp/$HOST/$PORT) >/dev/null 2>&1; then
    break
  fi
  sleep 0.1
done

send_resp() {
  local payload="$1"
  if command -v "$TIMEOUT_BIN" >/dev/null 2>&1; then
    printf '%b' "$payload" | "$TIMEOUT_BIN" 2 "$NC_BIN" "$HOST" "$PORT" 2>/dev/null || true
  else
    printf '%b' "$payload" | "$NC_BIN" "$HOST" "$PORT" 2>/dev/null || true
  fi
}

SAVE_OUT="$(send_resp '*1\r\n$4\r\nSAVE\r\n' | tr -d '\r')"
if [[ "$SAVE_OUT" != "-ERR snapshot disabled by persist_mode" ]]; then
  echo "[FAIL] SAVE should be blocked by persist_mode=none, got: $SAVE_OUT" >&2
  exit 1
fi

CFG_OUT="$(send_resp '*3\r\n$6\r\nCONFIG\r\n$10\r\nAPPENDFSYNC\r\n$7\r\nalways\r\n' | tr -d '\r')"
if [[ "$CFG_OUT" != "-ERR aof disabled by persist_mode" ]]; then
  echo "[FAIL] CONFIG APPENDFSYNC should be blocked when aof disabled, got: $CFG_OUT" >&2
  exit 1
fi

INFO_OUT="$(send_resp '*1\r\n$4\r\nINFO\r\n' | tr -d '\r')"
if [[ "$INFO_OUT" != *"persist_mode:none"* ]]; then
  echo "[FAIL] INFO missing persist_mode:none, got: $INFO_OUT" >&2
  exit 1
fi
if [[ "$INFO_OUT" != *"aof_enabled:0"* ]]; then
  echo "[FAIL] INFO missing aof_enabled:0, got: $INFO_OUT" >&2
  exit 1
fi

echo "[PASS] persist_mode none guards"
