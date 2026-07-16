#include "kvstore/kvstore.h"
#include <sys/wait.h>
#include <signal.h>
#include <sys/mman.h>
#include <liburing.h>
#include <sys/eventfd.h>

int g_aof_fd = -1;
pid_t g_bgsave_pid = -1;
long long g_bgsave_last_start_ms = 0;
long long g_bgsave_last_end_ms = 0;
unsigned long long g_dirty_counter = 0;

static long long g_aof_write_offset = 0;     /* confirmed by CQE */
static long long g_aof_write_submitted = 0;  /* staged into SQE, not yet confirmed */

static int g_persist_recovering = 0;
static long long g_recover_last_total_ms = 0;
static long long g_recover_last_dump_ms = 0;
static long long g_recover_last_aof_ms = 0;
static unsigned long long g_recover_mmap_attempts = 0;
static unsigned long long g_recover_mmap_success = 0;
static unsigned long long g_recover_mmap_fallbacks = 0;
static unsigned long long g_recover_last_mmap_bytes = 0;
static unsigned long long g_recover_last_fread_bytes = 0;
static unsigned long long g_recover_last_tail_bytes = 0;

static int g_bgsave_status = 0; /* 0 idle, 1 running, 2 ok, 3 err */
static unsigned long long g_bgsave_base_dirty = 0;
static long long g_last_snapshot_ms = 0;

static int g_aof_disabled = 0;

int persist_aof_disable(void) {
    g_aof_disabled = 1;
    return 0;
}

static pid_t g_bgrewrite_pid = -1;
static int g_bgrewrite_status = 0; /* 0 idle, 1 running, 2 ok, 3 err */
static char g_rewrite_tmp_path[512] = {0};

typedef struct rewrite_buf_node_s {
    unsigned char *data;
    size_t len;
    struct rewrite_buf_node_s *next;
} rewrite_buf_node_t;

static rewrite_buf_node_t *g_rewrite_buf_head = NULL;
static rewrite_buf_node_t *g_rewrite_buf_tail = NULL;
static pthread_mutex_t g_rewrite_buf_lock = PTHREAD_MUTEX_INITIALIZER;

/* ---- async persist inflight ring ----
 *
 * Each in-flight AOF command owns one slot.  The slot holds:
 *   - a safe copy of the AOF payload (so the async write never
 *     references connection input-buffer memory that may be
 *     overwritten by the next recv or memmove);
 *   - the response buffer to send back to the client after both
 *     write and fsync CQEs complete.
 *
 * Both SQEs (write + fsync) carry the slot pointer as user_data,
 * so CQEs can arrive in any order and still match the correct slot.
 * This replaces the previous TAILQ-based approach that assumed
 * strict FIFO CQE ordering.
 */
#define PERSIST_INFLIGHT_SIZE 1024
/* soft limit: trigger non-blocking reap when inflight reaches this.
   must be < min(ring_entries/2, PERSIST_INFLIGHT_SIZE) so the SQ ring
   doesn't fill up before the inflight check kicks in.
   ring=1024 → 512 SQE pairs → set to 480 to leave margin. */
#define MAX_AOF_INFLIGHT      480

typedef struct persist_slot_s {
    conn_t        *conn;
    unsigned char *resp;        /* owned; freed on slot release */
    size_t         resp_len;
    unsigned char *aof_buf;     /* owned; safe copy for async write */
    size_t         aof_len;
    int            cqe_seen;    /* 0 → 1 → 2 (write CQE + fsync CQE) */
    int            cqe_ok;      /* how many CQEs succeeded */
    int            last_error;
    int            in_use;
    /* group commit linkage */
    struct persist_slot_s *group_next;  /* next cmd slot in group, NULL if none */
} persist_slot_t;

static persist_slot_t g_inflight[PERSIST_INFLIGHT_SIZE];
static int g_inflight_count = 0;
static int g_inflight_head = 0;  /* next candidate allocation index */

static int g_persist_eventfd = -1;
static int g_persist_fatal_error = 0;
static int g_persist_aof_registered = 0;  /* g_aof_fd registered as fixed file index 0 */
/* ---- end async persist inflight ring ---- */

/* ---- group commit (pipeline fsync batching) ----
 *
 * In pipeline mode (P > 1), multiple commands arrive in a single recv()
 * batch.  Instead of one fsync per command, we link all writes together
 * via IOSQE_IO_LINK and append a single fsync at the end.  Every command
 * still gets its own write SQE; the fsync is shared.
 *
 * Group cmd slots expect 1 CQE (write).  The sync slot expects 1 CQE
 * (fsync).  When the fsync CQE arrives, all group cmd slots are released
 * and their responses sent — preserving appendfsync=always durability.
 */
static struct {
    int             active;
    persist_slot_t *cmd_head;  /* first cmd slot in group */
    persist_slot_t *cmd_tail;  /* last cmd slot (for appending) */
} g_group;

static int g_persist_uring_ready = 0;
static struct io_uring g_persist_uring;

/* ---- slot ring helpers ---- */

static persist_slot_t *persist_inflight_reserve(void) {
    if (g_inflight_count >= PERSIST_INFLIGHT_SIZE) return NULL;

    for (int i = 0; i < PERSIST_INFLIGHT_SIZE; i++) {
        int idx = (g_inflight_head + i) % PERSIST_INFLIGHT_SIZE;
        if (!g_inflight[idx].in_use) {
            persist_slot_t *s = &g_inflight[idx];
            memset(s, 0, sizeof(*s));
            s->in_use = 1;
            g_inflight_head = (idx + 1) % PERSIST_INFLIGHT_SIZE;
            g_inflight_count++;
            return s;
        }
    }
    return NULL;
}

