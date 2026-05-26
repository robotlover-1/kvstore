// ============================================================
// repl_kprobe.bpf.c — kprobe BPF 程序
//
// Hook tcp_sendmsg，拦截 replication 流量。
// 从 tcp_sendmsg(struct sock *sk, struct msghdr *msg, size_t size) 的
// msg 参数中读取实际数据，写入 BPF ringbuf。
//
// 用户态回调 kprobe_ringbuf_cb 收到数据后进行 RDMA WRITE。
//
// x86_64 调用约定:
//   PT_REGS_PARM1(ctx) = sk (struct sock*)
//   PT_REGS_PARM2(ctx) = msg (struct msghdr*)
//   PT_REGS_PARM3(ctx) = size (size_t)
//
// 限制: 单次最大 500 字节，当前增量数据量小可满足。
// ============================================================

#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

/* ---- 兼容性定义 ---- */
#ifndef BPF_MAP_TYPE_RINGBUF
#define BPF_MAP_TYPE_RINGBUF 27
#endif

/* ringbuf entry 格式: [4B payload_len][payload_len bytes payload]
 * payload_len == 0 表示仅通知（无实际数据） */
#define KVS_KPROBE_ENTRY_HDR_SZ   4
#define KVS_KPROBE_ENTRY_MAX_LEN  500  /* 单次最大数据长度 */

/* 手动定义 pt_regs（bpf target 下系统头文件不可用）
 * x86_64 寄存器命名: r15, r14, ..., rdi, rsi, rdx, rcx, rax, ... */
struct pt_regs {
    unsigned long r15;    unsigned long r14;
    unsigned long r13;    unsigned long r12;
    unsigned long rbp;    unsigned long rbx;
    unsigned long r11;    unsigned long r10;
    unsigned long r9;     unsigned long r8;
    unsigned long rax;    unsigned long rcx;
    unsigned long rdx;    unsigned long rsi;
    unsigned long rdi;    unsigned long orig_rax;
    unsigned long rip;    unsigned long cs;
    unsigned long rflags; unsigned long rsp;
    unsigned long ss;
};

/* 手动定义 msghdr/iovec，避免依赖系统头文件 */
struct bpf_iovec {
    __u64 iov_base;   /* void __user * */
    __u64 iov_len;    /* __kernel_size_t */
};

/* 只定义我们需要的字段 */
struct bpf_msghdr {
    __u64 msg_name;       /* void * */
    int    msg_namelen;
    int    msg_flags;
    /* msg_iter 从 offset 16 开始:
     *   iter_type(1) + copy_mc(1) + no_refault(1) + data_source(1) + __pad(1) = 5B
     *   padding 3B
     *   iov(8B) at offset 8 within iov_iter
     *   nr_segs(8B) at offset 16 within iov_iter
     * 所以 iov_ptr 在 msg+24, nr_segs 在 msg+32 */
    __u64 __padding[2];   /* 占位到 offset 16 */
};

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

/* 临时缓冲区（per-CPU，避免 BPF 栈 512B 限制） */
struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, unsigned char[KVS_KPROBE_ENTRY_HDR_SZ + KVS_KPROBE_ENTRY_MAX_LEN]);
} kprobe_tmpbuf SEC(".maps");

/* ---- Control Keys ---- */
#define KVS_KPROBE_CTL_ENABLED  0
#define KVS_KPROBE_CTL_PID      1

/* ---- Stats Keys ---- */
#define KVS_KPROBE_STAT_HIT      0
#define KVS_KPROBE_STAT_SKIP_PID 1
#define KVS_KPROBE_STAT_RB_ERR   2
#define KVS_KPROBE_STAT_DATA_OVR 3  /* 数据超过单次上限 */
#define KVS_KPROBE_STAT_READ_ERR 4  /* probe_read 失败 */

/* ringbuf entry 格式: [4B payload_len][payload_len bytes payload]
 * payload_len == 0 表示仅通知（无实际数据） */
#define KVS_KPROBE_ENTRY_HDR_SZ   4
#define KVS_KPROBE_ENTRY_MAX_LEN  500  /* 单次最大数据长度 */

/*
 * 从 tcp_sendmsg 的 msg 参数中读取用户态数据。
 *
 * struct msghdr 在 kernel 5.15 x86_64 的典型布局:
 *   offset 0:  msg_name     (void*, 8B)
 *   offset 8:  msg_namelen  (int, 4B)
 *   offset 12: msg_flags    (unsigned int, 4B)
 *   offset 16: msg_iter     (struct iov_iter)
 *
 * struct iov_iter:
 *   offset 0:  iter_type+copy_mc+no_refault+data_source(+__pad) (5B)
 *   offset 5-7: padding (3B)
 *   offset 8:  iov  (const struct iovec*, 8B)
 *   offset 16: nr_segs (unsigned long, 8B)
 *
 * struct iovec:
 *   offset 0: iov_base (void*, 8B)
 *   offset 8: iov_len  (size_t, 8B)
 *
 * 所以 iov 指针在 msg + 16 + 8 = msg + 24 处
 */

