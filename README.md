# kvstore with memory backend switch

## memory backends
- `libc`: default system allocator
- `jemalloc`: runtime loads jemalloc via `dlopen`, no compile-time dependency required
- `custom`: self-built memory pool
  - small allocations (`<= 1024B`): slab-like fixed-size classes + freelist
  - large allocations (`> 1024B`): `mmap` direct allocation

## run
```bash
./kvstore --port 5000 --mem libc
./kvstore --port 5000 --mem jemalloc
./kvstore --port 5000 --mem custom
```

Or via env:
```bash
KVS_MEM_BACKEND=custom ./kvstore --port 5000
```

## notes
If `--mem jemalloc` is selected but the runtime library is absent, startup exits with an error.
