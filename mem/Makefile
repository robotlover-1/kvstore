CC = gcc
CFLAGS = -Wall -Wextra -O2
LDFLAGS = -ldl

all: mempool_demo

mempool_demo: mempool_demo.o kvs_memory.o
	$(CC) -o $@ $^ $(LDFLAGS)

mempool_demo.o: mempool_demo.c kvs_memory.h
	$(CC) $(CFLAGS) -c mempool_demo.c -o mempool_demo.o

kvs_memory.o: kvs_memory.c kvs_memory.h
	$(CC) $(CFLAGS) -c kvs_memory.c -o kvs_memory.o

clean:
	rm -f mempool_demo *.o