static void persist_inflight_release(persist_slot_t *s) {
    if (!s || !s->in_use) return;
    kvs_free(s->resp);
    kvs_free(s->aof_buf);
    memset(s, 0, sizeof(*s));
    g_inflight_count--;
}

/* ---- uring lifecycle ---- */

static int persist_uring_init_once(void) {
    if (g_persist_uring_ready) return 0;

    struct io_uring_params p;
    memset(&p, 0, sizeof(p));
    /* SQPOLL: kernel thread polls SQ ring, userspace io_uring_submit()
       becomes a cheap memory barrier instead of io_uring_enter syscall.
       Well-suited for high-frequency small-I/O per-command submission.
       sq_thread_idle=2000: kernel thread parks after 2s of inactivity.
       If the kernel or system doesn't support SQPOLL, we fall back to
       non-SQPOLL mode — not a fatal error. */
    p.flags = IORING_SETUP_SINGLE_ISSUER
            | IORING_SETUP_COOP_TASKRUN
            | IORING_SETUP_SQPOLL;
    p.sq_thread_idle = 2000;

    if (io_uring_queue_init_params(1024, &g_persist_uring, &p) != 0) {
        /* fallback: retry without SQPOLL */
        memset(&p, 0, sizeof(p));
        p.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_COOP_TASKRUN;
        if (io_uring_queue_init_params(1024, &g_persist_uring, &p) != 0)
            return -1;
    }

    g_persist_eventfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (g_persist_eventfd < 0) {
        io_uring_queue_exit(&g_persist_uring);
        return -1;
    }
    if (io_uring_register_eventfd(&g_persist_uring, g_persist_eventfd) != 0) {
        close(g_persist_eventfd);
        g_persist_eventfd = -1;
        io_uring_queue_exit(&g_persist_uring);
        return -1;
    }
    g_persist_uring_ready = 1;

    /* register AOF fd as fixed file (index 0) to reduce per-SQE fd lookup cost.
       if registration fails we fall back to normal fd — not a fatal error. */
    if (g_aof_fd >= 0) {
        if (io_uring_register_files(&g_persist_uring, &g_aof_fd, 1) == 0)
            g_persist_aof_registered = 1;
    }
    return 0;
}

static void persist_uring_close(void) {
    if (!g_persist_uring_ready) return;
    if (g_persist_aof_registered) {
        io_uring_unregister_files(&g_persist_uring);
        g_persist_aof_registered = 0;
    }
    if (g_persist_eventfd >= 0) {
        io_uring_unregister_eventfd(&g_persist_uring);
        close(g_persist_eventfd);
        g_persist_eventfd = -1;
    }
    io_uring_queue_exit(&g_persist_uring);
    g_persist_uring_ready = 0;
}

/* ---- submit ---- */

void persist_submit_sqes(void) {
    if (!g_persist_uring_ready) return;
    io_uring_submit(&g_persist_uring);
}

/* ---- reap completions ----
 *
 * Each CQE carries the slot pointer as user_data.
 *
 * Normal slots expect 2 CQEs (write + fsync) and release when
 * cqe_seen reaches 2.
 *
 * Group-commit mode: cmd slots expect 1 CQE (write only).  A
 * separate "sync slot" (identified by conn == NULL) carries the
 * group fsync.  When the sync slot's CQE arrives it completes
 * every cmd slot in the group, ensuring all responses are sent
 * only after the shared fsync is durable.
 */

static void persist_reap_one(struct io_uring_cqe *cqe) {
    persist_slot_t *s = (persist_slot_t *)io_uring_cqe_get_data(cqe);
    if (!s) {
        fprintf(stderr, "persist: CQE without slot\n");
        io_uring_cqe_seen(&g_persist_uring, cqe);
        return;
    }

    if (cqe->res > 0) {
        g_aof_write_offset += cqe->res;
        s->cqe_ok++;
    } else if (cqe->res == 0) {
        s->cqe_ok++;
    } else {
        s->last_error = cqe->res;
    }
    s->cqe_seen++;
    io_uring_cqe_seen(&g_persist_uring, cqe);

    /* sync slot (conn==NULL): fsync CQE for a group.
       complete every cmd slot in the linked list. */
    if (s->conn == NULL && s->group_next != NULL) {
        int group_ok = s->cqe_ok;  /* 1 if fsync succeeded */
        persist_slot_t *cmd = s->group_next;
        while (cmd) {
            persist_slot_t *next = cmd->group_next;
            if (group_ok) cmd->cqe_ok++;
            cmd->cqe_seen++;  /* was 1 (write CQE), now 2 */
            if (cmd->cqe_seen == 2) {
                if (cmd->cqe_ok == 2 && cmd->conn && cmd->conn->fd > 0)
                    queue_bytes(cmd->conn, cmd->resp, cmd->resp_len);
                else if (cmd->cqe_ok != 2) {
                    fprintf(stderr, "persist: group cmd error cqe_ok=%d\n",
                            cmd->cqe_ok);
                    if (cmd->conn) cmd->conn->fd = -1;
                    g_persist_fatal_error = 1;
                }
                persist_inflight_release(cmd);
            }
            cmd = next;
        }
        persist_inflight_release(s);
        return;
    }

    /* normal slot or group cmd slot (write CQE):
       group cmd slots have cqe_seen==1 after write and wait for
       the sync slot to bump them to 2 */
    if (s->cqe_seen == 2) {
        if (s->cqe_ok == 2) {
            if (s->conn && s->conn->fd > 0)
                queue_bytes(s->conn, s->resp, s->resp_len);
        } else {
            fprintf(stderr, "persist: CQE error cqe_ok=%d last_error=%d\n",
                    s->cqe_ok, s->last_error);
            if (s->conn) s->conn->fd = -1;
            g_persist_fatal_error = 1;
        }
        persist_inflight_release(s);
    }
    /* group cmd slot with cqe_seen==1: deferred, sync slot will release */
}

