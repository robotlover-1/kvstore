CC=gcc
ENABLE_RDMA?=1
CFLAGS=-Wall -Wextra -O2 -I./include -I./NtyCo/core -I./liburing/src/include $(if $(filter 1,$(ENABLE_RDMA)),-DKVS_ENABLE_RDMA=1,-DKVS_ENABLE_RDMA=0)
LDFLAGS=-lpthread -ldl -L./NtyCo -lntyco -L./liburing/src -luring -ldl $(if $(filter 1,$(ENABLE_RDMA)),-lrdmacm -libverbs,)

SRC_DIR=src
INC_DIR=include/kvstore
TEST_HOST?=127.0.0.1
TEST_PORT?=5000
REPL_RDMA_SMOKE_HOST?=$(TEST_HOST)
REPL_MASTER_PORT?=6379
REPL_SLAVE_PORT?=6380
REPL_RECONNECT_WAIT?=3
REPL_RESTART_WAIT?=4
REPL_BENCH_MASTER_PORT?=5188
REPL_BENCH_SLAVE_PORT?=5189
REPL_BENCH_PRELOAD?=5000
REPL_BENCH_TAIL?=1000
REPL_PROFILE_MASTER_PORT?=5200
REPL_PROFILE_SLAVE_PORT?=5201
REPL_RDMA_UNSUPPORTED_MASTER_PORT?=5210
REPL_RDMA_UNSUPPORTED_SLAVE_PORT?=5211
REPL_RDMA_SMOKE_MASTER_PORT?=5220
REPL_RDMA_SMOKE_SLAVE_PORT?=5221
REPL_RDMA_STRESS_MASTER_PORT?=5230
REPL_RDMA_STRESS_SLAVE_PORT?=5231
REPL_RDMA_STRESS_PRELOAD?=128
REPL_RDMA_STRESS_TAIL_WRITES?=64
REPL_RDMA_STRESS_RESTART_ROUNDS?=3
REPL_RDMA_SOAK_SECONDS?=60
REPL_RDMA_SOAK_ALLOW_FAILURE?=0
REPL_RDMA_SOAK_RECONNECT_INTERVAL?=10
REPL_RDMA_SOAK_WRITE_INTERVAL_MS?=200
REPL_RDMA_TUNABLE_RECV_SLOTS?=0
REPL_RDMA_TUNABLE_CHUNK_SIZE?=0
REPL_RDMA_TUNABLE_QP_WR_DEPTH?=0
RDMA_PINGPONG_HOST?=$(REPL_RDMA_SMOKE_HOST)
RDMA_PINGPONG_PORT?=18515
RDMA_PINGPONG_DEV?=rxe0
RDMA_PINGPONG_IB_PORT?=1
RDMA_PINGPONG_GID_IDX?=1
MASS_TTL_KEYS?=10000
MASS_TTL_SECONDS?=2
MASS_TTL_BATCH?=1000
MASS_TTL_SAMPLE?=20
URING_PERSIST_COUNT?=1000
URING_PERSIST_PORT?=5140
URING_PERSIST_APPEND_FSYNC?=always
MMAP_RECOVER_COUNT?=2000
MMAP_RECOVER_PORT?=5142
MMAP_RECOVER_ENGINE?=hash
MMAP_RECOVER_APPEND_FSYNC?=everysec

SRCS=$(SRC_DIR)/main/kvstore.c \
     $(SRC_DIR)/core/reactor.c \
     $(SRC_DIR)/core/proactor.c \
     $(SRC_DIR)/core/ntyco.c \
     $(SRC_DIR)/storage/kvs_array.c \
     $(SRC_DIR)/storage/kvs_hash.c \
     $(SRC_DIR)/storage/kvs_rbtree.c \
     $(SRC_DIR)/storage/kvs_skiptable.c \
     $(SRC_DIR)/storage/kvs_doc.c \
     $(SRC_DIR)/memory/kvs_mem.c \
     $(SRC_DIR)/expire/kvs_expire.c \
     $(SRC_DIR)/persistence/kvs_persist.c \
     $(SRC_DIR)/replication/kvs_repl.c \
     $(SRC_DIR)/replication/kvs_sentinel.c \
     $(SRC_DIR)/utils/hash.c

