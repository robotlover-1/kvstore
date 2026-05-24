/* SPDX-License-Identifier: GPL-2.0 */
/*
 * repl_realtime_capture.bpf.c — eBPF kprobe/tracepoint program for RDMA capture
 *
 * 功能: 通过 kprobe 拦截 __sys_sendto，捕获指定 fd 上的数据，
 *       写入 BPF ring buffer，通过 bpf_override_return 阻止原始 TCP 发送。
 *
 * 数据流:
 *   kvstore send(fd_slave, data)
 *     → kprobe/__sys_sendto 触发
 *     → 匹配 fd → bpf_probe_read_user 拷贝数据到 ring buffer
 *     → bpf_override_return(-EPERM) 阻止 TCP
 *     → 用户态: ring buffer callback → RDMA pipeline → Slave
 *
 * 内核要求: CONFIG_BPF_KPROBE_OVERRIDE=y, Linux >= 5.8
 */

#include <linux/bpf.h>
#include <linux/types.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

/* 兼容老版本头文件 */
#ifndef BPF_MAP_TYPE_RINGBUF
#define BPF_MAP_TYPE_RINGBUF 27
#endif
#ifndef EPERM
#define EPERM 1
#endif

/* ---- 统计常量 ---- */
#define KVS_CAPTURE_STAT_COUNT     0
#define KVS_CAPTURE_STAT_BYTES     1
#define KVS_CAPTURE_STAT_BLOCKED   2
#define KVS_CAPTURE_STAT_SKIP      3
#define KVS_CAPTURE_STAT_RB_FAIL   4

/* ---- 控制 map 常量 ---- */
#define KVS_CAPTURE_CTL_ENABLED    0
#define KVS_CAPTURE_CTL_TARGET_FD  1
#define KVS_CAPTURE_CTL_MAX_ENTRIES 2

/* ---- Ring buffer 事件格式 ---- */
struct capture_event {
    __u64 len;
    __u64 flags;
    unsigned char data[];
};

/* ---- BPF Maps ---- */

/* Ring buffer: BPF 程序 → 用户态消费者线程 */
struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 1 << 20);    /* 1MB */
} capture_ringbuf SEC(".maps");

/* 控制 map: 用户态可写，BPF 只读 */
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, KVS_CAPTURE_CTL_MAX_ENTRIES);
    __type(key, __u32);
    __type(value, __u64);
} capture_ctl SEC(".maps");

/* 统计 map */
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 5);
    __type(key, __u32);
    __type(value, __u64);
} capture_stats SEC(".maps");

/* ---- 辅助函数 ---- */

static __always_inline void stat_inc(__u32 key) {
    __u64 *v = bpf_map_lookup_elem(&capture_stats, &key);
    if (v) __sync_fetch_and_add(v, 1);
}

static __always_inline void stat_add(__u32 key, __u64 delta) {
    __u64 *v = bpf_map_lookup_elem(&capture_stats, &key);
    if (v) __sync_fetch_and_add(v, delta);
}

static __always_inline __u64 ctl_get(__u32 key) {
    __u64 *v = bpf_map_lookup_elem(&capture_ctl, &key);
    return v ? *v : 0;
}

/*
 * kprobe/__sys_sendto — 捕获指定 fd 上的系统调用
 *
 * __sys_sendto(int fd, void __user *buff, size_t len, ...)
 * x86_64 ABI: rdi=fd, rsi=buf, rdx=len
 *
 * 使用 bpf_override_return 阻止原始 TCP 发送，
 * 数据通过 ring buffer 传递给用户态消费者线程。
 */
SEC("kprobe/__sys_sendto")
int kp_capture_sendto(struct pt_regs *ctx) {
    __u64 enabled = ctl_get(KVS_CAPTURE_CTL_ENABLED);
    if (!enabled) return 0;

    __u64 target_fd = ctl_get(KVS_CAPTURE_CTL_TARGET_FD);
    if (!target_fd) return 0;

    /* 直接从 pt_regs 读取寄存器 (x86_64 pt_regs 布局):
     *   offset 112: di (fd)
     *   offset 104: si (buf)
     *   offset  96: dx (len)
     * 使用 bpf_probe_read_kernel 安全读取以避免 verifier 问题 */
    __u64 fd_val = 0, buf_val = 0, len_val = 0;
    bpf_probe_read_kernel(&fd_val, sizeof(fd_val), (const void *)((unsigned long)ctx + 112));
    bpf_probe_read_kernel(&buf_val, sizeof(buf_val), (const void *)((unsigned long)ctx + 104));
    bpf_probe_read_kernel(&len_val, sizeof(len_val), (const void *)((unsigned long)ctx + 96));

    if (fd_val != target_fd) {
        stat_inc(KVS_CAPTURE_STAT_SKIP);
        return 0;
    }

    if (len_val == 0 || len_val > 65536) return 0;

    /* 预留 ring buffer 空间（变长） */
    struct capture_event *e = bpf_ringbuf_reserve(&capture_ringbuf,
        sizeof(struct capture_event) + len_val, 0);
    if (!e) {
        stat_inc(KVS_CAPTURE_STAT_RB_FAIL);
        return 0;
    }

    /* 拷贝用户态数据到 ring buffer */
    e->len = len_val;
    e->flags = 0;
    bpf_probe_read_user(e->data, len_val, (void *)(unsigned long)buf_val);

    bpf_ringbuf_submit(e, 0);

    stat_inc(KVS_CAPTURE_STAT_COUNT);
    stat_add(KVS_CAPTURE_STAT_BYTES, len_val);

    /* 阻止原始系统调用，返回 -EPERM */
    bpf_override_return(ctx, -EPERM);
    stat_inc(KVS_CAPTURE_STAT_BLOCKED);

    return 0;
}

char LICENSE[] SEC("license") = "GPL";
