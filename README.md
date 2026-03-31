# kvstore memory backend + MEMSTAT

这版在 `libc / jemalloc / custom` 三种内存后端基础上，新增了 `MEMSTAT` 命令和压测脚本。

## 1. 编译

```bash
make clean && make
```

## 2. 启动

```bash
./kvstore --port 5000 --mem libc
./kvstore --port 5000 --mem jemalloc
./kvstore --port 5000 --mem custom
```

如果 `jemalloc` 模式所在系统存在 static TLS block 问题，这版会自动通过 `LD_PRELOAD` 自重启进程，避免启动阶段 `dlopen` 失败。

## 3. INFO

```bash
printf '*1\r\n$4\r\nINFO\r\n' | nc 127.0.0.1 5000
```

返回示例：

```text
$24
role:master mem:custom
```

## 4. MEMSTAT

```bash
printf '*1\r\n$7\r\nMEMSTAT\r\n' | nc 127.0.0.1 5000
```

返回的是 bulk string，里面是多行 `key=value`：

```text
backend=custom
backend_id=2
initialized=1
alloc_calls=...
calloc_calls=...
realloc_calls=...
free_calls=...
small_max_size=1024
small_alloc_calls=...
small_free_calls=...
current_small_inuse=...
peak_small_inuse=...
total_small_page_bytes=...
large_alloc_calls=...
large_free_calls=...
current_large_inuse_bytes=...
peak_large_inuse_bytes=...
total_large_map_bytes=...
active_large_map_bytes=...
peak_active_large_map_bytes=...
class_0_size=32
class_0_pages=...
class_0_total_chunks=...
class_0_free_chunks=...
class_0_page_bytes=...
...
```

### custom 后端统计含义

- 小内存：`<= 1024B`
  - 走 size class + slab page + freelist
  - 统计重点：`current_small_inuse`、`peak_small_inuse`、`total_small_page_bytes`
  - 每个 class 还会输出 `size/pages/total_chunks/free_chunks/page_bytes`
- 大内存：`> 1024B`
  - 走 `mmap`
  - 统计重点：`current_large_inuse_bytes`、`peak_large_inuse_bytes`
  - 映射维度：`active_large_map_bytes`、`peak_active_large_map_bytes`、`total_large_map_bytes`

对于 `libc/jemalloc`，`MEMSTAT` 也能返回统一计数器；但 class/slab/mmap 这些细分统计主要对 `custom` 有意义。

## 5. 基准测试代码

项目提供了多个基准测试脚本，位于 `benchmarks/scripts/` 目录中：

- `bench_mem_backend.py` - 基本基准测试，覆盖三种内存后端
- `bench_mem_backend_append.py` - 追加模式基准测试，支持累积 CSV
- `bench_mem_backend_fixed.py` - 固定配置基准测试
- `plot_bench_grouped.py` - 生成分组图表
- `plot_bench_results.py` - 生成基准测试结果图表

这些脚本会逐个启动：

- `libc`
- `jemalloc`
- `custom`

然后完成两类对比：

1. QPS
   - 对每个后端执行同样数量的 `SET` (或 `HSET`/`RSET`)
   - 统计 `elapsed_sec` 和 `qps`
2. 同样数据量下的虚拟内存 / 物理内存
   - 从 `/proc/<pid>/status` 采样 `VmSize / VmRSS / VmData`
   - 输出 `mem_gap_kb = VmSize - VmRSS`

### 用法

从项目根目录运行：

```bash
cd benchmarks/scripts
python3 bench_mem_backend.py --ops 50000 --value-size 128
```

或使用提供的包装脚本：

```bash
./run_benchmark.sh bench_mem_backend.py --ops 50000 --value-size 128
```

也可以改参数：

```bash
python3 bench_mem_backend.py --ops 100000 --value-size 256 --warmup 1000 --csv my_bench.csv
```

输出示例：

```text
libc: qps=81234.11 vmrss_kb=8420 vmsize_kb=15640 mem_gap_kb=7220
jemalloc: qps=90120.55 vmrss_kb=8348 vmsize_kb=17012 mem_gap_kb=8664
custom: qps=87654.77 vmrss_kb=7904 vmsize_kb=14536 mem_gap_kb=6632
wrote /home/pp/Desktop/ls_study/proj/9.1-kvstore/benchmarks/data/bench_results_all.csv
```

**文件存放位置**：
- CSV 结果文件默认保存在 `benchmarks/data/` 目录
- 图表文件保存在 `benchmarks/plots/` 目录
- 临时 dump/aof 文件生成在脚本目录并自动清理

CSV 会包含：

- `backend`
- `ops`
- `elapsed_sec`
- `qps`
- `vmrss_kb`
- `vmsize_kb`
- `vmdata_kb`
- `mem_gap_kb`
- `info`
- `memstat_backend`
- `current_small_inuse`
- `peak_small_inuse`
- `total_small_page_bytes`
- `current_large_inuse_bytes`
- `peak_large_inuse_bytes`
- `active_large_map_bytes`
- `peak_active_large_map_bytes`
- `large_alloc_calls`
- `small_alloc_calls`

## 6. 推荐测试方法

### 小 value，观察小块池效果

```bash
python3 bench_mem_backend.py --ops 50000 --value-size 64
```

### 大 value，观察 mmap 大块效果

```bash
python3 bench_mem_backend.py --ops 20000 --value-size 4096
```

### 混合场景

可多跑几次，例如：

```bash
python3 bench_mem_backend.py --ops 30000 --value-size 128 --csv bench_small.csv
python3 bench_mem_backend.py --ops 30000 --value-size 4096 --csv bench_large.csv
```

这样可以同时比较：

- 小对象场景下 custom slab 的命中效果
- 大对象场景下 custom `mmap` 的虚拟内存增长情况
- jemalloc 与 libc 的 QPS 和 RSS/VSZ 差异
