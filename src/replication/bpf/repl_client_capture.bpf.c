// ============================================================
// repl_client_capture.bpf.c — kprobe BPF 程序
//
// Hook tcp_recvmsg，捕获 Master 接收的客户端写入数据。
//
// 使用 kprobe (entry) + kretprobe (return) 组合:
//   1. entry: 保存 msg 指针到 per-CPU map（供 return 使用）
//   2. return: 从 msg->msg_iter->iov 中读取接收到的数据
//
// 全量同步期间 (FULLSYNC_IN_PROGRESS=1 in client_ctl[3]):
//   数据写入 client_cache_ringbuf 缓存，全量同步完成后 flush 到 slave
//
// 全量同步完成后 (FULLSYNC_IN_PROGRESS=0):
//   数据仍然写入 ringbuf，用户态回调直接处理
//
// x86_64 调用约定 (tcp_recvmsg):
//   struct sock *sk   = di (PT_REGS_PARM1)
//   struct msghdr *msg = si (PT_REGS_PARM2)
//   size_t size        = dx (PT_REGS_PARM3)
//   返回值 (rax)       = 实际接收字节数
// ============================================================

#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_core_read.h>

#ifndef BPF_MAP_TYPE_RINGBUF
#define BPF_MAP_TYPE_RINGBUF 27
#endif

#define CLIENT_ENTRY_HDR_SZ    4
#define CLIENT_ENTRY_MAX_LEN   8192

/* x86_64 pt_regs */
struct pt_regs {
    unsigned long r15;    unsigned long r14;
    unsigned long r13;    unsigned long r12;
    unsigned long bp;     unsigned long bx;
    unsigned long r11;    unsigned long r10;
    unsigned long r9;     unsigned long r8;
    unsigned long ax;     unsigned long cx;
    unsigned long dx;     unsigned long si;
    unsigned long di;     unsigned long orig_ax;
    unsigned long ip;     unsigned long cs;
    unsigned long flags;  unsigned long sp;
    unsigned long ss;
};

/* ---- BPF Maps ---- */

/* Control Map:
 * [1]: CTL_PID              — 0=禁用, !=0=(enabled, PID=low32)
 * [2]: LISTEN_PORT          — Master 监听端口（用于过滤客户端连接）
 * [3]: FULLSYNC_IN_PROGRESS — 1=全量同步进行中 0=正常
 */
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 8);
    __type(key, __u32);
    __type(value, __u64);
} client_ctl SEC(".maps");

/* Stats Map:
 * [0]: HIT              — 总命中次数
 * [1]: SKIP_PID         — PID 不匹配跳过
 * [2]: RB_ERR           — ringbuf 写入错误
 * [3]: DATA_OVR         — 数据超过上限
 * [4]: READ_ERR         — probe_read 失败
 * [5]: CACHED           — 缓存条目数
 * [6]: RETPROBE_MISS    — kretprobe 未找到 entry 保存的 msg 指针
 * [7]: REPLDONE_DETECT  — tcp_sendmsg 探测到 REPLDONE
 */
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 16);
    __type(key, __u32);
    __type(value, __u64);
} client_stats SEC(".maps");

/* Ringbuf — 缓存客户端写入数据 */
struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 1 << 22);  /* 4MB */
} client_cache_ringbuf SEC(".maps");

/* 临时缓冲区（per-CPU，避免 BPF 栈 512B 限制） */
struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, unsigned char[CLIENT_ENTRY_HDR_SZ + CLIENT_ENTRY_MAX_LEN]);
} client_tmpbuf SEC(".maps");

/* 用于 kprobe→kretprobe 传递 msg 指针的 per-CPU map
 * key=0: 存储 msg 指针 (unsigned long) */
struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, unsigned long);
} client_entry_msg SEC(".maps");

/* 从 msghdr 的 iovec 中读取已接收的数据
 * 在 kretprobe 中调用（此时数据已写入 iovec）
 *
 * iov_iter 在 msghdr 内偏移 16 字节 (msg_name 8 + msg_namelen 4 + pad 4)
 * iov 指针在 iov_iter 内偏移 24 字节 (iter_type 1 + nofault 1 + data_source 1 + pad 5 + iov_offset 8 + count 8)
 * nr_segs 紧接在 iov 之后，偏移 32 字节
 * iov 和 nr_segs 在 msg+40 处相邻 16 字节，合并为一次 bpf_probe_read_kernel */
