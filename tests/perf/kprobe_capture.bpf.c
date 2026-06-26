/*
 * kprobe_capture.bpf.c — kprobe on tcp_recvmsg 截获接收数据到 ringbuf
 *
 * 用法：用户态加载后，tcp_recvmsg 返回的数据自动进 ringbuf。
 * 通过 PID + 端口过滤，只截获 echo server 连接上接收的数据。
 *
 * x86_64 调用约定 (tcp_recvmsg):
 *   struct sock *sk   = di (PT_REGS_PARM1)
 *   struct msghdr *msg = si (PT_REGS_PARM2)
 *   size_t len         = dx (PT_REGS_PARM3)
 *   返回值 (ax)         = 实际接收字节数
 */
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_core_read.h>
#include <bpf/bpf_tracing.h>

#ifndef BPF_MAP_TYPE_RINGBUF
#define BPF_MAP_TYPE_RINGBUF 27
#endif

#define CAPTURE_MAX_DATA 2048
#define STAT_HIT         0
#define STAT_SKIP_PORT   1
#define STAT_SKIP_PID    2
#define STAT_RB_ERR      3
#define STAT_BYTES       4
#define CTL_ENABLED      0
#define CTL_PID          1
#define CTL_PORT         2

/* 控制 map */
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 8);
    __type(key, __u32);
    __type(value, __u64);
} ctl SEC(".maps");

/* 统计 map */
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 8);
    __type(key, __u32);
    __type(value, __u64);
} stats SEC(".maps");

/* ringbuf */
struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 1 << 22); /* 4MB */
} ringbuf SEC(".maps");

/* 临时缓冲区 */
struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(max_entries, 2);
    __type(key, __u32);
    __type(value, unsigned char[4 + CAPTURE_MAX_DATA]);
} tmpbuf SEC(".maps");

/* 用于 kprobe→kretprobe 传递 msg 指针 */
struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, unsigned long);
} entry_msg SEC(".maps");

static __always_inline __u64 ctl_get(__u32 key) {
    __u64 *v = bpf_map_lookup_elem(&ctl, &key);
    return v ? *v : 0;
}

static __always_inline void stat_inc(__u32 key) {
    __u64 *v = bpf_map_lookup_elem(&stats, &key);
    if (v) __sync_fetch_and_add(v, 1);
}

static __always_inline void stat_add(__u32 key, __u64 delta) {
    __u64 *v = bpf_map_lookup_elem(&stats, &key);
    if (v) __sync_fetch_and_add(v, delta);
}

/* 从 msghdr→iov_iter 读取 iov 数据和 nr_segs（合并为一次 bpf_probe_read_kernel）
 * iov_iter 在 msghdr 内偏移 16 字节 (msg_name 8 + msg_namelen 4 + pad 4)
 * iov 指针在 iov_iter 内偏移 24 字节 (iter_type 1 + nofault 1 + data_source 1 + pad 5 + iov_offset 8 + count 8)
 * nr_segs 紧接在 iov 之后，偏移 32 字节
 * iov 和 nr_segs 在 msg+40 处相邻 16 字节，可一次读出 */
static __always_inline int read_iov_data(unsigned long msg_ptr,
    unsigned char *buf, int max_len)
{
    /* 1. 合并读取 iov 指针 + nr_segs（16 字节从 msg_ptr + 40） */
    struct { unsigned long iov_ptr; unsigned long _nr_segs; } head;
    if (bpf_probe_read_kernel(&head, sizeof(head),
            (const void *)(msg_ptr + 40)) != 0)
        return 0;
    if (!head.iov_ptr || head._nr_segs == 0) return 0;

    /* 2. 读取第一个 iovec */
    struct { unsigned long base; unsigned long len; } vec;
    if (bpf_probe_read_kernel(&vec, sizeof(vec),
            (const void *)head.iov_ptr) != 0)
        return 0;
    if (!vec.base || vec.len == 0) return 0;

    /* 3. 限制大小 */
    unsigned long long safe_len = vec.len;
    if (safe_len > (unsigned long long)max_len)
        safe_len = (unsigned long long)max_len;

    /* 4. 从用户空间读取实际数据 */
    if (bpf_probe_read_user(buf, (__u32)safe_len,
            (const void *)(unsigned long)vec.base) != 0)
        return 0;
    return (int)safe_len;
}

/* ──── fentry: 在 tcp_recvmsg 入口保存 msg 指针 ──── */
SEC("fentry/tcp_recvmsg")
int BPF_PROG(fentry_recv, struct sock *sk, struct msghdr *msg, size_t len)
{
    __u64 enabled = ctl_get(CTL_ENABLED);
    if (!enabled) return 0;

    __u64 target_pid = ctl_get(CTL_PID);
    if (!target_pid) return 0;

    __u32 pid = bpf_get_current_pid_tgid() >> 32;
    if (pid != (__u32)target_pid) {
        stat_inc(STAT_SKIP_PID);
        return 0;
    }

    /* 保存 msg 指针（供 fexit 使用） */
    unsigned long msg_val = (unsigned long)msg;
    __u32 key = 0;
    bpf_map_update_elem(&entry_msg, &key, &msg_val, 0);

    return 0;
}

