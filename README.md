# kvstore

一个基于 C 语言实现的轻量级 KV 数据库，支持 RESP 协议、多存储引擎、TTL 过期、AOF/DUMP 持久化、主从复制、分布式锁，以及可切换内存后端（`libc` / `jemalloc` / `custom`）。

> 本 README 目标：不仅告诉你“怎么跑”，还告诉你“每个功能怎么测、预期看到什么结果”。

---

## 1. 项目能力总览

- RESP 文本协议（兼容 `nc`、自定义客户端、pipeline）
- 四种 KV 引擎（Array / Red-Black Tree / Hash / Skiptable）
- 键过期管理（`EXPIRE` / `TTL` / `PERSIST`）
- 持久化（AOF 追加、DUMP 快照、重启恢复）
- 主从复制（从节点追主、全量 + 增量同步）
- 分布式锁命令（`LOCK` / `UNLOCK` / `RENEW`）
- 内存后端切换与 `MEMSTAT` 观测

---

## 2. 目录结构

```text
.
├── src/
│   ├── main/           # 命令解析、命令执行、配置解析
│   ├── core/           # reactor/proactor/协程相关网络模型
│   ├── storage/        # array / rbtree / hash / skiptable
│   ├── expire/         # TTL 管理
│   ├── persistence/    # AOF / DUMP / 自动快照
│   ├── replication/    # 主从复制
│   ├── memory/         # 内存后端抽象与 custom 分配器
│   └── utils/
├── include/kvstore/    # 公共头文件
├── tests/              # 单测/集成脚本
├── benchmarks/         # 压测和绘图脚本
├── clients/            # 多语言客户端示例
└── docs/               # 设计与测试文档
```

---

## 3. 构建与启动

### 3.1 编译

```bash
make clean && make
```

生成 `./kvstore`。

### 3.2 常见启动方式

```bash
# master（默认角色）
./kvstore --port 5000

# 切换内存后端
./kvstore --port 5000 --mem libc
./kvstore --port 5000 --mem jemalloc
./kvstore --port 5000 --mem custom

# AOF 每秒刷盘 + 自动快照规则
./kvstore --port 5000 --appendfsync everysec --autosnap 60:1000,300:10000
```

### 3.3 参数说明

- `--port <port>`：监听端口
- `--role <master|slave>`：节点角色
- `--master-host <ip>`：从库追主地址
- `--master-port <port>`：从库追主端口
- `--dump <path>`：DUMP 文件（默认 `kvstore.dump`）
- `--aof <path>`：AOF 文件（默认 `kvstore.aof`）
- `--mem <libc|jemalloc|custom>`：内存后端
- `--appendfsync <always|everysec>`：AOF 刷盘策略
- `--autosnap <sec:changes[,sec:changes...]>`：自动快照阈值规则

---

## 4. 命令与引擎映射

### 4.1 基础引擎（Array）

- 写：`SET` `MSET` `MOD` `DEL`
- 读：`GET` `MGET` `EXIST`
- 过期：`EXPIRE` `TTL` `PERSIST`

### 4.2 前缀引擎

- 红黑树：`RSET/RGET/RMSET/RMGET/RMOD/RDEL/REXPIRE/RTTL/RPERSIST/REXIST`
- 哈希：`HSET/HGET/HMSET/HMGET/HMOD/HDEL/HEXPIRE/HTTL/HPERSIST/HEXIST`
- 跳表：`TSET/TGET/TMSET/TMGET/TMOD/TDEL/TEXPIRE/TTTL/TPERSIST/TEXIST`

### 4.3 系统命令

- `INFO`：查看节点角色与内存后端
- `MEMSTAT`：内存统计（custom 后端有更细粒度字段）
- `SAVE` / `BGSAVE`：触发快照
- `LOCK` / `UNLOCK` / `RENEW`：锁语义命令

---

## 5. 快速验证（5 分钟）

### 5.1 启动服务

```bash
./kvstore --port 5000
```

### 5.2 发一组最小命令

