# RESP stream + write queue + error recovery + TTL

This package merges the RESP streaming state machine version with TTL support.

Included features:
- RESP array + bulk string parser
- half-packet / sticky-packet / pipeline support
- per-connection write queue
- RESP protocol error recovery
- TTL commands on all three engines
- lazy expiration on read/write checks
- active expiration cycle in the reactor loop

Supported commands:
- Array engine: `SET GET DEL MOD EXIST EXPIRE TTL PERSIST`
- RBTree engine: `RSET RGET RDEL RMOD REXIST REXPIRE RTTL RPERSIST`
- Hash engine: `HSET HGET HDEL HMOD HEXIST HEXPIRE HTTL HPERSIST`

TTL semantics:
- `EXPIRE key seconds` => `:1` on success, `:0` if key does not exist
- `TTL key` => `:-2` if key does not exist, `:-1` if no expire is set, or remaining seconds
- `PERSIST key` => `:1` if expire removed, `:0` if key does not exist or no expire was set

Notes:
- The reactor path is the main tested path in this package.
- `epoll_wait` uses a 100 ms timeout so active expiration can run even when the server is idle.
