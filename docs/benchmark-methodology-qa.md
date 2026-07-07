# 性能基准测试方法 Q&A

> 从对话中提取的性能测试相关问题与回答，覆盖 AOF、Pipeline、SAVE、内存后端、kprobe 转发、RDMA/sendfile/iperf 等测试方法。

---

## 一、AOF 持久化性能对比

### Q1: README 中 KVSTORE 与 Redis 不同 AOF 策略对 QPS 的影响是怎么测试的？

**测试脚本**：`tools/bench/run_persist_bench.sh`

**测试工具**：`redis-benchmark -n 1000000 -c 50 -P 1 -d 64 -r 1000000`

- kvstore 用 `HSET key:__rand_int__ value`（2-arg，走 Hash 引擎）
- Redis 用 `HSET key:__rand_int__ __rand_int__ value`（3-arg，标准 hash field 操作）
- 不用 SET 的原因：kvstore 的 SET 走 ARRAY 引擎（`KVS_ARRAY_SIZE=1024`），10 万随机 key 下很快存满，99% 返回错误不写 AOF，QPS 无效

**流程**：分 8 个配置各测一次（kvstore echo、redis echo、kvstore AOF 关闭/always/everysec、redis 无AOF/always/everysec），每个配置前重启服务器清理 AOF/dump。

**测试环境**：Intel Core Ultra 7 155H (4 vCPU) / 7.7GiB RAM / Ubuntu 20.04.6 / KVM 虚拟机，非 sudo，对比版本 Redis 5.0.7。

**关键结果**（2026-06-26 重测）：

| 配置 | ECHO (QPS) | HSET (QPS) | vs baseline |
|------|-----------|-----------|------------|
| kvstore AOF 关闭 | — | 132,926 | baseline |
| kvstore AOF always | — | 63,504 | 48% |
| kvstore AOF everysec | — | 129,316 | 97% |
| Redis AOF always | — | 44,439 | 34% |

---

## 二、Pipeline 批量性能

### Q2: PIPELINE 批量怎么测试的？

**测试脚本**：`tools/bench/run_pipeline_bench.sh`

**测试工具**：`redis-benchmark -n 1000000 -c 50 -P <N> -d 64 -r 1000000`

- Pipeline 深度：10, 20, 40, 80, 160
- 每种深度测 8 个配置：kvstore echo、redis echo、kvstore AOF disable/everysec/always、redis 无AOF/everysec/always
- 共 5 深度 × 8 配置 = 40 个测试点
- `redis-benchmark` 报告的 QPS 是**批次数/秒**，实际命令吞吐 = `QPS × P`

**测试环境**：同上，非 sudo 运行避免 kprobe 开销。

**关键结果**（2026-06-26 重测，HSET AOF 关闭）：

| P 深度 | kvstore (QPS) | Redis (QPS) | kv/redis |
|--------|-------------|------------|----------|
| 1 | 135,483 | 132,538 | 101% |
| 160 | 986,193 | 967,118 | 102% |

**注意**：kvstore 在 inline response 优化后，AOF always pipeline 全面追平 Redis，P=160 时 695K（超 Redis 11%）。

---

## 三、SAVE 性能

### Q3: SAVE 性能怎么测试的？

**测试脚本**：`tools/bench/run_persist_bench.sh` 的 Phase 3

**测试方法**：

1. 使用 Hash 引擎（`HSET key value`），避免 Array 引擎 1024 上限
2. 四种数据规模：100w（SAVE×1）、10w（SAVE×10 取平均）、1w（SAVE×100）、1k（SAVE×1000）
3. 写入用 `redis-benchmark`，SAVE 耗时用 `date +%s%N` 纳秒计时（每次 `redis-cli SAVE` 前后计时，带 3 次重试）
4. 指标：写入 QPS、写入耗时、平均每次 SAVE 耗时、**有效 QPS**（`数据量 / (写入时间 + SAVE 总耗时)`）

**关键结果**（非 sudo，AOF 关闭）：

| 场景 | 数据量 | 写入 QPS | 写入耗时 | 平均每次 SAVE | 有效 QPS |
|------|--------|---------|---------|-------------|---------|
| 100w→SAVE×1 | 100万 | 132,820 | 7.588s | 3,854ms | 87,398 |
| 10w→SAVE×10 | 10万 | 132,100 | 0.766s | 383.1ms | 21,751 |

**发现**：SAVE 耗时与数据量近似正比（~3.85ms/千条），dump 文件约 19 bytes/条目。

