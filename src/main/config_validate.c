#include "kvstore/kvstore.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

static int config_error(char *err, size_t cap, const char *fmt, ...) {
    va_list ap;
    if (!err || cap == 0 || !fmt) return -1;
    va_start(ap, fmt);
    vsnprintf(err, cap, fmt, ap);
    va_end(ap);
    return -1;
}

static int validate_port(int port) {
    return port > 0 && port <= 65535;
}

static int validate_choice(const char *value, const char *const *choices) {
    if (!value || !*value || !choices) return 0;
    for (int i = 0; choices[i]; ++i) {
        if (!strcasecmp(value, choices[i])) return 1;
    }
    return 0;
}

int kvs_validate_config(char *err, size_t cap) {
    static const char *const mem_backends[] = {"libc", "jemalloc", "custom", NULL};
    static const char *const net_backends[] = {"reactor", "proactor", "ntyco", NULL};
    static const char *const persist_modes[] = {"none", "snapshot", "aof", "hybrid", NULL};

    if (!g_cfg.bind_ip[0]) {
        return config_error(err, cap, "bind_ip must not be empty");
    }
    if (!validate_port(g_cfg.port)) {
        return config_error(err, cap, "port must be between 1 and 65535");
    }
    if (g_cfg.role != ROLE_MASTER && g_cfg.role != ROLE_SLAVE) {
        return config_error(err, cap, "role must be master or slave");
    }
    if (!validate_choice(g_cfg.mem_backend, mem_backends)) {
        return config_error(err, cap, "mem_backend must be one of: libc, jemalloc, custom");
    }
    if (!validate_choice(g_cfg.net_backend, net_backends)) {
        return config_error(err, cap, "net_backend must be one of: reactor, proactor, ntyco");
    }
    if (!validate_choice(g_cfg.persist_mode, persist_modes)) {
        return config_error(err, cap, "persist_mode must be one of: none, snapshot, aof, hybrid");
    }
    if (g_cfg.aof_fsync != KVS_AOF_FSYNC_ALWAYS && g_cfg.aof_fsync != KVS_AOF_FSYNC_EVERYSEC) {
        return config_error(err, cap, "appendfsync must be always or everysec");
    }
    if (g_cfg.autosnap_rule_count < 0 || g_cfg.autosnap_rule_count > KVS_AUTOSNAP_RULES_MAX) {
        return config_error(err, cap, "autosnap rule count exceeds supported range");
    }

    if (g_cfg.role == ROLE_SLAVE) {
        if (!g_cfg.master_host[0]) {
            return config_error(err, cap, "master_host is required when role=slave");
        }
        if (!validate_port(g_cfg.master_port)) {
            return config_error(err, cap, "master_port must be between 1 and 65535 when role=slave");
        }
    }

    if (g_cfg.is_sentinel) {
        if (!g_cfg.sentinel_master_name[0]) {
            return config_error(err, cap, "sentinel_master_name must not be empty when sentinel=true");
        }
        if (!g_cfg.sentinel_monitor_host[0]) {
            return config_error(err, cap, "sentinel_monitor_host must not be empty when sentinel=true");
        }
        if (!validate_port(g_cfg.sentinel_monitor_port)) {
            return config_error(err, cap, "sentinel_monitor_port must be between 1 and 65535 when sentinel=true");
        }
        if (g_cfg.sentinel_down_after_ms <= 0) {
            return config_error(err, cap, "sentinel_down_after_ms must be greater than 0 when sentinel=true");
        }
        if (g_cfg.sentinel_failover_timeout_ms <= 0) {
            return config_error(err, cap, "sentinel_failover_timeout_ms must be greater than 0 when sentinel=true");
        }
        if (g_cfg.sentinel_quorum <= 0) {
            return config_error(err, cap, "sentinel_quorum must be greater than 0 when sentinel=true");
        }
    }

    if (err && cap > 0) err[0] = '\0';
    return 0;
}
