# FTS 配对构建的 BEng 试验

**验证边界：本地构建和运行结果只作为编译、启动及功能跑通的正确性辅助证明。本地耗时、空间占用及其变化倍数不能作为准确性能数据，不能据此宣称提速、内存收益或推算官方评分。实际性能需在评测环境验证。**

## 启用方式

启动服务进程前设置 `SEEKDB_BENG_SCOPED_OVERLAP_EXPERIMENT=1`，允许 `beng` 和 `beng.1`（大小写不敏感）复用现有 IK 配对构建队列。只有精确的字符串 `1` 启用；未设置、空值、`0`、`01`、`true` 等均关闭。IK 解析器的原有准入行为不变。

这只是解析器准入开关。调用点仍要求：非分区本地构建、DDL 并行度为 1、有父任务 ID，且辅助表类型为倒排表或 doc-word 表。现有队列仅在 mini 模式下初始化，`SEEKDB_IK_SCOPED_OVERLAP_EXPERIMENT=0` 会关闭整个配对队列，因此也会阻止 BEng 配对。队列不可用、已有其他父任务占用或入队失败时，仍提交到原来的主队列。

BEng 和 IK 共用同一组配对资源；没有新增 worker。日志中的 worker 名仍为 `DdlBuild` 和 `IkDdlPair`。两个辅助表继续使用各自的构建任务、快照和重试流程，没有共享分词结果。

## 本地准入回归

在仓库根目录运行。此测试不需要启动数据库，不链接旧版本服务器；使用本次源码中的真实 `ObString` 和生产准入函数。

```sh
c++ -std=c++20 -O2 -DNDEBUG \
  -I src -I src/oblib -I src/oblib/easy -I src/oblib/easy/include \
  unittest/share/test_fts_build_overlap.cpp -o /tmp/test_fts_build_overlap
/tmp/test_fts_build_overlap
```

macOS 需使用可用 SDK；本轮使用 Apple Clang 21，并追加：

```text
-D_DARWIN_C_SOURCE -isysroot /Library/Developer/CommandLineTools/SDKs/MacOSX27.2.sdk -Wno-format
```

内存检查用 `-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer` 替代 `-O2 -DNDEBUG`，运行时设置 `ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1`。

测试覆盖开关未设置、合法/非法值和重新关闭；IK 行为保持不变；BEng 大小写、版本、前后缀，以及未以 NUL 结尾、截断和内嵌 NUL 的解析器名。总计 495 项检查，失败时返回非零退出码。测试顺序执行并恢复原环境变量，不使用会被 `NDEBUG` 关闭的断言。

## 本地服务端正确性对照

先完成当前分支的完整 `seekdb` 构建，再使用 MySQL/obclient 命令行客户端运行：

```sh
python3 unittest/share/test_fts_beng_overlap.py \
  --binary build_release/src/observer/seekdb \
  --mysql /path/to/mysql \
  --output /tmp/fts-beng-sql-check
```

`--output` 必须是尚不存在的新目录。脚本使用同一二进制依次运行 BEng 未开启、BEng 开启、BEng 开启但配对队列关闭三组实例，关闭 TCP，使用各自目录内的 Unix socket。每组设置 2 GiB 逻辑内存预算和服务要求的最小 2 GiB 日志盘；逻辑预算不等同于操作系统内存硬上限。使用 `SIGUSR1` 正常停机，保留数据、SQL 结果及日志。`SIGTERM` 在 seekdb 现有实现中会触发 `SIGKILL`，不用于此测试的正常停机。

默认生成 4096 行文档，检查独立计算的命中 ID、舍入到 12 位小数的 BM25 分数和确定性排序、外层 COUNT/LIMIT、插入更新删除、空表/短文本/空字符串/NULL，以及两个父任务并发建索引后的结果。三组的保存结果必须完全一致。另按任务 ID 和执行编号关联构建开始/结束日志，要求开启组实际执行 companion 并观察到主索引配对重叠，关闭组保持串行。日志只用于证明执行路径；脚本不输出提速或空间收益倍数。

脚本属于 Linux/macOS 本地正确性测试，不能代替官方容器中的评测；也未覆盖完整的 DML 与建索引同时发生、运行中取消或故障注入场景。

## 评测环境验证

测评机恢复后，在同一二进制、同一数据集上分别以 BEng 开关未设置和 `1` 启动服务。两组均保持 IK 开关相同；如需完整串行对照，另设 `SEEKDB_IK_SCOPED_OVERLAP_EXPERIMENT=0`。

需要核对以下结果，并保留全部试验记录：

- 实际 BEng 倒排表和 doc-word 表任务是否在两个 worker 上重叠；记录父任务、快照和执行编号，不能仅凭额外线程存在认定发生并行。
- 建索引前后结果集合、BM25 排序及后续增删改的查询结果是否一致；覆盖空表和短文本。
- BEng/IK 及两个 BEng 父任务同时提交时的串行回退、取消、重试、销毁与重新初始化。原 `test_ddl_local_builder_ik.cpp` 的 7 个队列生命周期用例也应在 Linux 重跑。
- 单独记录 BEng 建索引耗时、总构建耗时、峰值 RSS 和 CPU，并跑官方查询及分词对照。不要将历史理想重叠时间换算成已实现的提速或评分。

当前实现默认关闭 BEng 试验。本地准入测试、编译和 SQL 对照成功，不代表上述评测环境正确性或性能验证已经完成。