static __always_inline int read_recv_data(unsigned long msg_ptr,
    unsigned char *buf, int max_len)
{
    /* 1. 合并读取 iov 指针 + nr_segs（16 字节从 msg_ptr + 40） */
    struct { unsigned long iov_ptr; unsigned long _nr_segs; } head;
    if (bpf_probe_read_kernel(&head, sizeof(head),
            (const void *)(msg_ptr + 40)) != 0)
        return 0;
    if (!head.iov_ptr || head._nr_segs == 0) return 0;

    /* 2. 读取第一个 iovec */
    struct { unsigned long b; unsigned long l; } vec;
    if (bpf_probe_read_kernel(&vec, sizeof(vec),
            (const void *)head.iov_ptr) != 0)
        return 0;
    if (!vec.b || vec.l == 0)
        return 0;

    unsigned long long safe_len = vec.l;
    if (safe_len > (unsigned long long)max_len)
        safe_len = (unsigned long long)max_len;
    if (safe_len == 0) return 0;

    /* 3. 从用户空间读取数据（kretprobe 时数据已写入 iovec） */
    if (bpf_probe_read_user(buf, (__u32)safe_len,
            (const void *)(unsigned long)vec.b) != 0) {
        __u64 *st = bpf_map_lookup_elem(&client_stats,
            &(__u32){4});  /* READ_ERR */
        if (st) __sync_fetch_and_add(st, 1);
        return 0;
    }
    return (int)safe_len;
}

/* ──── Entry kprobe: 保存 msg 指针供 kretprobe 使用 ──── */
SEC("kprobe/tcp_recvmsg")
int kprobe_client_recv_entry(struct pt_regs *ctx)
{
    /* CTL_PID=0 表示禁用，无需额外 ENABLED 键 */
    __u64 *ctl_pid = bpf_map_lookup_elem(&client_ctl, &(__u32){1});
    if (!ctl_pid || !*ctl_pid)
        return 0;

    /* PID 过滤 — 只捕获 Master 进程的数据接收 */
    __u32 pid = bpf_get_current_pid_tgid() >> 32;
    if (pid != (__u32)(*ctl_pid)) {
        __u64 *st = bpf_map_lookup_elem(&client_stats, &(__u32){1}); /* SKIP_PID */
        if (st) __sync_fetch_and_add(st, 1);
        return 0;
    }

    /* 保存 msg 指针 (si = PT_REGS_PARM2) 到 per-CPU map */
    unsigned long msg_ptr = (unsigned long)ctx->si;
    __u32 map_key = 0;
    bpf_map_update_elem(&client_entry_msg, &map_key, &msg_ptr, 0);

    return 0;
}

/* ──── Return kretprobe: 从 iovec 读取接收到的数据 ──── */
SEC("kretprobe/tcp_recvmsg")
int kprobe_client_recv_return(struct pt_regs *ctx)
{
    /* CTL_PID=0 表示禁用 */
    __u64 *ctl_pid = bpf_map_lookup_elem(&client_ctl, &(__u32){1});
    if (!ctl_pid || !*ctl_pid)
        return 0;

    /* 获取返回值 = 实际接收字节数 */
    long retval = (long)ctx->ax;
    if (retval <= 0)
        return 0;

    /* 获取 entry 保存的 msg 指针 */
    __u32 map_key = 0;
    unsigned long *msg_ptr = bpf_map_lookup_elem(&client_entry_msg, &map_key);
    if (!msg_ptr || *msg_ptr == 0) {
        __u64 *miss = bpf_map_lookup_elem(&client_stats, &(__u32){6}); /* RETPROBE_MISS */
        if (miss) __sync_fetch_and_add(miss, 1);
        return 0;
    }

    /* 限制数据大小 */
    __u32 size = (__u32)retval;
    if (size > CLIENT_ENTRY_MAX_LEN) {
        __u64 *st = bpf_map_lookup_elem(&client_stats, &(__u32){3}); /* DATA_OVR */
        if (st) __sync_fetch_and_add(st, 1);
        size = CLIENT_ENTRY_MAX_LEN;
    }

    /* 读取数据到 tmpbuf */
    unsigned char(*entry)[CLIENT_ENTRY_HDR_SZ + CLIENT_ENTRY_MAX_LEN];
    entry = bpf_map_lookup_elem(&client_tmpbuf, &map_key);
    if (!entry) return 0;

    __u32 payload_len = 0;
    __builtin_memcpy(*entry, &payload_len, 4);

    int data_len = read_recv_data(*msg_ptr, (*entry) + 4, CLIENT_ENTRY_MAX_LEN);
    if (data_len <= 0) return 0;

    payload_len = (__u32)data_len;
    __builtin_memcpy(*entry, &payload_len, 4);

    /* 写入 ringbuf */
    int entry_size = CLIENT_ENTRY_HDR_SZ + data_len;
    if (bpf_ringbuf_output(&client_cache_ringbuf, *entry, entry_size, 0) != 0) {
        __u64 *st = bpf_map_lookup_elem(&client_stats, &(__u32){2}); /* RB_ERR */
        if (st) __sync_fetch_and_add(st, 1);
        return 0;
    }

    /* 更新统计 */
    __u64 *st = bpf_map_lookup_elem(&client_stats, &(__u32){0}); /* HIT */
    if (st) __sync_fetch_and_add(st, 1);

    __u64 *in_progress = bpf_map_lookup_elem(&client_ctl, &(__u32){3});
    if (in_progress && *in_progress) {
        st = bpf_map_lookup_elem(&client_stats, &(__u32){5}); /* CACHED */
        if (st) __sync_fetch_and_add(st, 1);
    }

    return 0;
}

