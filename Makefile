CC=gcc
CFLAGS=-Wall -Wextra -O2 -I./include -I./NtyCo/core -I./liburing/src/include
LDFLAGS=-lpthread -ldl -L./NtyCo -lntyco -L./liburing/src -luring -ldl

SRC_DIR=src
INC_DIR=include/kvstore

SRCS=$(SRC_DIR)/main/kvstore.c \
     $(SRC_DIR)/core/reactor.c \
     $(SRC_DIR)/storage/kvs_array.c \
     $(SRC_DIR)/storage/kvs_hash.c \
     $(SRC_DIR)/storage/kvs_rbtree.c \
     $(SRC_DIR)/memory/kvs_mem.c \
     $(SRC_DIR)/expire/kvs_expire.c \
     $(SRC_DIR)/persistence/kvs_persist.c \
     $(SRC_DIR)/replication/kvs_repl.c \
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

.PHONY: all clean build_dir
