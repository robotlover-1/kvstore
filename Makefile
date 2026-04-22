CC=gcc
CFLAGS=-Wall -Wextra -O2 -I./include -I./NtyCo/core -I./liburing/src/include
LDFLAGS=-lpthread -ldl -L./NtyCo -lntyco -L./liburing/src -luring -ldl

SRC_DIR=src
INC_DIR=include/kvstore
TEST_HOST?=127.0.0.1
TEST_PORT?=5000
REPL_MASTER_PORT?=6379
REPL_SLAVE_PORT?=6380
MASS_TTL_KEYS?=10000
MASS_TTL_SECONDS?=2
MASS_TTL_BATCH?=1000
MASS_TTL_SAMPLE?=20

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
	bash ./scripts/test_resp_persist_nc.sh $(TEST_HOST) $(TEST_PORT)

check-doc:
	bash ./tests/integration/test_doc_nc.sh $(TEST_HOST) $(TEST_PORT)

check-mass-ttl:
	python3 ./scripts/run_mass_ttl_validation.py --host $(TEST_HOST) --port $(TEST_PORT) --keys $(MASS_TTL_KEYS) --ttl $(MASS_TTL_SECONDS) --batch $(MASS_TTL_BATCH) --sample $(MASS_TTL_SAMPLE)

check-repl:
	MASTER_PORT=$(REPL_MASTER_PORT) SLAVE_PORT=$(REPL_SLAVE_PORT) bash ./scripts/run_repl_fullsync_test.sh

check: check-resp check-ttl check-persist check-doc

.PHONY: all clean build_dir check check-resp check-ttl check-persist check-doc check-mass-ttl check-repl
