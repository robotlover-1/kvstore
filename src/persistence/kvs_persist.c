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

static long long g_aof_write_offset = 0;
static long long g_aof_write_submitted = 0;

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

static int g_bgsave_status = 0;
static unsigned long long g_bgsave_base_dirty = 0;
static long long g_last_snapshot_ms = 0;

static int g_aof_disabled = 0;

int persist_aof_disable(void) {
    g_aof_disabled = 1;
    return 0;
}

static pid_t g_bgrewrite_pid = -1;
static int g_bgrewrite_status = 0;
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
 * Fixed-size slot ring.  Each in-flight AOF command owns one slot.
 * Both SQEs (write + fsync) carry the slot pointer as user_data,
 * so CQEs can arrive in any order and still match the correct slot.
 *
 * The slot holds a safe copy of the AOF payload — the async write
 * never references connection input-buffer memory.
 */
#define PERSIST_INFLIGHT_SIZE 1024
#define MAX_AOF_INFLIGHT      480

typedef struct persist_slot_s {
    conn_t        *conn;
    unsigned char *resp;
    size_t         resp_len;
    unsigned char *aof_buf;
    size_t         aof_len;
    int            cqe_seen;
    int            cqe_ok;
    int            last_error;
    int            in_use;
    int            is_batch;
} persist_slot_t;

/* ---- batch pipeline state ----
 *
 * During parse_resp_stream(), AOF-always commands accumulate their payload
 * into a single batch buffer.  persist_flush_batch() submits ONE write+ONE
 * fsync per epoll cycle.  Responses are sent immediately (KVS_PERSIST_OK),
 * matching Redis AOF-always semantics.
 */
#define PERSIST_BATCH_INIT_CAP  (64 * 1024)    /* 64 KB initial */
#define PERSIST_BATCH_MAX_CAP   (1024 * 1024)  /* 1 MB hard limit */

static unsigned char *g_batch_buf  = NULL;
static size_t         g_batch_len  = 0;
static size_t         g_batch_cap  = 0;

static int batch_ensure_cap(size_t needed) {
    if (g_batch_cap >= needed) return 0;
    size_t new_cap = g_batch_cap ? g_batch_cap : PERSIST_BATCH_INIT_CAP;
    while (new_cap < needed) {
        new_cap *= 2;
        if (new_cap > PERSIST_BATCH_MAX_CAP) {
            new_cap = PERSIST_BATCH_MAX_CAP;
            break;
        }
    }
    if (new_cap < needed) return -1;
    unsigned char *p = (unsigned char *)kvs_realloc(g_batch_buf, new_cap);
    if (!p) return -1;
    g_batch_buf = p;
    g_batch_cap = new_cap;
    return 0;
}

static persist_slot_t g_inflight[PERSIST_INFLIGHT_SIZE];
static int g_inflight_count = 0;
static int g_inflight_head = 0;

