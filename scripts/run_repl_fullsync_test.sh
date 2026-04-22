#!/usr/bin/env bash
set -euo pipefail

BIN="${BIN:-./kvstore}"
BUILD_CMD="${BUILD_CMD:-}"
MASTER_PORT="${MASTER_PORT:-6379}"
SLAVE_PORT="${SLAVE_PORT:-6380}"
BASE_DIR="${BASE_DIR:-./tmp_repl_test}"
PRELOAD_COUNT="${PRELOAD_COUNT:-5000}"
DURING_SYNC_WRITES="${DURING_SYNC_WRITES:-200}"
STARTUP_WAIT="${STARTUP_WAIT:-1}"
SYNC_WAIT="${SYNC_WAIT:-8}"
INJECT_DELAY="${INJECT_DELAY:-0.2}"
NC_BIN="${NC_BIN:-nc}"
TIMEOUT_BIN="${TIMEOUT_BIN:-timeout}"

MASTER_DIR="$BASE_DIR/master"
SLAVE_DIR="$BASE_DIR/slave"
MASTER_LOG="$MASTER_DIR/master.log"
SLAVE_LOG="$SLAVE_DIR/slave.log"
MASTER_DUMP="$MASTER_DIR/master.dump"
MASTER_AOF="$MASTER_DIR/master.aof"
SLAVE_DUMP="$SLAVE_DIR/slave.dump"
SLAVE_AOF="$SLAVE_DIR/slave.aof"

MASTER_PID=""
SLAVE_PID=""

red()   { printf '\033[31m%s\033[0m\n' "$*"; }
green() { printf '\033[32m%s\033[0m\n' "$*"; }
yellow(){ printf '\033[33m%s\033[0m\n' "$*"; }
blue()  { printf '\033[34m%s\033[0m\n' "$*"; }

need_cmd() {
  command -v "$1" >/dev/null 2>&1 || { red "missing command: $1"; exit 1; }
}

cleanup() {
  set +e
  if [[ -n "$SLAVE_PID" ]] && kill -0 "$SLAVE_PID" 2>/dev/null; then
    kill "$SLAVE_PID" 2>/dev/null || true
    wait "$SLAVE_PID" 2>/dev/null || true
  fi
  if [[ -n "$MASTER_PID" ]] && kill -0 "$MASTER_PID" 2>/dev/null; then
    kill "$MASTER_PID" 2>/dev/null || true
    wait "$MASTER_PID" 2>/dev/null || true
  fi
}
trap cleanup EXIT

run_cmd() {
  local host="$1"
  local port="$2"
  local cmd="$3"
  if command -v "$TIMEOUT_BIN" >/dev/null 2>&1; then
    printf '%s\r\n' "$cmd" | "$TIMEOUT_BIN" 2 "$NC_BIN" "$host" "$port" 2>/dev/null || true
  else
    printf '%s\r\n' "$cmd" | "$NC_BIN" "$host" "$port" 2>/dev/null || true
  fi
}

send_expect_ok() {
  local port="$1"
  local cmd="$2"
  local out
  out="$(run_cmd 127.0.0.1 "$port" "$cmd")"
  if [[ "$out" != "+OK"$'\r\n' && "$out" != ":1"$'\r\n' ]]; then
    red "unexpected response on port $port for command: $cmd"
    printf '%s\n' "$out"
    exit 1
  fi
}

cmd_get_value() {
  local port="$1"
  local cmd="$2"
  local out len value
  out="$(run_cmd 127.0.0.1 "$port" "$cmd")"
  out="${out//$'\r'/}"
  if [[ "$out" == '$-1'$'\n' ]]; then
    printf ''
    return 0
  fi
  if [[ "$out" =~ ^\$([0-9]+)$'\n'(.*)$'\n'$ ]]; then
    len="${BASH_REMATCH[1]}"
    value="${BASH_REMATCH[2]}"
    printf '%s' "$value"
    return 0
  fi
  printf '%s' "$out"
}

resp_info() {
  local port="$1"
  run_cmd 127.0.0.1 "$port" 'INFO'
}