void persist_reap_completions(void) {
    if (!g_persist_uring_ready) return;
    struct io_uring_cqe *cqe;
    while (io_uring_peek_cqe(&g_persist_uring, &cqe) == 0)
        persist_reap_one(cqe);
}

/* ---- group commit API ---- */

void persist_group_begin(void) {
    /* don't check g_persist_uring_ready — the uring may not be
       initialised yet (lazy init on first persist_append_prepare).
       if AOF is disabled persist_append_prepare returns early and
       group_commit is a no-op because cmd_head will be NULL. */
    g_group.active = 1;
    g_group.cmd_head = NULL;
    g_group.cmd_tail = NULL;
}

void persist_group_commit(void) {
    struct io_uring_sqe *sqe_f;
    persist_slot_t *sync_slot;
    int aof_fd;

    if (!g_group.active) return;
    g_group.active = 0;

    if (g_group.cmd_head == NULL) return;  /* empty group */

    /* reserve a sync slot and link it to the group's cmd list.
       conn==NULL signals the reap path that this is a group fsync. */
    sync_slot = persist_inflight_reserve();
    if (!sync_slot) {
        fprintf(stderr, "persist: group sync slot alloc failed\n");
        return;
    }
    sync_slot->conn = NULL;
    sync_slot->group_next = g_group.cmd_head;  /* head of cmd linked list */

    sqe_f = io_uring_get_sqe(&g_persist_uring);
    if (!sqe_f) {
        persist_inflight_release(sync_slot);
        return;
    }

    aof_fd = g_persist_aof_registered ? 0 : g_aof_fd;
    io_uring_prep_fsync(sqe_f, aof_fd, IORING_FSYNC_DATASYNC);
    if (g_persist_aof_registered) sqe_f->flags |= IOSQE_FIXED_FILE;
    io_uring_sqe_set_data(sqe_f, sync_slot);

    /* submit all accumulated write SQEs + the trailing fsync */
    persist_submit_sqes();
}

int persist_uring_fd(void) {
    return g_persist_eventfd;
}

void persist_drain_pending(void) {
    if (!g_persist_uring_ready) return;
    while (g_inflight_count > 0) {
        io_uring_submit_and_wait(&g_persist_uring, 1);
        persist_reap_completions();
    }
}

/* re-register AOF fd after bgrewrite replaces it.  must drain before
   calling because in-flight SQEs hold a reference to the old fd index. */
static void persist_reregister_aof_fd(void) {
    if (!g_persist_aof_registered || g_aof_fd < 0) return;
    persist_drain_pending();
    if (io_uring_unregister_files(&g_persist_uring) != 0) {
        g_persist_aof_registered = 0;
        return;
    }
    if (io_uring_register_files(&g_persist_uring, &g_aof_fd, 1) != 0)
        g_persist_aof_registered = 0;
}

/* ---- synchronous uring helpers (used by dump / fsync paths) ---- */

static int persist_uring_wait_single(void) {
    struct io_uring_cqe *cqe = NULL;
    int rc = io_uring_submit_and_wait(&g_persist_uring, 1);
    if (rc < 0) return -1;
    rc = io_uring_wait_cqe(&g_persist_uring, &cqe);
    if (rc < 0 || !cqe) return -1;
    rc = cqe->res;
    io_uring_cqe_seen(&g_persist_uring, cqe);
    return rc;
}

static int persist_write_fd_uring(int fd, const unsigned char *buf, size_t len, off_t *offset) {
    size_t written = 0;
    if (persist_uring_init_once() != 0) return -1;
    while (written < len) {
        struct io_uring_sqe *sqe = io_uring_get_sqe(&g_persist_uring);
        size_t chunk = len - written;
        int rc;
        if (!sqe) return -1;
        io_uring_prep_write(sqe, fd, buf + written, chunk, offset ? *offset : -1);
        rc = persist_uring_wait_single();
        if (rc <= 0) return -1;
        written += (size_t)rc;
        if (offset) *offset += rc;
    }
    return 0;
}

static int persist_fsync_fd_uring(int fd) {
    struct io_uring_sqe *sqe;
    int rc;
    if (persist_uring_init_once() != 0) return -1;
    sqe = io_uring_get_sqe(&g_persist_uring);
    if (!sqe) return -1;
    io_uring_prep_fsync(sqe, fd, 0);
    rc = persist_uring_wait_single();
    return rc < 0 ? -1 : 0;
}

static int persist_write_fd_sync(int fd, const unsigned char *buf, size_t len, off_t *offset) {
    size_t written = 0;
    while (written < len) {
        ssize_t rc = pwrite(fd, buf + written, len - written, offset ? *offset : -1);
        if (rc <= 0) return -1;
        written += (size_t)rc;
        if (offset) *offset += rc;
    }
    return 0;
}

static int persist_write_fd_best_effort(int fd, const unsigned char *buf, size_t len, off_t *offset) {
    if (persist_write_fd_uring(fd, buf, len, offset) == 0) return 0;
    return persist_write_fd_sync(fd, buf, len, offset);
}

int persist_write_raw_fd(int fd, const unsigned char *buf, size_t len, long long *offset_io) {
    off_t off;
    if (fd < 0 || !buf) return -1;
    off = offset_io ? (off_t)(*offset_io) : lseek(fd, 0, SEEK_CUR);
    if (off < 0) return -1;
    if (persist_write_fd_best_effort(fd, buf, len, &off) != 0) return -1;
    if (offset_io) *offset_io = (long long)off;
    return 0;
}

static int persist_fsync_fd_best_effort(int fd) {
    if (persist_fsync_fd_uring(fd) == 0) return 0;
    return fsync(fd);
}

int persist_fsync_fd(int fd) {
    return persist_fsync_fd_best_effort(fd);
}

