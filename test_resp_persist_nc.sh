#!/usr/bin/env bash
set -euo pipefail
HOST="${1:-127.0.0.1}"
PORT="${2:-5000}"

run() {
  printf '%b' "$1" | nc -q 1 "$HOST" "$PORT"
}

echo '[1] set/get'
run '*3\r\n$3\r\nSET\r\n$4\r\nname\r\n$5\r\nalice\r\n'
run '*2\r\n$3\r\nGET\r\n$4\r\nname\r\n'

echo '[2] ttl'
run '*3\r\n$6\r\nEXPIRE\r\n$4\r\nname\r\n$1\r\n5\r\n'
run '*2\r\n$3\r\nTTL\r\n$4\r\nname\r\n'

echo '[3] pipeline'
run '*3\r\n$3\r\nSET\r\n$4\r\npkey\r\n$4\r\npval\r\n*2\r\n$3\r\nGET\r\n$4\r\npkey\r\n'

echo '[4] save'
run '*1\r\n$4\r\nSAVE\r\n'
ls -l kvstore.dump kvstore.aof || true
