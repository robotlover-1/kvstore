#!/bin/bash
# cross_machine_bench.sh — eBPF kprobe 跨机 QPS 对比（slave 在远端）
set -e

SLAVE_HOST="192.168.233.129"
SLAVE_PORT=15901
SSHPASS="2983372202"
PAYLOAD=64
COUNT=5000
ROUNDS=3
TEST_BIN="./tests/perf/test_ebpf_proxy_qps"
SLAVE_BIN="/home/pp/slave_receiver"
BPF_PIN="/sys/fs/bpf/kvstore_repl_qps_test"

rssh() { sshpass -p "$SSHPASS" ssh -o StrictHostKeyChecking=no "pp@$SLAVE_HOST" "$@"; }

echo "=== Cross-Machine eBPF QPS (slave @ $SLAVE_HOST) ==="
echo "payload=$PAYLOAD count=$COUNT rounds=$ROUNDS"
echo ""

NONE_RES=""; SYNC_RES=""; EBPF_RES=""

for round in $(seq 1 $ROUNDS); do
  echo "=== Round $round ==="

  for mode in none sync ebpf; do
    # Start slave on remote
    rssh "pkill -9 slave_receiver 2>/dev/null; sleep 0.2; nohup $SLAVE_BIN $SLAVE_PORT > /tmp/slave.txt 2>&1 &"
    sleep 0.8

    # Clean local
    sudo pkill -9 -f "ebpf_proxy" 2>/dev/null || true
    sudo rm -rf "$BPF_PIN" 2>/dev/null
    sleep 0.3

    # Run test
    output=$(echo "$SSHPASS" | sudo -S $TEST_BIN --mode $mode \
      --payload $PAYLOAD --count $COUNT \
      --slave-host $SLAVE_HOST --no-local-slave 2>&1)
    qps=$(echo "$output" | grep "^$mode " | awk '{print $3}')

    # Get slave stats from remote
    rssh "pkill -INT slave_receiver 2>/dev/null" || true
    sleep 0.5
    sstat=$(rssh "cat /tmp/slave.txt 2>/dev/null | grep msgs= || echo 'msgs=0 bytes=0'")

    echo "  $mode qps=$qps  slave=[$sstat]"

    case $mode in
      none) NONE_RES="$NONE_RES $qps" ;;
      sync) SYNC_RES="$SYNC_RES $qps" ;;
      ebpf) EBPF_RES="$EBPF_RES $qps" ;;
    esac
    sleep 1
  done
  echo ""
done

# Cleanup
rssh "pkill -9 slave_receiver 2>/dev/null" || true
sudo pkill -9 -f "ebpf_proxy" 2>/dev/null || true
sudo rm -rf "$BPF_PIN" 2>/dev/null

# Compute median
median() {
  echo "$1" | tr ' ' '\n' | sort -n | awk '{a[NR]=$1} END{mid=int(NR/2)+1; print a[mid]}'
}

n=$(median "$NONE_RES"); s=$(median "$SYNC_RES"); e=$(median "$EBPF_RES")
echo "=== Median Results ==="
echo "none=$n  sync=$s  ebpf=$e"
if [ "$s" != "0" ] && [ -n "$s" ]; then
  delta=$(echo "scale=1; ($e - $s) * 100 / $s" | bc)
  echo "ebpf vs sync: ${delta}%"
fi
echo "Done."
