# 专业目录结构重构方案

## 当前问题
1. 源文件和头文件混杂在根目录
2. 缺乏清晰的分层结构
3. 第三方库与核心代码混合
4. 测试、基准测试、文档分散

## 目标
- 符合现代 C 项目标准
- 清晰的模块分离
- 易于维护和扩展
- 保持向后兼容性

## 新目录结构

```
9.1-kvstore/
├── README.md
├── Makefile
├── CMakeLists.txt (可选)
├── .gitignore
├── src/
│   ├── main/
│   │   └── kvstore.c (主程序入口)
│   ├── core/
│   │   ├── reactor.c
│   │   ├── server.c (从 kvstore.c 提取的服务器逻辑)
│   │   ├── connection.c
│   │   └── protocol.c
│   ├── storage/
│   │   ├── kvs_array.c
│   │   ├── kvs_hash.c
│   │   ├── kvs_rbtree.c
│   │   ├── kvs_skiptable.c
│   │   └── engine.c (存储引擎抽象)
│   ├── memory/
│   │   ├── kvs_mem.c
│   │   ├── allocator.c
│   │   └── stats.c
│   ├── expire/
│   │   └── kvs_expire.c
│   ├── replication/
│   │   ├── kvs_repl.c
│   │   └── master_slave.c
│   ├── persistence/
│   │   ├── kvs_persist.c
│   │   ├── aof.c
│   │   └── snapshot.c
│   └── utils/
│       ├── hash.c
│       ├── time.c
│       └── helpers.c
├── include/
│   ├── kvstore/
│   │   ├── kvstore.h (主头文件)
│   │   ├── core/
│   │   │   ├── server.h
│   │   │   └── protocol.h
│   │   ├── storage/
│   │   │   ├── engine.h
│   │   │   ├── array.h
│   │   │   ├── hash.h
│   │   │   └── rbtree.h
│   │   ├── memory/
│   │   │   ├── allocator.h
│   │   │   └── stats.h
│   │   ├── expire/
│   │   │   └── expire.h
│   │   ├── replication/
│   │   │   └── replication.h
│   │   ├── persistence/
│   │   │   └── persistence.h
│   │   └── utils/
│   │       ├── hash.h
│   │       └── time.h
│   └── thirdparty/ (如果需要)
├── lib/ (第三方库作为子模块)
│   ├── NtyCo/
│   └── liburing/
├── tests/
│   ├── unit/
│   │   ├── test_storage.c
│   │   ├── test_memory.c
│   │   └── test_expire.c
│   ├── integration/
│   │   ├── test_resp_nc_strict.sh
│   │   ├── test_resp_queue_recovery.sh
│   │   └── test_resp_ttl_nc.sh
│   ├── test.c (现有测试文件)
│   └── testcase.c
├── benchmarks/
│   ├── scripts/
│   │   ├── bench_mem_backend.py
│   │   ├── bench_mem_backend_fixed.py
│   │   └── bench_mem_backend_append.py
│   ├── data/
│   │   └── *.csv, *.aof
│   └── plots/
│       ├── bench_plots/
│       └── bench_grouped_plots/
├── docs/
│   ├── README_MASTER_SLAVE_MULTI_ENGINE.md
│   ├── kvstore_test_manual.md
│   ├── README_RESP_STREAM_TTL_PERSIST.md
│   ├── README_RESP_STREAM_TTL.md
│   ├── README_RESP_STREAM.md
│   └── RESP_TTL_Test_Manual.md
├── clients/ (重命名 kvs-client/)
│   ├── go/
│   ├── java/
│   ├── js/
│   ├── python/
│   └── rust/
├── examples/
│   ├── basic_usage.c
│   └── advanced.c
├── scripts/
│   ├── build.sh
│   ├── deploy.sh
│   └── test.sh
├── build/ (构建输出，在 .gitignore 中)
└── .github/ (可选 CI/CD)
    └── workflows/
```

## 重构步骤

### 阶段1: 创建基础结构
1. 创建新目录: src/, include/kvstore/, tests/unit/, benchmarks/, docs/, scripts/
2. 移动核心源文件到 src/ 子目录
3. 移动头文件到 include/kvstore/
4. 更新 #include 路径

### 阶段2: 组织第三方库
1. 保持 NtyCo/ 和 liburing/ 在根目录或移动到 lib/
2. 更新 Makefile 中的包含路径

### 阶段3: 更新构建系统
1. 修改 Makefile 适应新目录结构
2. 添加递归构建支持
3. 创建 CMakeLists.txt (可选)

### 阶段4: 更新脚本和文档
1. 更新测试脚本中的路径
2. 更新 README.md
3. 确保向后兼容性

## 考虑事项
1. **向后兼容性**: 创建符号链接或包装脚本
2. **Git 历史**: 使用 git mv 保留文件历史
3. **依赖关系**: 确保所有 #include 路径正确
4. **构建系统**: 确保 make 仍然工作

## 实施计划
1. 首先创建新目录结构
2. 逐步移动文件，每次移动后测试编译
3. 更新 Makefile
4. 运行测试确保功能正常
5. 清理旧文件