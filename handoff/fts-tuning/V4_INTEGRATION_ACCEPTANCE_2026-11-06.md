# v4 集成验收与 MATCH 边界修正（2026-11-06）

## 当前结论

**用户最新指令：停止远端操作与推送，只做本机最小收尾，然后等待新命令。2914保留原v4（PID31133），不切换。**

H2/H3/H7 的集成查询收益得到本机验证；原 v4 的三处 MATCH 兼容性错误已修复并在独立实例验证。**尚未完成全面验收，不标记“可提测通过”**：六对 A/B 的 `build_ik_content_sec` 配对中位数仍为 -2.78%，超过 1% 门禁；File SQL 需要 Linux 验证，legion 已在原2222端口恢复连接，但独立构建在链接阶段遇到系统I/O故障；目录写入返回EROFS，仍无法完成Linux验收。

用户选择是“验收通过后切换修正版并保持运行”。条件目前未满足，因此 **2914 仍保留原 v4，不重启、不替换**。这也意味着该实例仍带有本文复现的原 v4 边界错误。普通四条基准查询可用，已加载20k数据。当前入口仍是 `/Users/xiazihan/seekdb-work/data/fts-v4-20261106`，PID31133。

## 版本与二进制

- 精确基线源码：`7accd1ace8ee5d035b3ef8aaf426c46b1d0a595e`（v3，fdb8363 底座）。
- 修正代码提交：`194150497aca9a69e395317bd6d5a64f26340a61`，分支 `feat/fts-opt-v4`；仅本地，未推送。
- 原 v4 二进制：`seekdb-fts-opt-v4-20261106`，SHA f9716d9fe03eadc287a66c9a342bb153a9f23c286a1499abb9489d394cf3269d。
- 新 v3 基线：`seekdb-fts-opt-v3-verified-20261106`，SHA 27016ae84cca826239e8d967b5a4ab3d0d463dea7c0aa6389f86c97aceab4908。
- 修正版：`seekdb-fts-opt-v4-fixed-20261106`，SHA 6e3f898d7f45744e1dacfd7cd4a91da3a91d593ff4cad7823b76265cb1f5f600。
- 上述二进制都位于 `/Users/xiazihan/seekdb-work/saved-binaries/`；不要把 `build_release/` 的当前文件当作版本身份。
- 构建 v3 时，仅临时替换 v3/v4 不同的11个源码文件，构建后已按备份恢复；对照源码 SHA 在 `source-manifest.json`，没有修改另一 agent 的 seekdb1 工作树。
- 两个 File SQL `fast_float` 本机构建补丁维持未提交状态，不在本轮代码提交和导出补丁内。

## 发现并修复的三处问题

原 v4 f9716d9f 与精确 v3 在一次性端口2916、全新目录上对照：

| 输入 | v3 | 原 v4 | 修正版 |
|---|---|---|---|
| `SELECT id,MATCH(a,b) AGAINST('database')>0 FROM t ORDER BY id` | 成功 | 4016 内部错误 | 成功，完整输出相同 |
| 命中专用多列 MATCH，258个查询词 | 成功 | 1235 不支持 | 成功，计数相同 |
| 16分区、1行的多列 MATCH | 成功 | 4016 内部错误 | 成功，计数相同 |

修正只涉及两个现有源码：

1. `src/sql/optimizer/ob_log_plan.cpp`：评分省略分析移到 `prepare_text_retrieval_scan`；`prepare_text_retrieval_info` 的 lookup/index-merge 等调用方保持评分契约，避免 function lookup 缺失投影。
2. `src/sql/das/iter/ob_das_iter_utils.cpp`：无评分查询不创建不存在的 doc-agg 子迭代器；TAAT 只用于需要评分的查询，无评分长查询使用动态数组的 DaaT，不进入原先显式拒绝无评分的 TAAT。