---

## 四、内存后端性能

### Q4: 内存后端性能怎么测试的？

**QPS 测试**（`tools/bench/bench_mem_backend.py`）：

- Python 自建 RESP 客户端，不依赖 `redis-benchmark`
- 三种后端（libc/jemalloc/custom）各自启动独立 kvstore 实例（不同端口）
- 预热 200 次后，执行 N 次写命令，`time.perf_counter()` 计时算 QPS
- 每条命令后检查 `+OK` 响应
- 支持 ESET/RSET/HSET/XSET 四种引擎命令
- 额外采集：VmRSS、VmSize、MEMSTAT 详细统计（碎片率、页面利用率等）

**内存占用测试**（`tools/bench/mem_pool_bench.py`）：

- 写入 100w 条 HSET → 释放 100w 条 HDEL
- 在 1%/10%/50%/80%/100% 进度点采样 `/proc/<pid>/status` 的 VmSize 和 VmRSS
- 对比三种后端在峰值内存、释放率、碎片率方面的表现

**关键结果**（2026-07-01 重测，100w 条目峰值）：

| 后端 | 峰值 VmRSS | 释放后 VmRSS | 释放率 |
|------|-----------|-------------|--------|
| libc | 71,136 KB | 68,672 KB | 3.5% |
| jemalloc | 73,676 KB | 15,376 KB | 79.1% |
| custom | 69,432 KB | 11,000 KB | 84.2% |

---

## 五、kprobe 转发性能

### Q5: kprobe 转发以及网络转发的性能怎么测试的？

**kprobe 主从转发 QPS**（`tests/perf/test_kprobe_repl_qps.c`）：

- 自包含 C 程序，内部同时创建客户端线程、Master echo 线程、Slave 接收线程、BPF ringbuf 消费者线程
- 三种模式：`none`（纯 echo 基准）、`sync`（echo + 同步 TCP 转发）、`kprobe`（echo + kprobe 内核截获 → ringbuf → 异步转发）
- 客户端计时方式：
  ```
  预热 count/10 次 → 循环 write_full(req) → read_full(rsp) → now_us() 计时
  ```
- **QPS 计时终点是客户端收到 Master 回显响应，不等数据到达 Slave**
- 输出：`QPS = count / 总耗时`，延迟取 avg/p50/p99/min/max
- 跨机 Slave 运行 `test_slave_receiver.c`：`listen(15801) → accept → read 循环统计`
- QPS/latency 指标含义：
  - **QPS** = 客户端每秒完成的请求-响应往返次数
  - **avg/p50/p99/min/max** = N 次往返延迟的排序分位值（同一组数据，不同视角）

**为什么 kprobe QPS 反而比 sync 差？**

理论上 kprobe 异步转发不应阻塞 echo 路径，但 kprobe/kretprobe hook 本身跑在 `read()` 的临界路径上：
- 每次 `read()` 多执行 2 次 BPF 程序 + 1 次 `bpf_probe_read_user` + 1 次 `ringbuf_output`
- 这个内核态开销超过了 sync 模式下用户态 `write_full(slave)` 的开销

**关键结果**（跨机，2026-07-01 重测，10K 请求）：

| Payload | none QPS | sync QPS | kprobe QPS | kprobe vs sync |
|---------|---------|---------|-----------|---------------|
| 64B | 29,259 | 22,242 | 15,556 | −30.1% |
| 4096B | 26,406 | 19,926 | 14,341 | −28.0% |

**eBPF sockmap 转发**（`tests/perf/test_ebpf_forward_qps.c`）：

- 本地 loopback 三节点架构：`client → forwarder (eBPF sockmap / 用户态 epoll) → echo_server`
- 两种模式对比：`userspace`（epoll proxy）vs `ebpf`（sk_msg + sockmap 内核态重定向）
- 预热 1000 次后正式测试，记录每条延迟

---

## 六、RDMA / sendfile / iperf 吞吐量对比

### Q6: RDMA、sendfile、iperf 的性能怎么测试的？

共 4 种传输方式，3 个自研 C 程序 + 1 个标准工具。全部测跨机单方向吞吐量（Master → Slave）。

**测试环境**：2 × KVM 虚拟机，Soft-RoCE（rxe0 on ens33）。

**iperf3（TCP 基准线）**：
```bash
slave$ iperf3 -s -p 18526
master$ iperf3 -c 192.168.233.129 -p 18526 -t 5 -f m
```
`-t 5` 持续 5 秒自动取均值，`-f m` 输出 Mbps（`-f` = `--format`，控制带宽单位显示格式）。