/* ──── fexit: 在 tcp_recvmsg 返回时读取数据写 ringbuf ──── */
SEC("fexit/tcp_recvmsg")
int BPF_PROG(fexit_recv, struct sock *sk, struct msghdr *msg, size_t len)
{
    __u64 enabled = ctl_get(CTL_ENABLED);
    if (!enabled) return 0;

    /* fexit: ctx[0] = 返回值 (通过 ctx 原始参数读取, 避免 BTF 类型冲突) */
    long retval = (long)ctx[0];
    if (retval <= 0) return 0;

    __u32 key = 0;
    unsigned long *msg_ptr = bpf_map_lookup_elem(&entry_msg, &key);
    if (!msg_ptr || *msg_ptr == 0) return 0;

    /* 用 tmpbuf 做临时缓冲（BPF verifier 要求 map 指针作为 probe_read_user dst） */
    unsigned char *buf = bpf_map_lookup_elem(&tmpbuf, &key);
    if (!buf) return 0;

    /* 头 4 字节 = payload 长度 */
    __u32 plen = 0;
    __builtin_memcpy(buf, &plen, 4);

    int data_len = read_iov_data(*msg_ptr, buf + 4, CAPTURE_MAX_DATA);
    if (data_len <= 0) return 0;

    plen = (__u32)data_len;
    __builtin_memcpy(buf, &plen, 4);

    /* reserve → memcpy → submit（替代 bpf_ringbuf_output）
     * BPF verifier 要求 __builtin_memcpy 的 size 为常量，统一 reserve+copy 固定大小 */
    void *entry = bpf_ringbuf_reserve(&ringbuf, 4 + CAPTURE_MAX_DATA, 0);
    if (!entry) {
        stat_inc(STAT_RB_ERR);
        return 0;
    }
    bpf_probe_read_kernel(entry, 4 + CAPTURE_MAX_DATA, buf);
    bpf_ringbuf_submit(entry, 0);

    stat_inc(STAT_HIT);
    stat_add(STAT_BYTES, (__u64)data_len);

    /* 清除保存的 msg 指针 */
    unsigned long zero = 0;
    bpf_map_update_elem(&entry_msg, &key, &zero, 0);

    return 0;
}

/* ──── 旧 kprobe 版本保留作为 fallback ──── */
SEC("kprobe/tcp_recvmsg")
int kp_recv_entry(struct pt_regs *ctx)
{
    __u64 enabled = ctl_get(CTL_ENABLED);
    if (!enabled) return 0;

    __u64 target_pid = ctl_get(CTL_PID);
    if (!target_pid) return 0;

    __u32 pid = bpf_get_current_pid_tgid() >> 32;
    if (pid != (__u32)target_pid) {
        stat_inc(STAT_SKIP_PID);
        return 0;
    }

    /* 保存 msg 指针 */
    unsigned long msg_ptr = (unsigned long)ctx->si;
    __u32 key = 0;
    bpf_map_update_elem(&entry_msg, &key, &msg_ptr, 0);

    return 0;
}

/* ──── 旧 kretprobe 版本保留作为 fallback ──── */
SEC("kretprobe/tcp_recvmsg")
int kp_recv_return(struct pt_regs *ctx)
{
    __u64 enabled = ctl_get(CTL_ENABLED);
    if (!enabled) return 0;

    long retval = (long)ctx->ax;
    if (retval <= 0) return 0;

    __u32 key = 0;
    unsigned long *msg_ptr = bpf_map_lookup_elem(&entry_msg, &key);
    if (!msg_ptr || *msg_ptr == 0) return 0;

    /* 用 tmpbuf 做临时缓冲（BPF verifier 要求 map 指针作为 probe_read_user dst） */
    unsigned char *buf = bpf_map_lookup_elem(&tmpbuf, &key);
    if (!buf) return 0;

    /* 头 4 字节 = payload 长度 */
    __u32 plen = 0;
    __builtin_memcpy(buf, &plen, 4);

    int data_len = read_iov_data(*msg_ptr, buf + 4, CAPTURE_MAX_DATA);
    if (data_len <= 0) return 0;

    plen = (__u32)data_len;
    __builtin_memcpy(buf, &plen, 4);

    /* reserve → memcpy → submit（替代 bpf_ringbuf_output）
     * BPF verifier 要求 __builtin_memcpy 的 size 为常量，统一 reserve+copy 固定大小 */
    void *entry = bpf_ringbuf_reserve(&ringbuf, 4 + CAPTURE_MAX_DATA, 0);
    if (!entry) {
        stat_inc(STAT_RB_ERR);
        return 0;
    }
    bpf_probe_read_kernel(entry, 4 + CAPTURE_MAX_DATA, buf);
    bpf_ringbuf_submit(entry, 0);

    stat_inc(STAT_HIT);
    stat_add(STAT_BYTES, (__u64)data_len);

    /* 清除保存的 msg 指针 */
    unsigned long zero = 0;
    bpf_map_update_elem(&entry_msg, &key, &zero, 0);

    return 0;
}

char _license[] SEC("license") = "GPL";