static int replay_file_fread(const char *path, unsigned long long skip_bytes) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    if (skip_bytes > 0 && fseeko(fp, (off_t)skip_bytes, SEEK_SET) != 0) {
        fclose(fp);
        return 0;
    }
    unsigned char buf[BUFFER_CAP];
    size_t len = 0, n;
    unsigned long long total = 0;
    while ((n = fread(buf + len, 1, sizeof(buf) - len, fp)) > 0) {
        len += n;
        total += (unsigned long long)n;
        parse_resp_stream(NULL, buf, &len, 1);
    }
    g_recover_last_fread_bytes += total;
    fclose(fp);
    return 0;
}

static int replay_file_mmap(const char *path, unsigned long long skip_bytes) {
    int fd;
    struct stat st;
    unsigned char *mapped;
    size_t len;

    g_recover_mmap_attempts++;

    fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    if (fstat(fd, &st) != 0) {
        g_recover_mmap_fallbacks++;
        close(fd);
        return replay_file_fread(path, skip_bytes);
    }
    if ((unsigned long long)st.st_size <= skip_bytes) {
        close(fd);
        return 0;
    }

    mapped = mmap(NULL, (size_t)st.st_size, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    if (mapped == MAP_FAILED) {
        g_recover_mmap_fallbacks++;
        close(fd);
        return replay_file_fread(path, skip_bytes);
    }

    g_recover_mmap_success++;
    g_recover_last_mmap_bytes += (unsigned long long)st.st_size - skip_bytes;

    len = (size_t)st.st_size - (size_t)skip_bytes;
    parse_resp_stream(NULL, mapped + skip_bytes, &len, 1);
    g_recover_last_tail_bytes += (unsigned long long)len;

    munmap(mapped, (size_t)st.st_size);
    close(fd);
    return 0;
}

static int replay_file(const char *path, unsigned long long skip_bytes) {
    return replay_file_mmap(path, skip_bytes);
}

/*
 * 恢复 dump 文件（统一 KVSD 格式）：
 *   [8]  uint64_t aof_offset               — AOF 偏移量，用于跳过早期 AOF
 *   重复：
 *     [1]  uint8_t  engine_id              — KVS_ENGINE_xxx
 *     [1]  uint8_t  flags                  — KVSD_FLAG_HAS_EXPIRE 等
 *     [4]  uint32_t klen
 *     [klen] key
 *     [4]  uint32_t vlen
 *     [vlen] value
 *     [8]  uint64_t expire_at_ms           — 仅当 flags & KVSD_FLAG_HAS_EXPIRE
 *
 * DOC 引擎的 value 格式：field1=val1 field2=val2 ...
 * 返回 aof_offset
 */
unsigned long long replay_dump_file(const char *path) {
    int fd;
    struct stat st;
    unsigned char *mapped;
    size_t pos = 0, size;
    unsigned long long aof_offset = 0;

    fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    if (fstat(fd, &st) != 0) { close(fd); return 0; }
    if (st.st_size <= 0) { close(fd); return 0; }

    mapped = mmap(NULL, (size_t)st.st_size, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    if (mapped == MAP_FAILED) { close(fd); return 0; }
    size = (size_t)st.st_size;
    g_recover_mmap_success++;
    g_recover_last_mmap_bytes += (unsigned long long)size;

    /* read AOF offset header */
    if (pos + sizeof(aof_offset) > size) goto out;
    memcpy(&aof_offset, mapped + pos, sizeof(aof_offset));
    pos += sizeof(aof_offset);

    /* parse entries: engine, flags, klen, key, vlen, value, optional expire_ms */
    while (pos + 2 + 4 <= size) {
        uint8_t engine_id, flags;
        uint32_t klen, vlen;
        char *key, *value;

        engine_id = mapped[pos++];

        /* sanity check: engine_id must be 1-5 */
        if (engine_id < 1 || engine_id > 5) {
            fprintf(stderr, "replay_dump_file: invalid engine_id %u at pos %zu (old format KVSD file?)\n",
                (unsigned int)engine_id, pos - 1);
            break;
        }

        /* read flags (new in unified KVSD format) */
        if (pos + 1 > size) break;
        flags = mapped[pos++];

        if (pos + 4 > size) break;
        memcpy(&klen, mapped + pos, sizeof(klen));
        pos += sizeof(klen);
        if (pos + klen > size) break;

        key = (char *)kvs_malloc(klen + 1);
        if (!key) break;
        if (klen > 0) memcpy(key, mapped + pos, klen);
        key[klen] = '\0';
        pos += klen;

        if (pos + 4 > size) { kvs_free(key); break; }

        memcpy(&vlen, mapped + pos, sizeof(vlen));
        pos += sizeof(vlen);
        if (pos + vlen > size) { kvs_free(key); break; }

        value = (char *)kvs_malloc(vlen + 1);
        if (!value) { kvs_free(key); break; }
        if (vlen > 0) memcpy(value, mapped + pos, vlen);
        value[vlen] = '\0';
        pos += vlen;

        /* dispatch to correct engine */
        switch (engine_id) {
        case KVS_ENGINE_ARRAY:
            kvs_array_set(&global_array, key, value);
            break;
        case KVS_ENGINE_RBTREE:
            kvs_rbtree_set(&global_rbtree, key, value);
            break;
        case KVS_ENGINE_HASH:
            kvs_hash_set(&global_hash, key, value);
            break;
        case KVS_ENGINE_SKIPTABLE:
            kvs_skiptable_set(&global_skiptable, key, value);
            break;
        case KVS_ENGINE_DOC:
            /* DOC value format: "field1=val1 field2=val2 ..." */
            {
                char *tok, *saveptr;
                char *dup = (char *)kvs_malloc(vlen + 1);
                if (dup) {
                    memcpy(dup, value, vlen);
                    dup[vlen] = '\0';
                    tok = strtok_r(dup, " ", &saveptr);
                    while (tok) {
                        char *eq = strchr(tok, '=');
                        if (eq) {
                            *eq = '\0';
                            kvs_doc_set(&global_doc, key, tok, eq + 1);
                        }
                        tok = strtok_r(NULL, " ", &saveptr);
                    }
                    kvs_free(dup);
                }
            }
            break;
        default:
            break;
        }

        /* restore TTL if present */
        if (flags & KVSD_FLAG_HAS_EXPIRE) {
            uint64_t expire_at_ms;
            if (pos + sizeof(expire_at_ms) <= size) {
                memcpy(&expire_at_ms, mapped + pos, sizeof(expire_at_ms));
                pos += sizeof(expire_at_ms);
                long long ttl_ms = (long long)expire_at_ms - kvs_now_ms();
                if (ttl_ms > 0) {
                    kvs_expire_set(&global_expire, engine_id, key, ttl_ms);
                }
            } else {
                kvs_free(key);
                kvs_free(value);
                break;
            }
        }

        kvs_free(key);
        kvs_free(value);

        if (pos >= size) break;
    }

out:
    g_recover_last_tail_bytes += (unsigned long long)pos;
    munmap(mapped, size);
    close(fd);
    return aof_offset;
}

static int persist_flush_aof_fd(int fd) {
    if (fd < 0) return -1;
    if (persist_fsync_fd_best_effort(fd) != 0) return -1;
    return 0;
}

static int persist_save_dump_to(const char *path, unsigned long long aof_offset) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    int rc;
    if (fd < 0) return -1;
    rc = kvs_dump_to_fd(fd, aof_offset);
    if (rc == 0 && persist_fsync_fd(fd) != 0) rc = -1;
    close(fd);
    return rc;
}

static int persist_write_aof_snapshot_to(const char *path) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    int rc;
    if (fd < 0) return -1;
    rc = kvs_snapshot_to_fd(fd);
    if (rc == 0 && persist_fsync_fd(fd) != 0) rc = -1;
    close(fd);
    return rc;
}

