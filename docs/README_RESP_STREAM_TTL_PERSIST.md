# RESP stream + TTL + persistence

This bundle is a regenerated best-effort implementation of the requested merged version.

Included features:

- RESP array/bulk-string command parsing
- Stream parsing for half packets, sticky packets, and pipeline
- Per-connection write queue
- RESP protocol error recovery by resynchronizing to the next `*`
- TTL commands: `EXPIRE`, `TTL`, `PERSIST`, plus `R*` and `H*` variants
- Full persistence via `SAVE` to `kvstore.dump`
- Incremental persistence via append-only `kvstore.aof`
- Startup recovery by replaying `kvstore.dump` then `kvstore.aof`

Notes:

- The reactor path is the primary tested path in this regenerated bundle.
- Snapshot persistence is implemented as a full command image of the current dataset.
- Existing storage engines are reused from the original project files.

Build:

```bash
make
./kvstore 5000
```

Examples:

```bash
printf '*3\r\n$3\r\nSET\r\n$4\r\nname\r\n$5\r\nalice\r\n' | nc -q 1 127.0.0.1 5000
printf '*3\r\n$6\r\nEXPIRE\r\n$4\r\nname\r\n$1\r\n5\r\n' | nc -q 1 127.0.0.1 5000
printf '*1\r\n$4\r\nSAVE\r\n' | nc -q 1 127.0.0.1 5000
```
