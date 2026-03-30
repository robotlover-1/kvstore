# Multi-engine master/slave KV store

This bundle keeps the original three engines in parallel:

- array: `SET/GET/DEL/MOD/EXIST/EXPIRE/TTL/PERSIST`
- rbtree: `RSET/RGET/RDEL/RMOD/REXIST/REXPIRE/RTTL/RPERSIST`
- hash: `HSET/HGET/HDEL/HMOD/HEXIST/HEXPIRE/HTTL/HPERSIST`

It adds:

- single-master / multi-slave async replication
- full resync on `REPLSYNC`
- incremental replication using RESP write command stream
- TTL replication and persistence
- dump + AOF persistence and startup recovery

## Build

```bash
make
```

## Run master

```bash
./kvstore --port 5000 --role master
```

## Run slave

```bash
./kvstore --port 5001 --role slave --master-host 127.0.0.1 --master-port 5000
```

## Test write on A and read on B

```bash
printf '*3\r\n$3\r\nSET\r\n$4\r\nname\r\n$5\r\nalice\r\n' | nc -q 1 127.0.0.1 5000
printf '*2\r\n$3\r\nGET\r\n$4\r\nname\r\n' | nc -q 1 127.0.0.1 5001
```