static void persist_mark_snapshot_success(unsigned long long snap_dirty) {
    g_bgsave_last_end_ms = kvs_now_ms();
    g_last_snapshot_ms = g_bgsave_last_end_ms;
    if (g_dirty_counter >= snap_dirty) g_dirty_counter -= snap_dirty;
    else g_dirty_counter = 0;
}

static void free_rewrite_buffer_locked(void) {
    rewrite_buf_node_t *cur = g_rewrite_buf_head;
    while (cur) {
        rewrite_buf_node_t *next = cur->next;
        kvs_free(cur->data);
        kvs_free(cur);
        cur = next;
    }
    g_rewrite_buf_head = NULL;
    g_rewrite_buf_tail = NULL;
}

static int append_to_rewrite_buffer(const unsigned char *buf, size_t len) {
    rewrite_buf_node_t *node = (rewrite_buf_node_t *)kvs_malloc(sizeof(*node));
    if (!node) return -1;
    node->data = (unsigned char *)kvs_malloc(len);
    if (!node->data) {
        kvs_free(node);
        return -1;
    }
    memcpy(node->data, buf, len);
    node->len = len;
    node->next = NULL;

    pthread_mutex_lock(&g_rewrite_buf_lock);
    if (g_bgrewrite_pid <= 0) {
        pthread_mutex_unlock(&g_rewrite_buf_lock);
        kvs_free(node->data);
        kvs_free(node);
        return 0;
    }
    if (g_rewrite_buf_tail) g_rewrite_buf_tail->next = node;
    else g_rewrite_buf_head = node;
    g_rewrite_buf_tail = node;
    pthread_mutex_unlock(&g_rewrite_buf_lock);
    return 0;
}