先记录三个失败，再修改、完整构建并验证。补测共9组：上述三类、255/256/257/1024词、prepared short→long→short、强制PRIMARY的function lookup，全部与v3逐字节相同。原 projection / score controls / tokenize 数据集的 stdout、stderr、状态也与v3完全相同。独立审查未发现新增 Major/Blocker。

**资源限制**：长查询 DaaT 内存是 tokens×batch_size；1024词探针结束后的 RSS，v3约312.4MB、修正版333.8MB，多约21.4MB。这是功能修复的资源代价；不能用短查询的RSS持平来宣称任意长查询都不增加内存。超出1024词尚未测量。

复现 SQL：`regress/scoreless-match-boundaries.sql`。完整probe driver和原始输出位于本机 results 目录。

## 六对完整 A/B

macOS arm64，3次v3先、3次修正版先，每轮全新数据目录。官方脚本和参数未改：ROWS=20000、BATCH=500、ROUNDS=3000、QUERY_ROUNDS=200、SAMPLES=3、WARMUP=30、SKIP_LOAD=0。

改善为 `100*(1-candidate/base)`，正值更快。表中耗时是各版本运行值中位数；改善是逐对改善的中位数，二者不可直接混算。均值/标准差/全部样本与另一统计口径均保留在JSON中。

| 指标 | v3耗时中位数 | 修正版耗时中位数 | 配对改善中位数 |
|---|---:|---:|---:|
| `build_ik_all_sec` | 2.45900 | 2.45850 | +0.02% |
| `build_ik_content_sec` | 1.79050 | 1.84100 | -2.78% |
| `build_beng_en_sec` | 1.03400 | 1.03500 | -0.24% |
| `tokenize_ik_avg_ms` | 0.07320 | 0.07245 | -0.10% |
| `tokenize_beng_avg_ms` | 0.04740 | 0.04715 | +1.14% |
| `query_cn_avg_ms` | 0.82930 | 0.53970 | +34.93% |
| `query_beng_avg_ms` | 1.19300 | 0.83405 | +31.67% |
| `query_mixed_avg_ms` | 0.92015 | 0.63215 | +31.45% |
| `query_limit_avg_ms` | 0.97415 | 0.86535 | +11.73% |
| `build_total_sec` | 5.30150 | 5.31000 | -1.10% |

四项query改善中位数的平均约 **27.45%**，不是官方平台得分。12份报告的hits都精确为8001/11000/7332/20。

服务端指令数，六轮×每项3次重复：cn/beng/mixed/limit 分别约下降39%/38%/38%/12%；同二进制短重复波动远小于该收益。Tokenize指令数基本持平。峰值RSS的运行中位数变化 **-0.28%**。

## A/A与门禁判断

同一v3二进制、两个全新目录的完整A/A也出现明显墙钟波动：build_ik_all约4.36%、build_total约2.20%、tokenize_ik约-11.26%、query_beng约-8.95%。这表明本机墙钟有噪声，不能从单个小读数直接断言代码退化。

但也**不能据此豁免硬门禁**。六对A/B里build_ik_content仍为-2.78%，必须保留为“未过”，不把查询收益抵消它，也不改计时、SQL或数据量。build_total配对中位数-1.09%仅作诊断，它不直接进入官方九指标。需要稳定Linux环境重新验证。

## 三套回归

| 套件 | 精确v3 | 修正版 | 判断 |
|---|---|---|---|
| fts_index | 13/15 | 13/15 | 同为basic_dml和partitioned_simple_query，不新增失败 |
| ai_column，独立mock端口 | 5/5 | 5/5 | SQL与mock断言执行完，归一化URL端口后完整输出与原金样一致 |
| file_sql，本地fixture适配 | 不支持 | 不支持 | macOS在启用后read_csv处1235，需Linux，不能标成通过 |

