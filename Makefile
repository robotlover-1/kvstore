CC=gcc
CFLAGS=-Wall -Wextra -O2
OBJS=kvstore.o reactor.o kvs_expire.o kvs_persist.o kvs_repl.o kvs_array.o kvs_hash.o kvs_rbtree.o

all: kvstore

kvstore: $(OBJS)
	$(CC) -o $@ $(OBJS) -lpthread

%.o: %.c kvstore.h
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) kvstore kvstore.dump kvstore.aof