static int finalize_rewrite_parent(void) {
    int fd = open(g_rewrite_tmp_path, O_WRONLY | O_APPEND);
    if (fd < 0) return -1;

    pthread_mutex_lock(&g_rewrite_buf_lock);
    long long off = lseek(fd, 0, SEEK_END);
    for (rewrite_buf_node_t *cur = g_rewrite_buf_head; cur; cur = cur->next) {
        if (off < 0 || persist_write_raw_fd(fd, cur->data, cur->len, &off) != 0) {
            pthread_mutex_unlock(&g_rewrite_buf_lock);
            close(fd);
            return -1;
        }
    }
    pthread_mutex_unlock(&g_rewrite_buf_lock);

    if (persist_flush_aof_fd(fd) != 0) {
        close(fd);
        return -1;
    }
    close(fd);

    if (rename(g_rewrite_tmp_path, g_cfg.aof_path) != 0) return -1;

    if (g_aof_fd >= 0) close(g_aof_fd);
    g_aof_fd = open(g_cfg.aof_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (g_aof_fd < 0) return -1;
    g_aof_write_offset = lseek(g_aof_fd, 0, SEEK_END);
    if (g_aof_write_offset < 0) g_aof_write_offset = 0;
    g_aof_write_submitted = g_aof_write_offset;

    /* re-register new fd as fixed file so subsequent SQEs use IOSQE_FIXED_FILE */
    persist_reregister_aof_fd();

    pthread_mutex_lock(&g_rewrite_buf_lock);
    free_rewrite_buffer_locked();
    pthread_mutex_unlock(&g_rewrite_buf_lock);

    return 0;
}

int persist_init(void) {
    if (g_aof_disabled) {
        g_aof_fd = -1;
        return 0;
    }
    g_aof_fd = open(g_cfg.aof_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (g_aof_fd < 0) return -1;
    g_aof_write_offset = lseek(g_aof_fd, 0, SEEK_END);
    if (g_aof_write_offset < 0) g_aof_write_offset = 0;
    g_aof_write_submitted = g_aof_write_offset;
    /* eager-init uring so eventfd exists before reactor/proactor/ntyco epoll starts */
    if (g_cfg.aof_fsync == KVS_AOF_FSYNC_ALWAYS) {
        persist_uring_init_once();
    }
    return 0;
}

void persist_close(void) {
    persist_drain_pending();
    if (g_aof_fd >= 0) {
        persist_flush_aof_fd(g_aof_fd);
        close(g_aof_fd);
    }
    g_aof_fd = -1;
    persist_uring_close();
}

int persist_set_aof_policy(kvs_aof_fsync_policy_t policy) {
    if (policy != KVS_AOF_FSYNC_OFF && policy != KVS_AOF_FSYNC_ALWAYS) return -1;
    g_cfg.aof_fsync = policy;
    return 0;
}

kvs_aof_fsync_policy_t persist_get_aof_policy(void) {
    return g_cfg.aof_fsync;
}

const char *persist_aof_policy_name(void) {
    return g_cfg.aof_fsync == KVS_AOF_FSYNC_ALWAYS ? "always" : "off";
}

int persist_force_aof_flush(void) {
    if (g_aof_fd < 0) return -1;
    persist_drain_pending();
    if (persist_fsync_fd_best_effort(g_aof_fd) != 0) return -1;
    return 0;
}

/* ---- async append entry point ----
 *
 * Reserve an inflight slot, copy the AOF payload into a safe buffer,
 * bind both SQEs to the slot via user_data, and submit once.
 *
 * On KVS_PERSIST_PENDING the caller transfers ownership of resp and
 * must set resp = NULL.  The response is sent (and both buffers freed)
 * when the reap loop sees cqe_seen == 2.
 *
 * The caller can pass resp == NULL (e.g. slave replication path) —
 * in that case only the AOF write is performed, no client response
 * is deferred.
 */

int persist_append_prepare(conn_t *c, const unsigned char *buf, size_t len,
                           unsigned char *resp, size_t resp_len) {
    struct io_uring_sqe *sqe_w, *sqe_f;
    persist_slot_t *slot;

    if (g_aof_fd < 0) return g_aof_disabled ? KVS_PERSIST_OK : KVS_PERSIST_ERR;
    if (g_cfg.aof_fsync != KVS_AOF_FSYNC_ALWAYS) return KVS_PERSIST_OK;

    if (g_persist_fatal_error) return KVS_PERSIST_ERR;

    if (persist_uring_init_once() != 0) return KVS_PERSIST_ERR;

    /* soft backpressure: try to drain a few completions when the
       inflight queue is deep.  never block — the SQ ring capacity
       (1024 entries = 512 SQE pairs) is the hard limit. */
    if (g_inflight_count >= MAX_AOF_INFLIGHT) {
        persist_submit_sqes();
        persist_reap_completions();
    }

    /* in group mode we only need 1 SQE (write); normal mode needs 2 */
    unsigned need_sqes = g_group.active ? 1u : 2u;
    for (int sq_retry = 0; io_uring_sq_space_left(&g_persist_uring) < need_sqes; sq_retry++) {
        persist_submit_sqes();
        persist_reap_completions();
        if (sq_retry >= 100) return KVS_PERSIST_ERR;
    }

    /* 1. reserve slot BEFORE submit */
    slot = persist_inflight_reserve();
    if (!slot) return KVS_PERSIST_ERR;

    /* 2. copy AOF payload into safe buffer */
    slot->aof_buf = (unsigned char *)kvs_malloc(len);
    if (!slot->aof_buf) {
        persist_inflight_release(slot);
        return KVS_PERSIST_ERR;
    }
    memcpy(slot->aof_buf, buf, len);
    slot->aof_len = len;

    /* 3. save conn + response */
    slot->conn = c;
    slot->resp = resp;
    slot->resp_len = resp_len;

    off_t off = (off_t)g_aof_write_submitted;

    /* 4. write SQE — always with IOSQE_IO_LINK.
       in group mode the writes chain together; the trailing fsync
       (added by persist_group_commit) completes the chain.
       in normal mode the write links to its own fsync below. */
    sqe_w = io_uring_get_sqe(&g_persist_uring);
    if (!sqe_w) {
        persist_inflight_release(slot);
        return KVS_PERSIST_ERR;
    }
    io_uring_prep_write(sqe_w,
                        g_persist_aof_registered ? 0 : g_aof_fd,
                        slot->aof_buf, len, off);
    sqe_w->flags |= IOSQE_IO_LINK;
    if (g_persist_aof_registered) sqe_w->flags |= IOSQE_FIXED_FILE;
    io_uring_sqe_set_data(sqe_w, slot);

    if (g_group.active) {
        /* group mode: accumulate writes, no fsync yet.
           append slot to group linked list for batch completion. */
        slot->group_next = NULL;
        if (g_group.cmd_tail)
            g_group.cmd_tail->group_next = slot;
        else
            g_group.cmd_head = slot;
        g_group.cmd_tail = slot;
    } else {
        /* 5. normal mode: fsync SQE — bind same slot */
        sqe_f = io_uring_get_sqe(&g_persist_uring);
        if (!sqe_f) {
            persist_inflight_release(slot);
            return KVS_PERSIST_ERR;
        }
        io_uring_prep_fsync(sqe_f,
                            g_persist_aof_registered ? 0 : g_aof_fd,
                            IORING_FSYNC_DATASYNC);
        if (g_persist_aof_registered) sqe_f->flags |= IOSQE_FIXED_FILE;
        io_uring_sqe_set_data(sqe_f, slot);
    }

    g_aof_write_submitted += (long long)len;

    /* 6. submit immediately in normal mode; in group mode SQEs
       accumulate and are submitted once by persist_group_commit */
    if (!g_group.active)
        persist_submit_sqes();

    if (g_bgrewrite_pid > 0) append_to_rewrite_buffer(buf, len);

    return KVS_PERSIST_PENDING;
}

/* thin wrapper for callers that don't need response deferral */
int persist_append_raw(const unsigned char *buf, size_t len) {
    int rc = persist_append_prepare(NULL, buf, len, NULL, 0);
    if (rc == KVS_PERSIST_PENDING)
        persist_submit_sqes();
    return rc;
}

int persist_save_dump(void) {
    unsigned long long aof_off = (unsigned long long)g_aof_write_offset;
    int rc = persist_save_dump_to(g_cfg.dump_path, aof_off);
    if (rc == 0) persist_mark_snapshot_success(g_dirty_counter);
    return rc;
}

int persist_recover_in_progress(void) {
    return g_persist_recovering;
}

int persist_recover(void) {
    long long begin_ms = kvs_now_ms();
    long long dump_begin_ms;
    long long aof_begin_ms;

    g_persist_recovering = 1;
    g_recover_last_total_ms = 0;
    g_recover_last_dump_ms = 0;
    g_recover_last_aof_ms = 0;
    g_recover_mmap_attempts = 0;
    g_recover_mmap_success = 0;
    g_recover_mmap_fallbacks = 0;
    g_recover_last_mmap_bytes = 0;
    g_recover_last_fread_bytes = 0;
    g_recover_last_tail_bytes = 0;

    dump_begin_ms = kvs_now_ms();
    unsigned long long aof_offset = replay_dump_file(g_cfg.dump_path);
    g_recover_last_dump_ms = kvs_now_ms() - dump_begin_ms;

    aof_begin_ms = kvs_now_ms();
    if (!g_aof_disabled) {
        replay_file(g_cfg.aof_path, aof_offset);
    }
    g_recover_last_aof_ms = kvs_now_ms() - aof_begin_ms;

    g_recover_last_total_ms = kvs_now_ms() - begin_ms;
    g_dirty_counter = 0;
    g_last_snapshot_ms = kvs_now_ms();
    g_bgsave_last_end_ms = g_last_snapshot_ms;

    /* 清理恢复过程中已过期的 TTL key（短 TTL 可能在恢复期间已过期） */
    kvs_active_expire_cycle(1000000);

    g_persist_recovering = 0;

    return 0;
}

int persist_bgsave_start(void) {
    if (g_bgsave_pid > 0) return 1;

    unsigned long long snap_dirty = g_dirty_counter;
    unsigned long long aof_off = (unsigned long long)g_aof_write_offset;
    long long start_ms = kvs_now_ms();
    char tmp_path[512];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%ld", g_cfg.dump_path, (long)getpid());

    pid_t pid = fork();
    if (pid < 0) {
        g_bgsave_status = 3;
        return -1;
    }
    if (pid == 0) {
        int rc = persist_save_dump_to(tmp_path, aof_off);
        if (rc == 0 && rename(tmp_path, g_cfg.dump_path) != 0) rc = -1;
        if (rc != 0) unlink(tmp_path);
        _exit(rc == 0 ? 0 : 1);
    }

    g_bgsave_pid = pid;
    g_bgsave_status = 1;
    g_bgsave_last_start_ms = start_ms;
    g_bgsave_base_dirty = snap_dirty;
    return 0;
}

int persist_bgsave_poll(void) {
    if (g_bgsave_pid <= 0) return 0;
    int status = 0;
    pid_t rc = waitpid(g_bgsave_pid, &status, WNOHANG);
    if (rc == 0) return 0;
    if (rc < 0) {
        g_bgsave_status = 3;
        g_bgsave_pid = -1;
        g_bgsave_last_end_ms = kvs_now_ms();
        return -1;
    }

    g_bgsave_last_end_ms = kvs_now_ms();
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        g_bgsave_status = 2;
        persist_mark_snapshot_success(g_bgsave_base_dirty);
    } else {
        g_bgsave_status = 3;
    }
    g_bgsave_pid = -1;
    return 1;
}

int persist_bgsave_in_progress(void) {
    return g_bgsave_pid > 0 ? 1 : 0;
}

const char *persist_bgsave_state_name(void) {
    switch (g_bgsave_status) {
        case 1: return "running";
        case 2: return "ok";
        case 3: return "err";
        default: return "idle";
    }
}

int persist_bgrewriteaof_start(void) {
    if (g_bgrewrite_pid > 0) return 1;

    if (persist_force_aof_flush() != 0 && g_aof_fd >= 0) return -1;

    snprintf(g_rewrite_tmp_path, sizeof(g_rewrite_tmp_path), "%s.rewrite.tmp.%ld", g_cfg.aof_path, (long)getpid());

    pthread_mutex_lock(&g_rewrite_buf_lock);
    free_rewrite_buffer_locked();
    pthread_mutex_unlock(&g_rewrite_buf_lock);

    pid_t pid = fork();
    if (pid < 0) {
        g_bgrewrite_status = 3;
        g_rewrite_tmp_path[0] = '\0';
        return -1;
    }
    if (pid == 0) {
        int rc = persist_write_aof_snapshot_to(g_rewrite_tmp_path);
        _exit(rc == 0 ? 0 : 1);
    }

    g_bgrewrite_pid = pid;
    g_bgrewrite_status = 1;
    return 0;
}

int persist_bgrewriteaof_poll(void) {
    if (g_bgrewrite_pid <= 0) return 0;

    int status = 0;
    pid_t rc = waitpid(g_bgrewrite_pid, &status, WNOHANG);
    if (rc == 0) return 0;
    if (rc < 0) {
        g_bgrewrite_status = 3;
        g_bgrewrite_pid = -1;
        unlink(g_rewrite_tmp_path);
        g_rewrite_tmp_path[0] = '\0';
        return -1;
    }

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        if (finalize_rewrite_parent() == 0) g_bgrewrite_status = 2;
        else {
            g_bgrewrite_status = 3;
            unlink(g_rewrite_tmp_path);
        }
    } else {
        g_bgrewrite_status = 3;
        unlink(g_rewrite_tmp_path);
    }

    g_bgrewrite_pid = -1;
    g_rewrite_tmp_path[0] = '\0';
    return 1;
}

