CC=gcc
CFLAGS=-Wall -Wextra -O2
OBJS=kvstore.o reactor.o kvs_expire.o kvs_persist.o kvs_array.o kvs_hash.o kvs_rbtree.o

all: kvstore

kvstore: $(OBJS)
	$(CC) -o $@ $(OBJS)

%.o: %.c kvstore.h server.h
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f *.o kvstore kvstore.dump kvstore.aof
