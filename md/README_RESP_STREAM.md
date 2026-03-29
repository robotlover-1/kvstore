# RESP stream parser with per-connection write queue and error recovery

This package extends the earlier RESP stream-state-machine version in two ways:

- Adds a **per-connection write queue** so each response is buffered as an independent output node.
  - Better for pipeline workloads.
  - Handles partial sends cleanly.
  - Avoids relying on one monolithic output buffer.
- Adds **RESP protocol error recovery**.
  - On malformed input, the server replies with `-ERR ...\r\n`.
  - The connection stays open.
  - The parser resets its current request and tries to resynchronize to the next `*` request boundary.

## What changed

- `kvs_stream_t` now owns:
  - input buffer + parse state
  - current request assembly state
  - linked-list output queue (`kvs_out_node_t`)
- `kvs_stream_feed()` no longer treats protocol errors as fatal.
- `reactor.c` sends from the queue head and pops nodes as bytes are written.
- `close_conn()` frees queued output and request state.

## Supported behaviors

- fragmented requests (half packets)
- coalesced requests (sticky packets)
- request pipelining
- protocol error recovery on malformed RESP frames

## Build

```bash
gcc -O2 -Wall -Wextra -o kvstore \
  kvstore.c reactor.c kvs_array.c kvs_hash.c kvs_rbtree.c
```

## Quick tests

```bash
./kvstore 5000
printf '*3\r\n$3\r\nSET\r\n$4\r\nname\r\n$5\r\nalice\r\n' | nc 127.0.0.1 5000
printf '*2\r\n$3\r\nGET\r\n$4\r\nname\r\n' | nc 127.0.0.1 5000
```

## Error recovery example

Send a malformed request followed by a valid request on the same connection:

```bash
exec 3<>/dev/tcp/127.0.0.1/5000
printf '*2\r\n$3\r\nGET\r\n$X\r\n' >&3
sleep 1
printf '*2\r\n$3\r\nGET\r\n$4\r\nname\r\n' >&3
cat <&3
```

Expected behavior:

- first reply: `-ERR invalid bulk length`
- second reply: normal GET response