static int g_persist_eventfd = -1;
static int g_persist_fatal_error = 0;
static int g_persist_aof_registered = 0;

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
    p.flags = IORING_SETUP_SINGLE_ISSUER
            | IORING_SETUP_COOP_TASKRUN
            | IORING_SETUP_SQPOLL;
    p.sq_thread_idle = 2000;

    if (io_uring_queue_init_params(1024, &g_persist_uring, &p) != 0) {
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

/* ---- submit ---- */

void persist_submit_sqes(void) {
    if (!g_persist_uring_ready) return;
    io_uring_submit(&g_persist_uring);
}

/* ---- batch flush (one write + one fsync + one submit) ---- */

void persist_flush_batch(void) {
    struct io_uring_sqe *sqe_w, *sqe_f;
    persist_slot_t *slot;

    if (!g_persist_uring_ready) return;
    if (g_batch_len == 0) return;

    /* reap to free inflight slots */
    persist_reap_completions();

    if (g_inflight_count >= MAX_AOF_INFLIGHT) {
        persist_reap_completions();
    }

    /* ensure SQ space for 2 SQEs */
    for (int sq_retry = 0; io_uring_sq_space_left(&g_persist_uring) < 2; sq_retry++) {
        persist_submit_sqes();
        persist_reap_completions();
        if (sq_retry >= 100) {
            fprintf(stderr, "persist_flush_batch: no SQ space after retries\n");
            return;
        }
    }

    slot = persist_inflight_reserve();
    if (!slot) {
        fprintf(stderr, "persist_flush_batch: no inflight slot\n");
        return;
    }

    /* transfer batch buffer ownership to slot */
    slot->aof_buf  = g_batch_buf;
    slot->aof_len  = g_batch_len;
    slot->conn     = NULL;         /* batch marker */
    slot->resp     = NULL;
    slot->resp_len = 0;
    slot->is_batch = 1;

    off_t off = (off_t)(g_aof_write_submitted - (long long)g_batch_len);

    sqe_w = io_uring_get_sqe(&g_persist_uring);
    if (!sqe_w) {
        persist_inflight_release(slot);
        return;
    }
    io_uring_prep_write(sqe_w,
                        g_persist_aof_registered ? 0 : g_aof_fd,
                        slot->aof_buf, slot->aof_len, off);
    sqe_w->flags |= IOSQE_IO_LINK;
    if (g_persist_aof_registered) sqe_w->flags |= IOSQE_FIXED_FILE;
    io_uring_sqe_set_data(sqe_w, slot);

    sqe_f = io_uring_get_sqe(&g_persist_uring);
    if (!sqe_f) {
        persist_inflight_release(slot);
        return;
    }
    io_uring_prep_fsync(sqe_f,
                        g_persist_aof_registered ? 0 : g_aof_fd,
                        IORING_FSYNC_DATASYNC);
    if (g_persist_aof_registered) sqe_f->flags |= IOSQE_FIXED_FILE;
    io_uring_sqe_set_data(sqe_f, slot);

    io_uring_submit(&g_persist_uring);

    /* reset accumulation state — batch buffer ownership transferred to slot */
    g_batch_buf  = NULL;
    g_batch_len  = 0;
    g_batch_cap  = 0;
}

/* ---- reap completions ---- */

void persist_reap_completions(void) {
    if (!g_persist_uring_ready) return;

    struct io_uring_cqe *cqe;
    while (io_uring_peek_cqe(&g_persist_uring, &cqe) == 0) {
        persist_slot_t *s = (persist_slot_t *)io_uring_cqe_get_data(cqe);
        if (!s) {
            fprintf(stderr, "persist: CQE without slot\n");
            io_uring_cqe_seen(&g_persist_uring, cqe);
            continue;
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

        if (s->cqe_seen == 2) {
            if (s->cqe_ok == 2) {
                if (!s->is_batch) {
                    /* per-command slot (legacy / non-batch paths) */
                    if (s->conn && s->conn->fd > 0)
                        queue_bytes(s->conn, s->resp, s->resp_len);
                }
                /* batch slot: no deferred responses, just free buffer */
            } else {
                fprintf(stderr, "persist: CQE error cqe_ok=%d last_error=%d\n",
                        s->cqe_ok, s->last_error);
                if (!s->is_batch && s->conn) {
                    s->conn->fd = -1;
                }
                g_persist_fatal_error = 1;
            }
            persist_inflight_release(s);
        }
    }
}

int persist_uring_fd(void) {
    return g_persist_eventfd;
}

void persist_drain_pending(void) {
    if (!g_persist_uring_ready) return;
    persist_flush_batch();
    while (g_inflight_count > 0) {
        io_uring_submit_and_wait(&g_persist_uring, 1);
        persist_reap_completions();
    }
}

/* ---- synchronous uring helpers ---- */

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

/* ---- recovery (mmap / fread) ---- */

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

    if (pos + sizeof(aof_offset) > size) goto out;
    memcpy(&aof_offset, mapped + pos, sizeof(aof_offset));
    pos += sizeof(aof_offset);

    while (pos + 2 + 4 <= size) {
        uint8_t engine_id, flags;
        uint32_t klen, vlen;
        char *key, *value;

        engine_id = mapped[pos++];
        if (engine_id < 1 || engine_id > 5) {
            fprintf(stderr, "replay_dump_file: invalid engine_id %u at pos %zu (old format KVSD file?)\n",
                (unsigned int)engine_id, pos - 1);
            break;
        }

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

/* ---- snapshot / bgrewrite ---- */

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

/* ---- batch-accumulate write commands ----
 *
 * Append the AOF payload into an accumulating batch buffer.
 * Returns KVS_PERSIST_OK immediately — the caller sends the response
 * without waiting for fsync (like Redis AOF always semantics).
 * The actual write+fsync+submit happens once per epoll cycle
 * in persist_flush_batch().
 */

int persist_append_prepare(conn_t *c, const unsigned char *buf, size_t len,
                           unsigned char *resp, size_t resp_len) {
    (void)c;
    (void)resp;
    (void)resp_len;

    if (g_aof_fd < 0) return g_aof_disabled ? KVS_PERSIST_OK : KVS_PERSIST_ERR;
    if (g_cfg.aof_fsync != KVS_AOF_FSYNC_ALWAYS) return KVS_PERSIST_OK;
    if (g_persist_fatal_error) return KVS_PERSIST_ERR;
    if (persist_uring_init_once() != 0) return KVS_PERSIST_ERR;

    /* reap any completed batches — makes inflight slots available */
    persist_reap_completions();

    /* flush if batch buffer is full */
    if (g_batch_len + len > g_batch_cap && g_batch_cap >= PERSIST_BATCH_MAX_CAP) {
        persist_flush_batch();
    }

    if (batch_ensure_cap(g_batch_len + len) != 0) return KVS_PERSIST_ERR;

    memcpy(g_batch_buf + g_batch_len, buf, len);
    g_batch_len += len;

    g_aof_write_submitted += (long long)len;

    if (g_bgrewrite_pid > 0) append_to_rewrite_buffer(buf, len);

    return KVS_PERSIST_OK;
}

int persist_append_raw(const unsigned char *buf, size_t len) {
    return persist_append_prepare(NULL, buf, len, NULL, 0);
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