resp_ping() {
  local port="$1"
  run_cmd 127.0.0.1 "$port" 'PING'
}

wait_port() {
  local port="$1"
  local retries="${2:-100}"
  local i
  for ((i=0; i<retries; i++)); do
    if [[ "$(resp_ping "$port")" == "+PONG"$'\r\n' ]]; then
      return 0
    fi
    sleep 0.1
  done
  return 1
}

assert_eq() {
  local label="$1"
  local expected="$2"
  local got="$3"
  if [[ "$expected" != "$got" ]]; then
    red "[FAIL] $label expected='$expected' got='$got'"
    return 1
  fi
  green "[OK] $label => $got"
  return 0
}

sample_compare_string_key() {
  local key="$1"
  local vm vs
  vm="$(cmd_get_value "$MASTER_PORT" "GET $key")"
  vs="$(cmd_get_value "$SLAVE_PORT" "GET $key")"
  assert_eq "GET $key" "$vm" "$vs"
}

sample_compare_hash_key() {
  local key="$1"
  local vm vs
  vm="$(cmd_get_value "$MASTER_PORT" "HGET $key")"
  vs="$(cmd_get_value "$SLAVE_PORT" "HGET $key")"
  assert_eq "HGET $key" "$vm" "$vs"
}

sample_compare_rbtree_key() {
  local key="$1"
  local vm vs
  vm="$(cmd_get_value "$MASTER_PORT" "RGET $key")"
  vs="$(cmd_get_value "$SLAVE_PORT" "RGET $key")"
  assert_eq "RGET $key" "$vm" "$vs"
}

sample_compare_skip_key() {
  local key="$1"
  local vm vs
  vm="$(cmd_get_value "$MASTER_PORT" "XGET $key")"
  vs="$(cmd_get_value "$SLAVE_PORT" "XGET $key")"
  assert_eq "XGET $key" "$vm" "$vs"
}

if [[ ! -x "$BIN" ]]; then
  yellow "binary not found or not executable: $BIN"
  if [[ -n "$BUILD_CMD" ]]; then
    blue "running build command: $BUILD_CMD"
    bash -lc "$BUILD_CMD"
  fi
fi

[[ -x "$BIN" ]] || { red "kvstore binary not found: $BIN"; exit 1; }
need_cmd "$NC_BIN"

rm -rf "$BASE_DIR"
mkdir -p "$MASTER_DIR" "$SLAVE_DIR"

blue "starting master on :$MASTER_PORT"
"$BIN" \
  --port "$MASTER_PORT" \
  --role master \
  --dump "$MASTER_DUMP" \
  --aof "$MASTER_AOF" \
  >"$MASTER_LOG" 2>&1 &
MASTER_PID=$!

wait_port "$MASTER_PORT" || { red "master did not become ready"; exit 1; }
sleep "$STARTUP_WAIT"
green "master ready pid=$MASTER_PID"

blue "preloading $PRELOAD_COUNT keys into master"
for i in $(seq 1 "$PRELOAD_COUNT"); do
  key=$(printf 'preload:%05d' "$i")
  val=$(printf 'value:%05d' "$i")
  send_expect_ok "$MASTER_PORT" "SET $key $val"
done
send_expect_ok "$MASTER_PORT" 'HSET h:base hv:base'
send_expect_ok "$MASTER_PORT" 'RSET r:base rv:base'
send_expect_ok "$MASTER_PORT" 'XSET t:base tv:base'
send_expect_ok "$MASTER_PORT" 'EXPIRE preload:00001 300'
send_expect_ok "$MASTER_PORT" 'EXPIRE preload:00002 300'
send_expect_ok "$MASTER_PORT" 'HEXPIRE h:base 300'
send_expect_ok "$MASTER_PORT" 'REXPIRE r:base 300'
send_expect_ok "$MASTER_PORT" 'XEXPIRE t:base 300'

green "preload done"
blue "master INFO before slave start:"
resp_info "$MASTER_PORT" || true