```bash
# SET name alice
printf '*3\r\n$3\r\nSET\r\n$4\r\nname\r\n$5\r\nalice\r\n' | nc 127.0.0.1 5000

# GET name
printf '*2\r\n$3\r\nGET\r\n$4\r\nname\r\n' | nc 127.0.0.1 5000

# INFO
printf '*1\r\n$4\r\nINFO\r\n' | nc 127.0.0.1 5000
```

预期：写命令返回 `+OK`，`GET` 返回 bulk string，`INFO` 返回角色/后端信息。

---

## 6. 各功能测试方法（重点）

> 建议开三个终端：
> - 终端 A：服务端日志
> - 终端 B：手工发命令
> - 终端 C：跑脚本

### 6.1 RESP 协议正确性（基础）

**方法 A（推荐）**：运行严格集成脚本。

```bash
bash tests/integration/test_resp_nc_strict.sh 127.0.0.1 5000
```

覆盖点：
- 正常 `SET/GET/MOD/DEL`
- pipeline（多命令粘连）
- 半包恢复（分片发送）
- 协议错误恢复（坏包后继续处理）

**方法 B**：手工发非法请求验证容错。

```bash
printf 'x\r\n*2\r\n$3\r\nGET\r\n$4\r\nname\r\n' | nc 127.0.0.1 5000
```

预期：先返回协议错误，再继续处理后续合法命令。

---

### 6.2 多引擎功能测试（Array/RBTree/Hash/Skiptable）

对每个引擎执行“写->读->删->查不存在”。

```bash
# Array
printf '*3\r\n$3\r\nSET\r\n$2\r\na1\r\n$2\r\nv1\r\n' | nc 127.0.0.1 5000
printf '*2\r\n$3\r\nGET\r\n$2\r\na1\r\n' | nc 127.0.0.1 5000

# RBTree
printf '*3\r\n$4\r\nRSET\r\n$2\r\nr1\r\n$2\r\nv1\r\n' | nc 127.0.0.1 5000
printf '*2\r\n$4\r\nRGET\r\n$2\r\nr1\r\n' | nc 127.0.0.1 5000

# Hash
printf '*3\r\n$4\r\nHSET\r\n$2\r\nh1\r\n$2\r\nv1\r\n' | nc 127.0.0.1 5000
printf '*2\r\n$4\r\nHGET\r\n$2\r\nh1\r\n' | nc 127.0.0.1 5000

# Skiptable
printf '*3\r\n$4\r\nTSET\r\n$2\r\nt1\r\n$2\r\nv1\r\n' | nc 127.0.0.1 5000
printf '*2\r\n$4\r\nTGET\r\n$2\r\nt1\r\n' | nc 127.0.0.1 5000
```

预期：四类引擎均能返回一致的写读行为。

---

### 6.3 TTL 过期测试

**脚本法（推荐）**：

```bash
bash tests/integration/test_resp_ttl_nc.sh 127.0.0.1 5000
```

**手工法**：

```bash
printf '*3\r\n$3\r\nSET\r\n$2\r\nk1\r\n$2\r\nv1\r\n' | nc 127.0.0.1 5000
printf '*3\r\n$6\r\nEXPIRE\r\n$2\r\nk1\r\n$1\r\n2\r\n' | nc 127.0.0.1 5000
printf '*2\r\n$3\r\nTTL\r\n$2\r\nk1\r\n' | nc 127.0.0.1 5000
sleep 3
printf '*2\r\n$3\r\nGET\r\n$2\r\nk1\r\n' | nc 127.0.0.1 5000
```

预期：TTL 倒计时后键失效，`GET` 返回空值。

---

### 6.4 持久化测试（AOF + DUMP）

#### AOF 恢复

1) 启动服务并写入数据；2) 正常退出；3) 重启同路径 AOF。

```bash
./kvstore --port 5000 --aof kvstore.aof --dump kvstore.dump
# 写入一些数据后，重启同参数再 GET
```

预期：重启后数据仍可读。

#### SAVE/BGSAVE