**sendfile 测试**（`tests/perf/test_sendfile_throughput.c`）：

- 客户端：创建临时文件 → `connect` → 循环 `sendfile(sock, fd, &offset, size)` → `shutdown(SHUT_WR)`
- 服务端：`accept → read` 循环计数 → 计时
- **计时起止点**（客户端侧）：第一次 `sendfile()` 之前开始 → `shutdown(SHUT_WR)` 之后结束
- 吞吐量 = `总发送字节 × 8 / 耗时`
- `sendfile()` 零拷贝：数据从文件 page cache 直接进入 socket 发送缓冲区，省去 `read→用户态buf→write` 两次拷贝

**RDMA WRITE/SEND 测试**（`tests/perf/test_rdma_throughput.c`）：

- 使用 `rdma_cm`（对齐生产代码 `kvs_repl.c`）
- 连接建立：`rdma_resolve_addr → rdma_resolve_route → rdma_create_qp → ibv_reg_mr → rdma_connect/accept`
- MR 信息通过 `private_data` 交换
- 客户端核心循环：
  ```c
  double t0 = now_us();              // ← 开始计时
  
  for (int i = 0; i < iters; i++) {
      ibv_post_send(qp, &send_wr, ...);  // 非阻塞
      // 周期性 poll CQ 回收 completion
  }
  
  // drain 所有 inflight completions
  while (inflight > 0) {
      ibv_poll_cq(cq, 64, wc);
  }
  
  double t1 = now_us();              // ← 结束计时
  throughput = (iters * size * 8) / (t1 - t0);
  ```
- **计时覆盖**：所有 `ibv_post_send()` 提交 + drain 等待全部 completion 返回，不等远端确认即结束
- QP depth=1024，服务端预 post 256 recv WR，inflight ≥ 256 时主动 poll 回收

**为什么这样对比？**

| 维度 | iperf3 TCP | sendfile | RDMA WRITE | RDMA SEND |
|------|:---:|:---:|:---:|:---:|
| 用户态拷贝 | 有 | 无（内核零拷贝） | 无（DMA） | 无（DMA） |
| 内核协议栈 | TCP 全栈 | TCP 全栈 | 绕过，UDP 封装 | 绕过，UDP 封装 |
| 远端 CPU | 参与 recv | 参与 recv | **不参与** | 参与 recv |

**关键结果**（3 次取中位数）：

| 传输方式 | Payload | 吞吐量 | 对比 iperf3 |
|---------|---------|--------|------------|
| iperf3 TCP | — | 5,370 Mbps | 基准 |
| sendfile | 64KB | 5,700 Mbps | +6.1% |
| RDMA WRITE | 1MB | 1,020 Mbps | −81.0% |
| RDMA SEND | 1MB | 994 Mbps | −81.5% |

**关键发现**：
1. Soft-RoCE 跨机 RDMA 远低于 TCP（UDP 封装 + KVM 虚拟交换机瓶颈）
2. 本地 RDMA 可达 45 Gbps（同机 rxe0 loopback），代码路径高效，瓶颈在网络
3. kernel 6.1 rxe 跨机发包吞吐仅为 5.15 的 ~40%
4. RDMA WRITE ≈ RDMA SEND（瓶颈在底层 RoCEv2 UDP，而非操作类型）
5. sendfile ≈ iperf3（5.4-5.7 Gbps），跨机 TCP 栈已充分优化

---

## 相关文件

| 文件 | 说明 |
|------|------|
| `tools/bench/run_persist_bench.sh` | AOF 持久化 + SAVE 性能测试 |
| `tools/bench/run_pipeline_bench.sh` | Pipeline 批量性能测试 |
| `tools/bench/run_save_hset.sh` | SAVE 性能测试（早期版本） |
| `tools/bench/bench_mem_backend.py` | 内存后端 QPS + 内存统计测试 |
| `tools/bench/mem_pool_bench.py` | 内存占用/释放测试 |
| `tests/perf/test_kprobe_repl_qps.c` | kprobe 主从转发 QPS 对比 |
| `tests/perf/test_ebpf_forward_qps.c` | eBPF sockmap vs 用户态 proxy QPS |
| `tests/perf/test_rdma_throughput.c` | RDMA WRITE/SEND 吞吐量 |
| `tests/perf/test_sendfile_throughput.c` | sendfile 零拷贝吞吐量 |