int persist_bgrewriteaof_in_progress(void) {
    return g_bgrewrite_pid > 0 ? 1 : 0;
}

const char *persist_bgrewriteaof_state_name(void) {
    switch (g_bgrewrite_status) {
        case 1: return "running";
        case 2: return "ok";
        case 3: return "err";
        default: return "idle";
    }
}

void persist_note_write(void) {
    g_dirty_counter++;
}

unsigned long long persist_dirty_count(void) {
    return g_dirty_counter;
}

long long persist_last_snapshot_ms(void) {
    return g_last_snapshot_ms;
}

int persist_register_autosnap_rule(long long seconds, long long changes) {
    if (seconds <= 0 || changes <= 0) return -1;
    for (int i = 0; i < g_cfg.autosnap_rule_count; ++i) {
        if (g_cfg.autosnap_rules[i].seconds == seconds) {
            g_cfg.autosnap_rules[i].changes = changes;
            return 0;
        }
    }
    if (g_cfg.autosnap_rule_count >= KVS_AUTOSNAP_RULES_MAX) return -1;
    g_cfg.autosnap_rules[g_cfg.autosnap_rule_count].seconds = seconds;
    g_cfg.autosnap_rules[g_cfg.autosnap_rule_count].changes = changes;
    g_cfg.autosnap_rule_count++;
    return 0;
}