```bash
printf '*1\r\n$4\r\nSAVE\r\n' | nc 127.0.0.1 5000
printf '*1\r\n$6\r\nBGSAVE\r\n' | nc 127.0.0.1 5000
ls -lh kvstore.dump kvstore.aof
```

预期：出现/更新 dump 文件，响应成功。

---

### 6.5 主从复制测试

#### 步骤

```bash
# 终端 A：master
./kvstore --role master --port 5000

# 终端 B：slave
./kvstore --role slave --port 5001 --master-host 127.0.0.1 --master-port 5000
```

向 master 写：

```bash
printf '*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n' | nc 127.0.0.1 5000
```

向 slave 读：

```bash
printf '*2\r\n$3\r\nGET\r\n$3\r\nfoo\r\n' | nc 127.0.0.1 5001
```

预期：从库可读到 `bar`，并且从库写命令应被限制。

---

### 6.6 内存后端与 MEMSTAT 测试

分别启动三种后端，对比 `INFO + MEMSTAT` 输出。

```bash
./kvstore --port 5000 --mem libc
./kvstore --port 5000 --mem jemalloc
./kvstore --port 5000 --mem custom

printf '*1\r\n$4\r\nINFO\r\n' | nc 127.0.0.1 5000
printf '*1\r\n$7\r\nMEMSTAT\r\n' | nc 127.0.0.1 5000
```

预期：
- `INFO` 展示对应 backend
- `MEMSTAT` 均有统一统计字段
- `custom` 能看到更多 `class_*`、`small/large` 指标

---

### 6.7 写队列/粘包/恢复测试

```bash
bash tests/integration/test_resp_queue_recovery.sh 127.0.0.1 5000
```

预期：脚本汇总 PASS/FAIL；验证 pipeline、半包、错误恢复。

---

### 6.8 分布式锁测试（手工）

```bash
# 加锁：key=lock:user42 token=abc ttl=3000(ms)
printf '*4\r\n$4\r\nLOCK\r\n$11\r\nlock:user42\r\n$3\r\nabc\r\n$4\r\n3000\r\n' | nc 127.0.0.1 5000

# 续租
printf '*4\r\n$5\r\nRENEW\r\n$11\r\nlock:user42\r\n$3\r\nabc\r\n$4\r\n3000\r\n' | nc 127.0.0.1 5000

# 解锁
printf '*3\r\n$6\r\nUNLOCK\r\n$11\r\nlock:user42\r\n$3\r\nabc\r\n' | nc 127.0.0.1 5000
```

预期：同 token 可续租/释放；非持有者 token 应失败。

---

## 7. 自动化测试与压测入口

### 7.1 集成测试

```bash
bash tests/integration/test_resp_nc_strict.sh
bash tests/integration/test_resp_ttl_nc.sh
bash tests/integration/test_resp_queue_recovery.sh
```

### 7.2 旧版 C 测试入口（按需）

```bash
bash test_kv.sh
```

### 7.3 性能压测

```bash
./run_benchmark.sh bench_mem_backend.py --ops 50000 --value-size 128

# 或直接跑脚本
cd benchmarks/scripts
python3 bench_mem_backend.py --ops 100000 --value-size 256 --warmup 1000
python3 plot_bench_grouped.py
```

输出位置：
- CSV：`benchmarks/data/`
- 图表：`benchmarks/plots/`

---

## 8. 排障建议

- `nc` 无输出：检查端口、服务是否启动、payload 是否携带 `\r\n`
- 从库无数据：检查 `--master-host/--master-port` 是否正确，先看 master/slave 日志
- 重启无恢复：检查 `--aof` / `--dump` 路径是否一致
- `make` 链接失败：确认 `liburing` 与 `NtyCo` 依赖可用

---

## 9. 进阶阅读

- `docs/技术文档_详细实现分析.md`
- `docs/kvstore_design_walkthrough.md`
- `docs/kvstore_test_manual.md`
- `README_MASTER_SLAVE_MULTI_ENGINE.md`

---

## 10. License

当前仓库未显式提供 License；如需开源分发，请补充许可证文件。
