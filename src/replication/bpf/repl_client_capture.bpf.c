// ============================================================
// repl_client_capture.bpf.c — fentry + fexit BPF 程序
//
// fentry: 保存 msg 指针和 msg_iter.count（用于 fexit 计算 retval）
// fexit:  从 msg->msg_iter 读取已接收的数据，写入 ringbuf
//
// kernel 6.1 的 fexit 不提供返回值，也不支持 bpf_get_func_ret()。
// 通过 fentry 保存 msg_iter.count，fexit 中计算:
//   retval = count_before - count_after
// ============================================================

#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_core_read.h>

#ifndef BPF_MAP_TYPE_RINGBUF
#define BPF_MAP_TYPE_RINGBUF 27
#endif

#define CLIENT_ENTRY_HDR_SZ    4
#define CLIENT_ENTRY_MAX_LEN   8192

/* 从 msg+32 读取 {_count, ptr, _nr} 的偏移 */
struct iov_head {
    unsigned long long _count;
    unsigned long ptr;
    unsigned long _nr;
};

/* fentry→fexit 传递: msg_ptr + count_before */
struct fexit_ctx {
    unsigned long msg_ptr;
    unsigned long long count_before;
    int valid;
};

/* ---- BPF Maps ---- */

struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 8);
    __type(key, __u32);
    __type(value, __u64);
} client_ctl SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 16);
    __type(key, char[32]);
    __type(value, __u64);
} proxy_cfg SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 16);
    __type(key, __u32);
    __type(value, __u64);
} client_stats SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 1 << 22);
} client_cache_ringbuf SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, unsigned char[CLIENT_ENTRY_HDR_SZ + CLIENT_ENTRY_MAX_LEN]);
} client_tmpbuf SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, struct fexit_ctx);
} client_fexit_ctx SEC(".maps");

/* ──── fentry: 保存 msg_ptr + count_before ──── */
SEC("fentry/tcp_recvmsg")
int fentry_tcp_recvmsg(__u64 *ctx)
{
    __u64 *ctl_pid = bpf_map_lookup_elem(&client_ctl, &(__u32){1});
    if (!ctl_pid || !*ctl_pid)
        return 0;

    __u32 pid = bpf_get_current_pid_tgid() >> 32;
    if (pid != (__u32)(*ctl_pid))
        return 0;

    unsigned long msg_ptr = (unsigned long)ctx[1];
    if (!msg_ptr) return 0;

    struct iov_head head;
    if (bpf_probe_read_kernel(&head, sizeof(head),
            (const void *)(msg_ptr + 32)) != 0)
        return 0;

    __u32 map_key = 0;
    struct fexit_ctx e = {
        .msg_ptr = msg_ptr,
        .count_before = head._count,
        .valid = 1,
    };
    bpf_map_update_elem(&client_fexit_ctx, &map_key, &e, 0);
    return 0;
}

/* ──── fexit: 读取数据，写入 ringbuf ──── */
SEC("fexit/tcp_recvmsg")
int fexit_tcp_recvmsg(__u64 *ctx)
{
    __u32 map_key = 0;

    struct fexit_ctx *ec = bpf_map_lookup_elem(&client_fexit_ctx, &map_key);
    if (!ec || !ec->valid || ec->msg_ptr == 0)
        return 0;
    ec->valid = 0;  /* 消费标记 */

    /* 读取当前 iov 头，计算 retval = count_before - count_after */
    struct iov_head head;
    if (bpf_probe_read_kernel(&head, sizeof(head),
            (const void *)(ec->msg_ptr + 32)) != 0)
        return 0;

    long long count_after = (long long)head._count;
    long long count_before = (long long)ec->count_before;
    long retval = (long)(count_before - count_after);
    if (retval <= 0)
        return 0;

    if (retval > CLIENT_ENTRY_MAX_LEN)
        retval = CLIENT_ENTRY_MAX_LEN;

    unsigned char(*entry)[CLIENT_ENTRY_HDR_SZ + CLIENT_ENTRY_MAX_LEN];
    entry = bpf_map_lookup_elem(&client_tmpbuf, &map_key);
    if (!entry) return 0;

    int data_len;
    unsigned long user_ptr;
    {
        if (head._nr > 0) {
            if (!head.ptr) return 0;
            struct { unsigned long b; unsigned long l; } vec;
            if (bpf_probe_read_kernel(&vec, sizeof(vec),
                    (const void *)head.ptr) != 0)
                return 0;
            if (!vec.b || vec.l == 0) return 0;
            unsigned long long safe_len = vec.l;
            if (safe_len > (unsigned long long)retval)
                safe_len = (unsigned long long)retval;
            if (safe_len > CLIENT_ENTRY_MAX_LEN)
                safe_len = CLIENT_ENTRY_MAX_LEN;
            if (safe_len == 0) return 0;
            data_len = (int)safe_len;
            user_ptr = (unsigned long)vec.b;
        } else {
            if (!head.ptr || head._count == 0) return 0;
            unsigned long orig_buf = head.ptr - (unsigned long)retval;
            data_len = (int)retval;
            user_ptr = orig_buf;
        }
    }
    if (data_len <= 0 || user_ptr == 0) return 0;

    __u32 payload_len = (__u32)data_len;
    __builtin_memcpy(*entry, &payload_len, 4);

    if (bpf_probe_read_user((*entry) + 4, (__u32)data_len,
            (const void *)user_ptr) != 0)
        return 0;

    bpf_ringbuf_output(&client_cache_ringbuf, *entry,
                        CLIENT_ENTRY_HDR_SZ + data_len, 0);
    return 0;
}

char _license[] SEC("license") = "GPL";