FTS当前fdb底座环境的basic_dml失败是1598行未解析的`$rowkey_doc_table_id`，partitioned是EXPLAIN编号差异；与v3一致。不能照抄master底座此前的14/15。另做了删表重建的元数据探针：table_id确实改变，但刷新ID后无主键表也没有index_type=11子表；因此不能仅补一行变量刷新就宣称用例修好。没有改原用例或金样来消除该失败，详见results下`metadata-fixture-repro.out`。

**测试适配与原始结果区别**：

- 不修改产品账号/权限。cn_word的临时驱动把硬编码admin连接适配为已有root；原始.test/.result不变。自动审批曾拒绝旧脚本的admin全库授权，这一操作已移除，未实施。
- File SQL只在临时驱动中用本地fixture代替ssh/scp部署，保留原查询和金样；仍因平台功能不支持而失败。
- 原19121–19124 mock由另一工作树的旧进程占用，且pid文件错位，重启后实际仍使用旧计数。原AI并发/共享端口结果不作为验收。
- AI隔离到19221–19224，逐版本串行。native mysqltest因打印的URL端口与金样不同返回非零；只把URL中的测试端口映回原端口后，5份完整.reject输出都精确等于原金样。没有修改、删改任何断言或金样文件。

## 脚本与资料

`/Users/xiazihan/seekdb-work/results/fts-opt-v4-20261106/` 保存所有原始数据和可重用driver：

- `verify_v4.py`：attach live / baseline / accept-fixed / ai-isolated / ab / ab-more / aa。
- `semantics.sh`、`run_suite_v4.sh`：已改成仅连接现有实例，不再占用2914启动新进程，不再使用弃用目录；语义比较v3与v4不同文件，不自比较。
- `probe_h3_edges.py`：独立实例边界探针。
- `integration-ab-summary.json`：六对墙钟、instructions/cycles、RSS、均值/标准差、A/A、回归与明确门禁状态。
- `summarize_integration.py`：重算证据。验收JSON另存仓内 `v4-integration-acceptance-evidence.json`。
- 两份Linux增量补丁：`CHANGES_V3_TO_V4_FIXED_2026-11-06.patch`、`CHANGES_FDB_TO_V4_FIXED_2026-11-06.patch`，仅已提交源码，不包含本地macOS构建补丁。

## 恢复工作时的步骤与服务器切换条件

0. 当前暂停；收到用户新命令后再恢复，不自动重试SSH、构建、压测或推送。
1. 使用可用Linux节点，从上述精确v3/fdb底座和修正版提交重建，跑相同数据量、3套回归与平衡A/B。
2. 优先确认build_ik_content >1%读数、File SQL，以及长查询资源成本；没有授权改远端系统、网络、驱动，也未这样做。
3. 按用户要求复查legion：SSH两次重连与TCP 2222探测均超时；Tailscale status显示节点在线，ping经直接路径收到726ms回应。本机路由指向utun2。不能据此判定主机关机；其后用户恢复了节点，原2222端口已成功连接并启动独立验收。基线15个源码SHA匹配，完整编译到100%链接时LLVM报崩溃；此时`df`/`pgrep`执行报EIO，独立目录小文件写入/fsync探针报EROFS。只停止本轮tmux会话，未重挂载、修盘、重启或改其他实验。用户协调另一agent后再次复查，故障仍在并已通知；没有可验收Linux二进制，也未收集LinuxA/B样本。没有改网络配置，也未读取或复制私钥/密码/网络凭据。详情见results目录下`legion-connectivity-20261106.json`。
4. 全面验收后，按用户选择将2914短暂重启为修正版，保留同一数据目录并保持运行；**目前未切换**。
5. 原H3 PR #13不含本地194150497修正，本轮未更新任何远端PR或分支。不能把旧CI通过视为本轮修正版验收。

本机收尾状态：修正代码已在194150497；本报告、证据JSON、两份源码补丁和计划一并本地保存。工作树保留两个未提交的File SQL macOS兼容补丁；不包含在本轮提交中。Linux失败日志没有作为通过证据，Windows宿主磁盘是否已满尚未核实。
