# 2026-11-15 本地优化候选与充分验证

状态：本地候选，尚未测得 Linux 整机收益，未运行本轮官方测评。
基线为队长分支 `a285f2e67be0f6de2ac68c2b32ec6a2426984f01`；其产品源码与
`c760bb5` 相同，后续差异只有评测材料。候选保留这些材料。

## 产品改动

1. `common.h` 使用编译期 slicing-by-8 表计算原有 IEEE CRC32。种子、补码、尾部处理和 WAL 格式保持一致；额外只读查找表约 7 KiB。
2. `main.cpp` 在 JSONL 请求间复用响应字符串。每次请求清空内容，发出响应后若容量超过 64 KiB 则释放；没有限制允许返回的响应大小。

提交预扩容暂缓，原因是需要额外验证扩容失败时的落盘/恢复状态。本轮未更改存储同步、向量算法或 SQL 语义。

## 本地证据

macOS arm64 / Apple clang，基线与候选均使用相同的临时兼容构建：仅在副本中调整 x86 条件编译，并将缺失的 `fdatasync` 映射为 `fsync`。产品源码没有加入兼容替换。普通构建 `-O3`；sanitizer 构建为 `-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all`。故以下证明的是本机标量实现的兼容性与内存检查，不能替代 Linux/AVX2/实际持久化性能测量。

| 检查 | 普通候选 | ASan/UBSan 候选 |
|---|---|---|
| 官方 SQL 文件 / 自建回归 | 20/20、13/13 | 20/20、13/13 |
| JSONL、比较边界、过滤 TopK | 通过 | 通过 |
| SQL 随机参考模型 | 20 个种子 × 500 轮 | 10 个种子 × 500 轮 |
| SQLite 独立事务行状态模型 | 20 个种子 × 1,000 步 | 10 个种子 × 1,000 步 |
| 事务模型完整状态检查 | 26,912 次 | 13,406 次 |
| 事务模型预期约束失败 | 6,314 次 | 3,186 次 |
| 事务模型 SIGKILL 重启 | 1,605 次 | 795 次 |
| 混合 JSONL 与基线逐字节对照 | 3 个种子 × 3,000 条生成请求 | 相同语料通过 |
| 20 万行 × 32 维重启/全表扫描 | 通过 | 通过 |
| 单批 1 万行 × 128 维 | 通过 | 通过 |

同一种子在不同构建重复运行是交叉验证，不是新的独立随机样本。

- CRC 单测：标准向量、空指针零长度、4,102 个独立按位参考对照，覆盖非对齐、尾长、种子串联和约 6.5 MB 输入；另有 20,464 个差分/串联 helper 检查。独立审查者也运行了 CRC sanitizer 单测。
- WAL 兼容：两个表、每个 writer 共 5 帧逐字节一致，Python zlib 独立核验；清空测试副本的行槽后双向重放，覆盖 14 类尾部截断/CRC/长度/魔数损坏，修复后追加并再次重放。
- 检查点：14.5 万行 × 128 维，在第 115,000 行跨过 64 MiB 提交阈值；强杀后启动清理 17,550,600 字节 WAL 后缀，核对完整 ID 集和向量样本；再次强杀验证已提交的更新/删除/主键迁移及未提交内容回滚。
- 缓冲区诊断：小响应预热后复用同一缓冲区；大响应容量 98,303 字节在发出后释放为 22 字节，后续小响应恢复复用。
- 负向对照：错误 CRC 多项式、删除恢复端 CRC 校验、修改协议 affected_rows，均被对应测试发现。额外的可恢复 UBSan 故障程序证明测试驱动会拒绝 sanitizer 错误。
- 独立代码审查完成，无遗留 critical/important 问题。测试驱动现强制并记录 sanitizer 遇错退出，且将同一二进制测试标记为 self-recovery。

CRC helper 在本机的配对吞吐中位数约为旧版的 4.49–4.61 倍；这是函数级测量，不能换算为数据库提速或比赛分数。目标机上仍应先运行同二进制 A/A，再随机交错 A/B，保留全部样本和控制负载。

## 可复现入口

Linux 上用原始源码构建候选与冻结的基线，二进制不要混用：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
c++ -std=c++17 -O3 -Wall -Wextra -Isrc tests/crc32.cpp -o build/crc32-test
build/crc32-test
python3 tests/validate.py --binary ./build/minivectordb \
  --baseline /absolute/path/to/baseline/minivectordb \
  --output /absolute/path/to/new-validation-directory \
  --seeds 20 --rounds 500 --steps 1000
```

`--output` 必须是新目录；日志、命令、二进制 SHA、sanitizer 设置、跳过项及失败状态均写入 `summary.json`。
省略 `--baseline` 只证明同版本恢复，不代表新旧兼容。所有测试串行运行，超时会终止该测试进程组。
CRC 单测需单独执行上述编译命令；主验证驱动接收已构建的数据库二进制。

本地原始证据：`/Users/xiazihan/seekdb-work/handoff/minivectordb-next-20261115/`，
主入口为 `final-optimized/summary.json`、`final-sanitized/summary.json`、`manifest.json`。
源码改动的测试已接入镜像 CI；本轮没有触发远端 CI。

## 留给稀缺测评机的检查

1. 原始 Linux 构建与实际 Docker 镜像的官方协议检查；两项 Linux `/proc` + `prlimit` 内存门槛（Mac 明确跳过）。
2. x86 AVX2/FMA 路径、目标编译器和真实 `fdatasync` 环境下的全套正确性检查。
3. 冻结同一二进制的 A/A 校准，再测 baseline、CRC-only、buffer-only、组合候选。记录写入、过滤/无过滤查询、恢复、RSS、CPU 与墙钟；吞吐证据不足时不推广。

进程强杀保留 OS 页缓存，本地结果不证明断电安全，也不穷尽存储错误注入。
