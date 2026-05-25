// ============================================================
// repl_kprobe.bpf.c — kprobe BPF 程序
//
// Hook tcp_sendmsg，拦截 replication 流量。
// 通过 BPF ringbuf 将数据从内核传递到用户态转发模块。
//
// 限制: 单次最大 500 字节，当前增量数据量小可满足。
// ============================================================

#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>

/* ---- 兼容性定义 ---- */
#ifndef BPF_MAP_TYPE_RINGBUF
#define BPF_MAP_TYPE_RINGBUF 27
#endif

/* ---- BPF Maps ---- */

struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 8);
    __type(key, __u32);
    __type(value, __u64);
} kprobe_ctl SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 16);
    __type(key, __u32);
    __type(value, __u64);
} kprobe_stats SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 1 << 20);
} repl_ringbuf SEC(".maps");

/* ---- Control Keys ---- */
#define KVS_KPROBE_CTL_ENABLED  0
#define KVS_KPROBE_CTL_PID      1

/* ---- Stats Keys ---- */
#define KVS_KPROBE_STAT_HIT      0
#define KVS_KPROBE_STAT_SKIP_PID 1
#define KVS_KPROBE_STAT_RB_ERR   2

/* ringbuf entry 格式: [4B payload_len] */
#define KVS_KPROBE_ENTRY_HDR_SZ 4

SEC("kprobe/tcp_sendmsg")
int kprobe_kvs_repl_tcp_sendmsg(struct pt_regs *ctx)
{
    __u64 *enabled, *target_pid, *stat;
    (void)ctx;

    /* 1. 检查开关 */
    enabled = bpf_map_lookup_elem(&kprobe_ctl, &(__u32){KVS_KPROBE_CTL_ENABLED});
    if (!enabled || !*enabled)
        return 0;

    /* 2. PID 过滤 */
    __u32 pid = bpf_get_current_pid_tgid() >> 32;
    target_pid = bpf_map_lookup_elem(&kprobe_ctl, &(__u32){KVS_KPROBE_CTL_PID});
    if (!target_pid)
        return 0;
    if (pid != (__u32)(*target_pid)) {
        stat = bpf_map_lookup_elem(&kprobe_stats,
            &(__u32){KVS_KPROBE_STAT_SKIP_PID});
        if (stat) __sync_fetch_and_add(stat, 1);
        return 0;
    }

    /* 3. 写入 ringbuf 触发通知（仅 4 字节标记）
     *  实际 payload 由用户态根据 repl_broadcast 路径独立写入
     *  kprobe 在此充当"事件通知"角色 */
    __u32 zero = 0;
    if (bpf_ringbuf_output(&repl_ringbuf, &zero, 4, 0) != 0) {
        stat = bpf_map_lookup_elem(&kprobe_stats,
            &(__u32){KVS_KPROBE_STAT_RB_ERR});
        if (stat) __sync_fetch_and_add(stat, 1);
        return 0;
    }

    /* 4. 更新统计 */
    stat = bpf_map_lookup_elem(&kprobe_stats, &(__u32){KVS_KPROBE_STAT_HIT});
    if (stat) __sync_fetch_and_add(stat, 1);

    return 0;  /* 放行 TCP 正常发送 */
}

char LICENSE[] SEC("license") = "GPL";