blue "starting slave on :$SLAVE_PORT"
"$BIN" \
  --port "$SLAVE_PORT" \
  --role slave \
  --master-host 127.0.0.1 \
  --master-port "$MASTER_PORT" \
  --dump "$SLAVE_DUMP" \
  --aof "$SLAVE_AOF" \
  >"$SLAVE_LOG" 2>&1 &
SLAVE_PID=$!

wait_port "$SLAVE_PORT" || { red "slave did not become ready"; exit 1; }
sleep "$STARTUP_WAIT"
green "slave ready pid=$SLAVE_PID"

blue "injecting writes during full sync after ${INJECT_DELAY}s"
sleep "$INJECT_DELAY"
for i in $(seq 1 "$DURING_SYNC_WRITES"); do
  key=$(printf 'after_sync:%05d' "$i")
  val=$(printf 'v:%05d' "$i")
  send_expect_ok "$MASTER_PORT" "SET $key $val"
done
send_expect_ok "$MASTER_PORT" 'HSET after_h hv_after'
send_expect_ok "$MASTER_PORT" 'RSET after_r rv_after'
send_expect_ok "$MASTER_PORT" 'XSET after_t tv_after'
send_expect_ok "$MASTER_PORT" 'SET special:key during-sync'

green "during-sync writes injected"
blue "waiting $SYNC_WAIT seconds for full sync + backlog replay"
sleep "$SYNC_WAIT"

blue "master INFO after sync window:"
resp_info "$MASTER_PORT" || true
blue "slave INFO after sync window:"
resp_info "$SLAVE_PORT" || true

blue "verifying slave read-only"
slave_set_out="$(run_cmd 127.0.0.1 "$SLAVE_PORT" 'SET should_fail x')"
echo "$slave_set_out"
if echo "$slave_set_out" | grep -qi 'read only slave'; then
  green "[OK] slave is read-only"
else
  yellow "[WARN] slave read-only message not observed exactly; inspect logs"
fi

blue "sampling master/slave key equality"
fail=0
sample_compare_string_key preload:00001 || fail=1
sample_compare_string_key preload:00010 || fail=1
sample_compare_string_key preload:00100 || fail=1
sample_compare_string_key preload:01000 || fail=1
sample_compare_string_key preload:$(printf '%05d' "$PRELOAD_COUNT") || fail=1
sample_compare_string_key after_sync:00001 || fail=1
sample_compare_string_key after_sync:00010 || fail=1
sample_compare_string_key after_sync:$(printf '%05d' "$DURING_SYNC_WRITES") || fail=1
sample_compare_string_key special:key || fail=1
sample_compare_hash_key h:base || fail=1
sample_compare_hash_key after_h || fail=1
sample_compare_rbtree_key r:base || fail=1
sample_compare_rbtree_key after_r || fail=1
sample_compare_skip_key t:base || fail=1
sample_compare_skip_key after_t || fail=1

blue "checking TTL presence on a sample key"
master_ttl="$(run_cmd 127.0.0.1 "$MASTER_PORT" 'TTL preload:00001' | perl -0pe 's/\r//g')"
slave_ttl="$(run_cmd 127.0.0.1 "$SLAVE_PORT" 'TTL preload:00001' | perl -0pe 's/\r//g')"
echo "master TTL preload:00001 => $master_ttl"
echo "slave  TTL preload:00001 => $slave_ttl"

blue "showing recent master log lines"
tail -n 40 "$MASTER_LOG" || true
blue "showing recent slave log lines"
tail -n 40 "$SLAVE_LOG" || true

if [[ "$fail" -ne 0 ]]; then
  red "replication check failed"
  exit 1
fi

green "replication check passed"

blue "artifacts saved under: $BASE_DIR"
echo "master log: $MASTER_LOG"
echo "slave  log: $SLAVE_LOG"
echo "master dump: $MASTER_DUMP"
echo "slave  dump: $SLAVE_DUMP"

if [[ "$fail" -ne 0 ]]; then
  red "REPLICATION TEST FAILED"
  exit 1
fi

green "REPLICATION TEST PASSED"
