#!/usr/bin/env bash
set -euo pipefail

BIN="${BIN:-./kvstore}"
PORT="${PORT:-6391}"
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

if ! command -v "$NC_BIN" >/dev/null 2>&1; then
  echo "[FAIL] missing nc command: $NC_BIN" >&2
  exit 1
fi

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
persist_mode = aof
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

PING_OUT="$(send_resp '*1\r\n$4\r\nPING\r\n' | tr -d '\r')"
if [[ "$PING_OUT" != "+PONG" ]]; then
  echo "[FAIL] unexpected PING response: $PING_OUT" >&2
  exit 1
fi

echo "[PASS] config bootstrap ping"

SET_OUT="$(send_resp '*3\r\n$3\r\nSET\r\n$4\r\nname\r\n$3\r\nbob\r\n' | tr -d '\r')"
GET_OUT="$(send_resp '*2\r\n$3\r\nGET\r\n$4\r\nname\r\n' | tr -d '\r')"

if [[ "$SET_OUT" != "+OK" ]]; then
  echo "[FAIL] unexpected SET response: $SET_OUT" >&2
  exit 1
fi
if [[ "$GET_OUT" != '$3
bob' ]]; then
  echo "[FAIL] unexpected GET response: $GET_OUT" >&2
  exit 1
fi

echo "[PASS] config bootstrap set/get"
