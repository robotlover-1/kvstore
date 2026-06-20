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
    __uint(max_entries, 1 << 20); /* 1MB */
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

/* 从 msghdr 的 iovec 读取数据
 * msghdr + 40 = iov 指针, + 48 = nr_segs */
static __always_inline int read_iov_data(unsigned long msg_ptr,
    unsigned char *buf, int max_len)
{
    struct { unsigned long base; unsigned long len; } *iov = 0;
    if (bpf_probe_read_kernel(&iov, sizeof(iov),
            (const void *)(msg_ptr + 40)) != 0)
        return 0;
    if (!iov) return 0;

    unsigned long nr_segs = 0;
    if (bpf_probe_read_kernel(&nr_segs, sizeof(nr_segs),
            (const void *)(msg_ptr + 48)) != 0)
        return 0;
    if (nr_segs == 0) return 0;

    struct { unsigned long base; unsigned long len; } vec;
    if (bpf_probe_read_kernel(&vec, sizeof(vec), &iov[0]) != 0)
        return 0;
    if (!vec.base || vec.len == 0) return 0;

    unsigned long long safe_len = vec.len;
    if (safe_len > (unsigned long long)max_len)
        safe_len = (unsigned long long)max_len;

    if (bpf_probe_read_user(buf, (__u32)safe_len,
            (const void *)(unsigned long)vec.base) != 0)
        return 0;
    return (int)safe_len;
}

/* ──── Entry: 保存 msg 指针 ──── */
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

/* ──── Return: 读取数据并写 ringbuf ──── */
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

    __u32 size = (__u32)retval;
    if (size > CAPTURE_MAX_DATA) size = CAPTURE_MAX_DATA;

    unsigned char *buf = bpf_map_lookup_elem(&tmpbuf, &key);
    if (!buf) return 0;

    /* 头 4 字节 = payload 长度 */
    __u32 plen = 0;
    __builtin_memcpy(buf, &plen, 4);

    int data_len = read_iov_data(*msg_ptr, buf + 4, CAPTURE_MAX_DATA);
    if (data_len <= 0) return 0;

    plen = (__u32)data_len;
    __builtin_memcpy(buf, &plen, 4);

    int total = 4 + data_len;
    if (bpf_ringbuf_output(&ringbuf, buf, total, 0) != 0) {
        stat_inc(STAT_RB_ERR);
        return 0;
    }

    stat_inc(STAT_HIT);
    stat_add(STAT_BYTES, (__u64)data_len);

    /* 清除保存的 msg 指针 */
    unsigned long zero = 0;
    bpf_map_update_elem(&entry_msg, &key, &zero, 0);

    return 0;
}

char _license[] SEC("license") = "GPL";