/* ──── kprobe: 探测 REPLDONE 发送 → 自动关闭全量同步标志 ────
 *
 * Hook tcp_sendmsg，拦截 Master 发出的数据。
 * 当检测到 REPLDONE 时，自动清除 client_ctl[3] (FULLSYNC_IN_PROGRESS)。
 *
 * 这比用户态 queue_snapshot() 中调用 repl_client_capture_set_fullsync(0)
 * 更早一步——REPLDONE 报文刚发出，BPF 就切到 INCR 模式。
 *
 * x86_64 调用约定 (tcp_sendmsg):
 *   struct sock *sk   = di (PT_REGS_PARM1)
 *   struct msghdr *msg = si (PT_REGS_PARM2)
 *   size_t size        = dx (PT_REGS_PARM3)
 */
SEC("kprobe/tcp_sendmsg")
int kprobe_client_sendmsg(struct pt_regs *ctx)
{
	/* CTL_PID=0 表示禁用 */
	__u64 *ctl_pid = bpf_map_lookup_elem(&client_ctl, &(__u32){1});
	if (!ctl_pid || !*ctl_pid)
		return 0;

	/* PID 过滤 — 只关注 Master 进程发出的数据 */
	__u32 pid = bpf_get_current_pid_tgid() >> 32;
	if (pid != (__u32)(*ctl_pid))
		return 0;

	/* 只在全量同步进行中才探测 REPLDONE */
	__u64 *in_progress = bpf_map_lookup_elem(&client_ctl, &(__u32){3});
	if (!in_progress || !*in_progress)
		return 0;

	/* 4. 获取 msg 指针 (si = PT_REGS_PARM2) */
	unsigned long msg_ptr = (unsigned long)ctx->si;
	if (!msg_ptr)
		return 0;

	/* 5. 读取发送数据到临时缓冲区 */
	__u32 map_key = 0;
	unsigned char(*entry)[CLIENT_ENTRY_HDR_SZ + CLIENT_ENTRY_MAX_LEN];
	entry = bpf_map_lookup_elem(&client_tmpbuf, &map_key);
	if (!entry) return 0;

	int data_len = read_recv_data(msg_ptr, *entry, CLIENT_ENTRY_MAX_LEN);
	if (data_len < 8)  /* "REPLDONE" 至少 8 字节 */
		return 0;

	/* 6. 在前 64 字节中扫描 "REPLDONE" 子串
	 * 64 字节足够覆盖 RESP 头 (*1\r\n$8\r\n 最多 10 字节) + 数据
	 * 使用固定上限 + pragma unroll 保证 verifier 接受 */
	int found = 0;
#pragma unroll
	for (int i = 0; i < 64; i++) {
		if (!found && i + 8 <= data_len &&
		    (*entry)[i] == 'R' && (*entry)[i+1] == 'E' &&
		    (*entry)[i+2] == 'P' && (*entry)[i+3] == 'L' &&
		    (*entry)[i+4] == 'D' && (*entry)[i+5] == 'O' &&
		    (*entry)[i+6] == 'N' && (*entry)[i+7] == 'E') {
			found = 1;
		}
	}

	if (!found)
		return 0;

	/* 7. 检测到 REPLDONE → 自动清除 FULLSYNC_IN_PROGRESS
	 * 此后 BPF 内核态已知全量同步结束，用户态 flush 后即可转发 */
	__u32 ctl_key = 3;
	__u64 zero = 0;
	bpf_map_update_elem(&client_ctl, &ctl_key, &zero, 0);

	/* 8. 统计 */
	__u64 *stat = bpf_map_lookup_elem(&client_stats, &(__u32){7}); /* REPLDONE_DETECT */
	if (stat) __sync_fetch_and_add(stat, 1);

	return 0;
}

char _license[] SEC("license") = "GPL";