OBJS=$(patsubst $(SRC_DIR)/%.c, build/%.o, $(SRCS))

all: build_dir kvstore

kvstore: $(OBJS)
	$(CC) -o $@ $(OBJS) $(LDFLAGS)

build/%.o: $(SRC_DIR)/%.c $(INC_DIR)/kvstore.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

build_dir:
	@mkdir -p build/main build/core build/storage build/memory build/expire build/persistence build/replication build/utils

clean:
	rm -rf build kvstore kvstore.dump kvstore.aof

check-resp:
	bash ./tests/integration/test_resp_nc_strict.sh $(TEST_HOST) $(TEST_PORT)

check-ttl:
	bash ./tests/integration/test_resp_ttl_nc.sh $(TEST_HOST) $(TEST_PORT)

check-persist:
	bash ./tools/persist/test_resp_persist_nc.sh $(TEST_HOST) $(TEST_PORT)

check-doc:
	bash ./tests/integration/test_doc_nc.sh $(TEST_HOST) $(TEST_PORT)

check-mass-ttl:
	python3 ./tools/persist/run_mass_ttl_validation.py --host $(TEST_HOST) --port $(TEST_PORT) --keys $(MASS_TTL_KEYS) --ttl $(MASS_TTL_SECONDS) --batch $(MASS_TTL_BATCH) --sample $(MASS_TTL_SAMPLE)

check-uring-persist:
	python3 ./tools/persist/run_uring_persist_bench.py --bin ./kvstore --host $(TEST_HOST) --port $(URING_PERSIST_PORT) --count $(URING_PERSIST_COUNT) --appendfsync $(URING_PERSIST_APPEND_FSYNC)

check-mmap-recover:
	python3 ./tools/persist/run_mmap_recover_bench.py --bin ./kvstore --host $(TEST_HOST) --port $(MMAP_RECOVER_PORT) --count $(MMAP_RECOVER_COUNT) --engine $(MMAP_RECOVER_ENGINE) --appendfsync $(MMAP_RECOVER_APPEND_FSYNC)

check-repl:
	MASTER_PORT=$(REPL_MASTER_PORT) SLAVE_PORT=$(REPL_SLAVE_PORT) RECONNECT_WAIT=$(REPL_RECONNECT_WAIT) RESTART_WAIT=$(REPL_RESTART_WAIT) bash ./tools/repl/run_repl_fullsync_test.sh

check-repl-metrics:
	python3 ./tools/repl/run_repl_metrics_bench.py --bin ./kvstore --host $(TEST_HOST) --master-port $(REPL_BENCH_MASTER_PORT) --slave-port $(REPL_BENCH_SLAVE_PORT) --preload-count $(REPL_BENCH_PRELOAD) --tail-count $(REPL_BENCH_TAIL)

check-repl-profile:
	python3 ./tools/repl/run_repl_profile.py --bin ./kvstore --host $(TEST_HOST) --master-port $(REPL_PROFILE_MASTER_PORT) --slave-port $(REPL_PROFILE_SLAVE_PORT) --preload-count $(REPL_BENCH_PRELOAD) --tail-count $(REPL_BENCH_TAIL)

check-repl-ebpf:
	python3 ./tools/repl/run_repl_profile.py --bin ./kvstore --host $(TEST_HOST) --master-port $(REPL_PROFILE_MASTER_PORT) --slave-port $(REPL_PROFILE_SLAVE_PORT) --preload-count $(REPL_BENCH_PRELOAD) --tail-count $(REPL_BENCH_TAIL) --ebpf

check-repl-ebpf-env:
	python3 ./tools/repl/run_repl_ebpf_env_probe.py

