# PR 30 与 CRC-32C 目标分支整合

目标基线：`63cffcbce7623f3ee4a0fc3cb423ba8c68e1d353`。整合前候选：`e125a8df5a69f5615b19049a9444d38594dc7b0f`。将目标分支合入现有 PR 分支，保留它的 AVX2 批处理、过滤索引、向量缓存、CRC-32C 硬件派发、profile 和输出改动。

## 最终方案

- 保留 `runStdio` 的跨请求响应缓冲：每轮清空，发出响应后容量超过64KiB就释放。阈值不限制响应大小；profile 只写 stderr。
- 采用目标已有的 Castagnoli CRC-32C，不替换 SSE4.2 加速和软件回退。撤下本 PR 原先的 IEEE slicing-by-8 改动，`common.h` 恢复为目标内容，避免留下不会被 WAL 使用的优化。
- WAL 检查用独立按位 CRC-32C 实现与标准向量核对。跨版本对照基线改为最新目标，而非旧 IEEE 版本；不声称兼容早期 IEEE WAL。负向对照确认新的检查器拒绝旧格式。
- 增加 CRC-32C 非对齐/种子/尾长测试、profile 开关下逐字节协议对照，以及 TopK 每坐标、不同维度、缓存开关、删除/更新检查。

## 整合回归发现的32维问题

Linux首轮运行 [43732529292](https://github.com/Funnyvalentine00/seekdb1/actions/runs/43732529292) 在新增逐坐标测试中失败：32维、缓存开启、exact的Top1预期主键1，实际为9。目标分支的 `VECS == 4` 分支只归约第一个8维累加器，漏掉后24维；同样问题存在于连续/行槽扫描、双行与尾行路径。修复统一使用原有四累加器归约，保留AVX2双行批处理、求和树、缓存与过滤索引。

## 验证范围

本机 arm64 只能覆盖软件/标量路径。临时副本使用相同的 macOS 兼容变换：保护 x86 intrinsics include、将共享 heap helper 移出 x86 guard、fdatasync 映射为 fsync；这些兼容变换不提交到产品源码。目标与整合版分开构建并记录源码/二进制 SHA。

本地验证入口为 `tests/validate.py`，以目标构建为 `--baseline`，普通与 ASan/UBSan 候选各运行5个随机种子、每种子500轮SQL随机模型及1000步事务模型，保留20万行/单批1万行恢复规模。Linux特有的两项内存检查在Mac显式跳过，由CI执行。

普通与ASan/UBSan两轮各21项已通过；每轮公开SQL为20/20，自建回归为13/13。CRC-32C独立单测普通及sanitizer各通过4102个seed/非对齐/串联对照。

Linux CI执行原始CMake和Docker构建，核对原生CRC-32C、强制软件回退、完整维度TopK、实际镜像协议/公开用例、目标↔镜像WAL双向兼容、3000条协议差分、事务/检查点/查询边界/内存和随机模型。执行状态及日志以PR的最新检查为准，不用旧轮结果代替整合验证。

原始证据目录：`/Users/xiazihan/seekdb-work/handoff/mvd-pr30-integration-20261115/`。本轮不进行性能验收。此前拯救者未接电源的数值仅作旧版本的诊断参考；本次机器无法启动，Linux验证改用GitHub Actions。
