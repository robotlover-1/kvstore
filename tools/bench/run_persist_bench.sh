#!/usr/bin/env bash
# 持久化性能基准测试
set -euo pipefail

PROJ_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="$PROJ_DIR/kvstore"
OUTDIR="$PROJ_DIR/benchmarks/data/persist_bench"
mkdir -p "$OUTDIR"

KVSTORE_PORT=5190
REDIS_PORT=6390
TMPDIR="/tmp/kvstore_persist_bench"
mkdir -p "$TMPDIR"

echo "============================================"
echo " 持久化性能基准测试"
echo " 日期: $(date)"
echo " Host: $(hostname) CPU: $(nproc) cores"
echo "============================================"

cleanup_all() {
    pkill -f "kvstore.*--port $KVSTORE_PORT" 2>/dev/null || true
    pkill -f "redis-server.*$REDIS_PORT" 2>/dev/null || true
    sleep 1
}
trap cleanup_all EXIT

wait_port() {
    local port=$1
    for i in $(seq 1 60); do
        if redis-cli -p "$port" PING >/dev/null 2>&1; then return 0; fi
        sleep 0.2
    done
    return 1
}

start_kvstore() {
    local extra="$1"
    cleanup_all
    rm -f kvstore.dump kvstore.aof
    $BIN --port $KVSTORE_PORT --role master --mem libc --net reactor $extra \
        > "$TMPDIR/kvstore.log" 2>&1 &
    wait_port $KVSTORE_PORT || { echo "FAIL: kvstore"; cat "$TMPDIR/kvstore.log"; return 1; }
    sleep 1
}

start_redis() {
    local extra="$1"
    cleanup_all
    rm -f "$TMPDIR/dump.rdb" "$TMPDIR/appendonly.aof"
    redis-server --port $REDIS_PORT --dir "$TMPDIR" --save "" $extra \
        > "$TMPDIR/redis.log" 2>&1 &
    wait_port $REDIS_PORT || { echo "FAIL: redis"; cat "$TMPDIR/redis.log"; return 1; }
    sleep 2
}

run_bench() {
    local label="$1" port="$2"
    local outfile="$OUTDIR/aof_${label}.txt"
    redis-benchmark -p "$port" -t set -n 100000 -c 50 -d 64 --csv > "$outfile" 2>&1 || true
    local qps=$(grep 'SET' "$outfile" | head -1 | cut -d',' -f2 | tr -d '"' || echo "N/A")
    echo "  $label QPS: $qps"
    echo "$label,$qps" >> "$OUTDIR/aof_summary.csv"
}

# ==================== 测试1: AOF 对比 ====================
echo ""
echo "============================================"
echo " 测试1: AOF 性能对比"
echo "============================================"
echo "label,qps" > "$OUTDIR/aof_summary.csv"

echo "--- kvstore_aof_always ---"
start_kvstore "--appendfsync always" || exit 1
run_bench "kvstore_aof_always" $KVSTORE_PORT

echo "--- kvstore_aof_everysec ---"
start_kvstore "--appendfsync everysec" || exit 1
run_bench "kvstore_aof_everysec" $KVSTORE_PORT

echo "--- redis_no_aof ---"
start_redis "" || exit 1
run_bench "redis_no_aof" $REDIS_PORT

echo "--- redis_aof_everysec ---"
start_redis "--appendonly yes --appendfsync everysec" || exit 1
run_bench "redis_aof_everysec" $REDIS_PORT

echo "--- redis_aof_always ---"
start_redis "--appendonly yes --appendfsync always" || exit 1
run_bench "redis_aof_always" $REDIS_PORT

echo ""
echo "=== AOF 对比结果 ==="
cat "$OUTDIR/aof_summary.csv"

# ==================== 测试2: SAVE 性能 ====================
echo ""
echo "============================================"
echo " 测试2: SAVE 性能 (共 100w key)"
echo "============================================"
echo "scenario,total_keys,total_time_sec,save_count,write_time_sec,save_time_sec,avg_save_ms" > "$OUTDIR/save_summary.csv"

gen_batch_resp() {
    local out="$1" start="$2" end="$3"
    > "$out"
    for i in $(seq "$start" "$end"); do
        printf '*3\r\n$3\r\nSET\r\n$15\r\nsavebench:k:%07d\r\n$9\r\nv:%07d\r\n' "$i" "$i" >> "$out"
    done
}

run_save_test() { local TOTAL=1000000
    local label="$1" batch="$2"
    local iter=$((TOTAL / batch))
    echo ""
    echo "--- $label (batch=$batch, iter=$iter) ---"
    start_kvstore "--appendfsync everysec" || return
    local write_ms=0 save_ms=0 key=0
    for i in $(seq 1 $iter); do
        local ks=$((key + 1)) ke=$((key + batch))
        local respf="$TMPDIR/resp_${label}_${i}.txt"
        gen_batch_resp "$respf" $ks $ke
        local t0=$(date +%s%N)
        redis-cli -p $KVSTORE_PORT --pipe < "$respf" > /dev/null 2>&1 || true
        local t1=$(date +%s%N)
        local wd=$(( (t1 - t0) / 1000000 ))
        write_ms=$((write_ms + wd))
        local ts0=$(date +%s%N)
        redis-cli -p $KVSTORE_PORT SAVE > /dev/null 2>&1 || true
        local ts1=$(date +%s%N)
        local sd=$(( (ts1 - ts0) / 1000000 ))
        save_ms=$((save_ms + sd))
        key=$ke
        rm -f "$respf"
        if [ $((i % 10)) -eq 0 ] || [ $i -eq $iter ] || [ $i -eq 1 ]; then
            printf "  %4d/%d keys=%d write=%dms save=%dms avg_save=%dms\n" \
                "$i" "$iter" "$key" "$wd" "$sd" "$((save_ms / i))"
        fi
    done
    local total_ms=$((write_ms + save_ms))
    echo "$label,$TOTAL,$(echo "scale=2;$total_ms/1000"|bc),$iter,$(echo "scale=2;$write_ms/1000"|bc),$(echo "scale=2;$save_ms/1000"|bc),$(echo "scale=1;$save_ms/$iter"|bc)" >> "$OUTDIR/save_summary.csv"
    echo "  结果: 总$(echo "scale=2;$total_ms/1000"|bc)s 写入$(echo "scale=2;$write_ms/1000"|bc)s SAVE$(echo "scale=2;$save_ms/1000"|bc)s 平均$(echo "scale=1;$save_ms/$iter"|bc)ms"
}

run_save_test "100w_save_1"  1000000
run_save_test "10w_save_10"  100000
run_save_test "1w_save_100"  10000
run_save_test "1k_save_1000" 1000

echo ""
echo "=== SAVE 性能结果 ==="
cat "$OUTDIR/save_summary.csv"
echo ""
echo "完成! 结果保存在 $OUTDIR/"
