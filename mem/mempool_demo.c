#include <stdio.h>
#include <string.h>
#include "kvs_memory.h"

int main(int argc, char **argv) {
    kvs_mem_mode_t mode = KVS_MEM_EXISTING;
    kvs_mem_stats_t st;

    if (argc >= 2) {
        if (kvs_mem_parse_mode(argv[1], &mode) != 0) {
            fprintf(stderr, "invalid mode: %s\n", argv[1]);
            return 1;
        }
    }

    if (kvs_mem_init(mode) != 0) {
        fprintf(stderr, "failed to init mode: %s\n", kvs_mem_mode_name(mode));
        return 2;
    }

    char *a = (char *)kvs_malloc(8);
    char *b = (char *)kvs_malloc(64);
    char *c = (char *)kvs_malloc(300);
    if (!a || !b || !c) {
        fprintf(stderr, "allocation failed\n");
        return 3;
    }

    memcpy(a, "abc", 4);
    memset(b, 'x', 63);
    b[63] = '\0';
    memset(c, 'y', 299);
    c[299] = '\0';

    printf("mode=%s a=%s b_len=%zu c_len=%zu\n",
           kvs_mem_mode_name(kvs_mem_get_mode()), a, strlen(b), strlen(c));

    kvs_free(a);
    kvs_free(b);
    kvs_free(c);

    kvs_mem_get_stats(&st);
    printf("alloc_calls=%llu free_calls=%llu bytes_in_use=%llu bytes_peak=%llu small_pool=%llu large_pool=%llu jemalloc_allocs=%llu\n",
           (unsigned long long)st.alloc_calls,
           (unsigned long long)st.free_calls,
           (unsigned long long)st.bytes_in_use,
           (unsigned long long)st.bytes_peak,
           (unsigned long long)st.small_pool_allocs,
           (unsigned long long)st.large_pool_allocs,
           (unsigned long long)st.jemalloc_allocs);

    kvs_mem_fini();
    return 0;
}