/* 从 userspace 的 iovec 数组中读取数据拼接到 buf
 * 返回总数据长度（<= max_len），0 表示失败 */
static __always_inline int read_msg_data(const struct bpf_msghdr *msg,
    unsigned char *buf, int max_len)
{
    /* 1. 读取 iov 指针 (msg + 24) */
    const struct bpf_iovec *iov = 0;
    if (bpf_probe_read_user(&iov, sizeof(iov), (const void *)msg + 24) != 0)
        return 0;
    if (!iov) return 0;

    /* 2. 读取 nr_segs (msg + 32) */
    unsigned long nr_segs = 0;
    if (bpf_probe_read_user(&nr_segs, sizeof(nr_segs), (const void *)msg + 32) != 0)
        return 0;
    if (nr_segs == 0) return 0;

    /* 3. 遍历 iovec 读取数据 */
    int total = 0;
    #pragma unroll
    for (int i = 0; i < 8; i++) {
        if (i >= nr_segs || total >= max_len) break;

        struct bpf_iovec vec;
        if (bpf_probe_read_user(&vec, sizeof(vec), &iov[i]) != 0) break;
        if (!vec.iov_base || vec.iov_len == 0) continue;

        int chunk = vec.iov_len;
        if (total + chunk > max_len)
            chunk = max_len - total;

        if (bpf_probe_read_user(buf + total, chunk, (const void *)(unsigned long)vec.iov_base) != 0) {
            /* 记录读错误统计 */
            __u64 *st = bpf_map_lookup_elem(&kprobe_stats,
                &(__u32){KVS_KPROBE_STAT_READ_ERR});
            if (st) __sync_fetch_and_add(st, 1);
            break;
        }
        total += chunk;
    }
    return total;
}

SEC("kprobe/tcp_sendmsg")
int kprobe_kvs_repl_tcp_sendmsg(struct pt_regs *ctx)
{
    __u64 *enabled, *target_pid, *stat;

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

    /* 3. 获取数据长度 */
    __u32 size = (__u32)PT_REGS_PARM3(ctx);
    if (size == 0) return 0;
    if (size > KVS_KPROBE_ENTRY_MAX_LEN) {
        stat = bpf_map_lookup_elem(&kprobe_stats,
            &(__u32){KVS_KPROBE_STAT_DATA_OVR});
        if (stat) __sync_fetch_and_add(stat, 1);
        size = KVS_KPROBE_ENTRY_MAX_LEN;
    }

    /* 4. 从 msg 参数中读取数据 — 使用 per-CPU 数组避免栈溢出 */
    __u32 map_key = 0;
    unsigned char(*entry)[KVS_KPROBE_ENTRY_HDR_SZ + KVS_KPROBE_ENTRY_MAX_LEN];
    entry = bpf_map_lookup_elem(&kprobe_tmpbuf, &map_key);
    if (!entry) return 0;

    const struct bpf_msghdr *msg = (const struct bpf_msghdr *)PT_REGS_PARM2(ctx);

    /* 先写 4 字节长度头（初始为 0，读取成功后再更新） */
    __u32 payload_len = 0;
    __builtin_memcpy(*entry, &payload_len, 4);

    int data_len = read_msg_data(msg, (*entry) + 4, KVS_KPROBE_ENTRY_MAX_LEN);
    if (data_len <= 0) {
        /* 读数据失败，退化为通知模式 */
        payload_len = 0;
        __builtin_memcpy(*entry, &payload_len, 4);
        if (bpf_ringbuf_output(&repl_ringbuf, *entry, 4, 0) != 0) {
            stat = bpf_map_lookup_elem(&kprobe_stats,
                &(__u32){KVS_KPROBE_STAT_RB_ERR});
            if (stat) __sync_fetch_and_add(stat, 1);
        }
        return 0;
    }

    /* 5. 更新 payload_len 并写入 ringbuf */
    payload_len = (__u32)data_len;
    __builtin_memcpy(*entry, &payload_len, 4);

    int entry_size = KVS_KPROBE_ENTRY_HDR_SZ + data_len;
    if (bpf_ringbuf_output(&repl_ringbuf, *entry, entry_size, 0) != 0) {
        stat = bpf_map_lookup_elem(&kprobe_stats,
            &(__u32){KVS_KPROBE_STAT_RB_ERR});
        if (stat) __sync_fetch_and_add(stat, 1);
        return 0;
    }

    /* 6. 更新统计 */
    stat = bpf_map_lookup_elem(&kprobe_stats, &(__u32){KVS_KPROBE_STAT_HIT});
    if (stat) __sync_fetch_and_add(stat, 1);

    return 0;  /* 放行 TCP 正常发送 */
}

char LICENSE[] SEC("license") = "GPL";