check-repl-rdma-unsupported:
	python3 ./tools/repl/run_repl_rdma_unsupported.py --bin ./kvstore --host $(TEST_HOST) --master-port $(REPL_RDMA_UNSUPPORTED_MASTER_PORT) --slave-port $(REPL_RDMA_UNSUPPORTED_SLAVE_PORT)

check-repl-rdma-smoke:
	ENABLE_RDMA=1 python3 ./tools/repl/run_repl_rdma_smoke.py --bin ./kvstore --host $(REPL_RDMA_SMOKE_HOST) --master-port $(REPL_RDMA_SMOKE_MASTER_PORT) --slave-port $(REPL_RDMA_SMOKE_SLAVE_PORT)

check-repl-rdma-stress:
	ENABLE_RDMA=1 python3 ./tools/repl/run_repl_rdma_stress.py --bin ./kvstore --host $(REPL_RDMA_SMOKE_HOST) --master-port $(REPL_RDMA_STRESS_MASTER_PORT) --slave-port $(REPL_RDMA_STRESS_SLAVE_PORT) --preload $(REPL_RDMA_STRESS_PRELOAD) --tail-writes $(REPL_RDMA_STRESS_TAIL_WRITES) --restart-rounds $(REPL_RDMA_STRESS_RESTART_ROUNDS) --rdma-recv-slots $(REPL_RDMA_TUNABLE_RECV_SLOTS) --rdma-chunk-size $(REPL_RDMA_TUNABLE_CHUNK_SIZE) --rdma-qp-wr-depth $(REPL_RDMA_TUNABLE_QP_WR_DEPTH)

check-repl-rdma-soak:
	ENABLE_RDMA=1 python3 ./tools/repl/run_repl_rdma_stress.py --bin ./kvstore --host $(REPL_RDMA_SMOKE_HOST) --master-port $(REPL_RDMA_STRESS_MASTER_PORT) --slave-port $(REPL_RDMA_STRESS_SLAVE_PORT) --preload $(REPL_RDMA_STRESS_PRELOAD) --tail-writes $(REPL_RDMA_STRESS_TAIL_WRITES) --restart-rounds $(REPL_RDMA_STRESS_RESTART_ROUNDS) --soak-seconds $(REPL_RDMA_SOAK_SECONDS) --soak-reconnect-interval $(REPL_RDMA_SOAK_RECONNECT_INTERVAL) --soak-write-interval-ms $(REPL_RDMA_SOAK_WRITE_INTERVAL_MS) --rdma-recv-slots $(REPL_RDMA_TUNABLE_RECV_SLOTS) --rdma-chunk-size $(REPL_RDMA_TUNABLE_CHUNK_SIZE) --rdma-qp-wr-depth $(REPL_RDMA_TUNABLE_QP_WR_DEPTH) $(if $(filter 1,$(REPL_RDMA_SOAK_ALLOW_FAILURE)),--allow-soak-failure,)

check-repl-rdma-soak-skip:
	@echo "Skipping RDMA soak check"

check-rdma-standalone-probe:
	python3 ./tools/rdma/run_rdma_standalone_probe.py

check-rdma-pingpong-smoke:
	python3 ./tools/rdma/run_rdma_pingpong_smoke.py --host $(RDMA_PINGPONG_HOST) --port $(RDMA_PINGPONG_PORT) --ib-dev $(RDMA_PINGPONG_DEV) --ib-port $(RDMA_PINGPONG_IB_PORT) --gid-idx $(RDMA_PINGPONG_GID_IDX)

check: check-resp check-ttl check-persist check-doc

.PHONY: all clean build_dir check check-resp check-ttl check-persist check-doc check-mass-ttl check-uring-persist check-mmap-recover check-repl check-repl-metrics check-repl-profile check-repl-ebpf check-repl-ebpf-env check-repl-rdma-unsupported check-repl-rdma-smoke check-repl-rdma-stress check-repl-rdma-soak check-repl-rdma-soak-skip check-rdma-standalone-probe check-rdma-pingpong-smoke