void persist_clear_autosnap_rules(void) {
    g_cfg.autosnap_rule_count = 0;
    memset(g_cfg.autosnap_rules, 0, sizeof(g_cfg.autosnap_rules));
}

int persist_build_autosnap_text(char *buf, size_t cap) {
    int n = snprintf(buf, cap,
        "autosnap_rules=%d\n"
        "dirty=%llu\n"
        "last_snapshot_ms=%lld\n"
        "bgsave=%s\n"
        "bgsave_pid=%ld\n"
        "aof_fsync=%s\n"
        "aof_rewrite=%s\n"
        "aof_rewrite_pid=%ld\n"
        "recover_total_ms=%lld\n"
        "recover_dump_ms=%lld\n"
        "recover_aof_ms=%lld\n"
        "recover_mmap_attempts=%llu\n"
        "recover_mmap_success=%llu\n"
        "recover_mmap_fallbacks=%llu\n"
        "recover_mmap_bytes=%llu\n"
        "recover_fread_bytes=%llu\n"
        "recover_tail_bytes=%llu\n",
        g_cfg.autosnap_rule_count,
        (unsigned long long)g_dirty_counter,
        g_last_snapshot_ms,
        persist_bgsave_state_name(),
        (long)g_bgsave_pid,
        persist_aof_policy_name(),
        persist_bgrewriteaof_state_name(),
        (long)g_bgrewrite_pid,
        g_recover_last_total_ms,
        g_recover_last_dump_ms,
        g_recover_last_aof_ms,
        g_recover_mmap_attempts,
        g_recover_mmap_success,
        g_recover_mmap_fallbacks,
        g_recover_last_mmap_bytes,
        g_recover_last_fread_bytes,
        g_recover_last_tail_bytes);
    if (n < 0 || (size_t)n >= cap) return -1;
    size_t pos = (size_t)n;
    for (int i = 0; i < g_cfg.autosnap_rule_count; ++i) {
        n = snprintf(buf + pos, cap - pos, "rule_%d=%lld:%lld\n", i,
            g_cfg.autosnap_rules[i].seconds, g_cfg.autosnap_rules[i].changes);
        if (n < 0 || (size_t)n >= cap - pos) return -1;
        pos += (size_t)n;
    }
    return (int)pos;
}

int persist_build_recover_text(char *buf, size_t cap) {
    int n = snprintf(buf, cap,
        "recover_total_ms=%lld\n"
        "recover_dump_ms=%lld\n"
        "recover_aof_ms=%lld\n"
        "recover_mmap_attempts=%llu\n"
        "recover_mmap_success=%llu\n"
        "recover_mmap_fallbacks=%llu\n"
        "recover_mmap_bytes=%llu\n"
        "recover_fread_bytes=%llu\n"
        "recover_tail_bytes=%llu\n",
        g_recover_last_total_ms,
        g_recover_last_dump_ms,
        g_recover_last_aof_ms,
        g_recover_mmap_attempts,
        g_recover_mmap_success,
        g_recover_mmap_fallbacks,
        g_recover_last_mmap_bytes,
        g_recover_last_fread_bytes,
        g_recover_last_tail_bytes);
    if (n < 0 || (size_t)n >= cap) return -1;
    return n;
}

int persist_autosnap_cron(void) {
    persist_bgsave_poll();
    persist_bgrewriteaof_poll();

    if (g_cfg.role != ROLE_MASTER) return 0;
    if (g_bgsave_pid > 0) return 0;
    if (g_cfg.autosnap_rule_count <= 0) return 0;

    long long now = kvs_now_ms();
    long long last_ms = g_last_snapshot_ms > 0 ? g_last_snapshot_ms : g_bgsave_last_end_ms;
    if (last_ms <= 0) last_ms = now;

    for (int i = 0; i < g_cfg.autosnap_rule_count; ++i) {
        long long sec = g_cfg.autosnap_rules[i].seconds;
        long long changes = g_cfg.autosnap_rules[i].changes;
        if ((long long)g_dirty_counter >= changes && now - last_ms >= sec * 1000) {
            return persist_bgsave_start();
        }
    }
    return 0;
}
