# Linux v4 修正版验收记录（2026-11-06）

## 1. 施工摘要与验收结论

用户恢复 legion 的 2222 入口后，已使用 `xiazihan` 重新检查环境、完成 v3/修正版构建、三套回归、A/A、六对平衡 A/B、语义及边界差分。**本轮验收执行完成，但验收未通过**，不推送，不切换 2914。

四项 query 的配对改善中位数平均为 **23.77%**，不是官方平台得分。仍有三项资源/性能门禁未满足：`build_ik_content_sec` 慢 **1.50%**，`tokenize_beng_avg_ms` 慢 **2.14%**，峰值 RSS 中位数增加 **2.67%**。`fts_index` 两版均为 **13/15**，低于只豁免 `partitioned_simple_query` 的 14/15 要求。不能用查询收益、A/A 噪声或基线同样失败来豁免这些门禁。

用户指定的完整施工规范已写入根目录 `AGENT_ENGINEERING_STANDARD.md`，由 `AGENTS.md` 必读入口引用，提交 `e0eb06c82`。本轮没有新增产品 C++ 修改；候选代码仍为 `194150497`。两个 File SQL macOS 兼容补丁继续留在工作树且未提交。

## 2. 源码、二进制与环境

| 项目 | 已核实记录 |
| --- | --- |
| 基线代码 | `7accd1ace8ee5d035b3ef8aaf426c46b1d0a595e` |
| 修正版代码 | `194150497aca9a69e395317bd6d5a64f26340a61` |
| Linux v3 SHA256 | `2cae5186cb29e6ffca0db46b9ba3dbb2455005f61a5966114c5a77ea61db0788` |
| Linux 修正版 SHA256 | `9544ca38c4d6cc336adb079f362eff1fa220739d58ce70bc2d0f1d948d5ade32` |
| 节点/用户 | `ssh -l xiazihan legion`，原 2222 端口，`/home/xiazihan` |
| 系统/CPU | Ubuntu 22.04 WSL2，Linux 6.18.33.2-microsoft-standard-WSL2 x86_64；Intel Core Ultra 9 275HX |
| 编译工具 | clang 17.0.6，CMake 3.22.1；系统 Rust 1.75.0；项目已有固定 Rust/Cargo 1.98.1 |
| 本轮目录 | `/home/xiazihan/benchmark/fts-v4-integration-20261106` |
| 缓存复用 | `/home/xiazihan/benchmark/seekdb-verify-20261104/deps-shared/3rd` 及该目录下 `toolchain/{cargo,rustup}` |

两版各 15 个相关源码 SHA 均与本地 Git 导出值一致；`logs/source-{v3,fixed}.json`、`source-manifest.json` 和二进制完成标记已封存。项目工具链版本来自 `src/rust/rust-toolchain.toml`，没有安装或升级 Rust/CUDA/系统依赖，没有改变系统默认工具链。

make、Cargo 和 lld 均限制为 2 并发，lld `--threads=2` 已核对生成的链接命令。采用单实例、端口 2917；两版配置均为 `memory_budget=2G`、`vector_memory_limit=512M`、`datafile_size=2G`、`datafile_maxsize=2G`、`log_disk_size=2G`。实际使用 `memory_budget`，没有将已弃用的 `memory_limit` 当成生效限制。

恢复期间 WSL 再次重启：97% 构建日志为 Terminated/Hangup，但旧 EXIT trap 错写 `build.rc=0`。最小复现中旧脚本进程返回 -1、标记 0；修复后返回和标记均为 129。已增加 HUP/INT/TERM 处理，只有两份可执行二进制与 SHA 全部匹配才生成 `build.complete.json`；验证驱动同时要求该标记和实际二进制 SHA。重连后复用已编译缓存完成构建，没有重复 clone、下载或全量重建。

## 3. 测试报告

### 正确性与覆盖

- 四条官方 hits：16 次完整报告（12 次 A/B、2 次 A/A、2 次初始验收加载）全部精确为 **8001/11000/7332/20**。
- MATCH 边界：每版 9 组全部成功，stdout/stderr/返回码逐字节一致，包括 lookup projection、长查询、tiny partition、255/256/257/1024 词、prepared short-long-short、强制 PRIMARY lookup。
- projection、score-controls：两版执行成功，完整输出一致。
- Tokenize：原批处理在第 6 行 `NULL` 输入的 1235 错误处提前结束，后续 SELECT 原先未覆盖。已保留原日志并用独立驱动逐条执行原有 **22 条 SELECT**，两版输出/错误/返回码全部一致；各 21 条成功，`NULL` 输入均报相同 1235。没有修改原 SQL；不能把原批处理误记为完整语料通过。

### 三套回归（保留原始执行与后续复测）

| 执行 | v3 | 修正版 | 判断 |
| --- | --- | --- | --- |
| 首次 `fts_index` | 13/15 | 13/15 | 两版均失败 `basic_dml`、`partitioned_simple_query`；硬门禁未过 |
| 首次 `file_sql` | 1/1 | 1/1 | Linux 已覆盖；仅 fixture 部署改为本地拷贝，SQL/金样未改 |
| 首次 `ai_column` | 4/5 | 5/5 | v3 的 `retry_strict` 初始等待未就绪；原失败保留 |
| 全新实例、完整原生 AI 复测 | 5/5 | 5/5 | 原端口、无金样归一化、无断言改动 |

首次三套合计 **37 通过 / 5 失败 / 0 跳过**；独立 AI 复测另计 **10 通过 / 0 失败**，不覆盖首次失败。Linux instructions/cycles **未执行**：缺少匹配 WSL 内核的 perf 工具，未为此安装新依赖。官方平台 CI **未执行**。2914 切换 **未执行**，验收条件未满足。

FTS 的 `cn_word`、`partitioned_simple_query` 临时驱动将连接用户 admin 适配为已有 root；没有创建账号、改产品权限或修改仓库测试/金样。File SQL 仅替换 SSH/scp fixture 部署步骤，保留全部测试查询和原金样。各适配都记录在 JSON 的 `adapters` 中。

### 已定位的失败与证据边界

`basic_dml` 在 1598 行引用未解析的 `$rowkey_doc_table_id`。此前本机元数据探针确认删表后 table_id 变化，但刷新 ID 后，无显式主键的表仍没有 index_type=11 子表。代码证据为 `ObFtsIndexBuilderUtil::determine_docid_type` 为隐藏主键选择 `HIDDEN_INC_PK`，而 `ob_index_builder.cpp` 只为 `TABLET_SEQUENCE` 创建 rowkey-doc 子表。这解释了该固定底座与旧 fixture 的假设不符；本轮未改 DDL 行为或金样来强行通过。`partitioned_simple_query` 为 EXPLAIN 参数编号差异。两项在精确 v3 和候选均复现，仍计入失败。

首次 AI v3 失败的差异仅为 `initial_results_ready` 从预期 1 变为 0，其余完整结果一致。已排除“行内 `--real_sleep` 未执行”的假设：5 次 0.1 秒等待，行内/换行两种写法均约 0.508 秒且返回 0。独立全套复测两版 5/5；首次偶发失败的根因**尚未证明**，不能记为修复完成。

## 4. 六对 A/B、波动与 RSS

官方 `tools/benchmark/fts_large_bench.sh`、数据和 SQL 均未改。参数：ROWS=20000、BATCH=500、ROUNDS=3000、QUERY_ROUNDS=200、SAMPLES=3、WARMUP=30、SKIP_LOAD=0。预先安排 3 对 v3 先、3 对候选先；每次全新数据目录，单实例串行。

改善定义为 `100*(1-candidate/base)`，正值更快。耗时列为六次运行值的均值±样本标准差；最后一列为六个配对改善的中位数，不能用前两列直接重算。全部样本、版本中位数及另一统计口径均在 JSON 中，无样本剔除。

| 指标 | v3 均值±标准差 | 修正版均值±标准差 | 配对改善中位数 |
| --- | ---: | ---: | ---: |
| build_ik_all_sec | 5.036500 ± 0.043205 | 5.036833 ± 0.045473 | -0.01% |
| build_ik_content_sec | 3.682667 ± 0.059204 | 3.820667 ± 0.214888 | **-1.50%** |
| build_beng_en_sec | 2.127667 ± 0.002160 | 2.132667 ± 0.006439 | -0.12% |
| tokenize_ik_avg_ms | 0.249833 ± 0.013810 | 0.237633 ± 0.011883 | +2.81% |
| tokenize_beng_avg_ms | 0.195767 ± 0.013052 | 0.197117 ± 0.011625 | **-2.14%** |
| query_cn_avg_ms | 1.125183 ± 0.032667 | 0.782767 ± 0.019001 | +30.41% |
| query_beng_avg_ms | 1.553233 ± 0.028306 | 1.079983 ± 0.013863 | +30.57% |
| query_mixed_avg_ms | 1.265383 ± 0.018747 | 0.894817 ± 0.014109 | +28.91% |
| query_limit_avg_ms | 1.360900 ± 0.052335 | 1.276833 ± 0.024781 | +5.20% |
| build_total_sec（诊断项） | 10.856167 ± 0.090641 | 10.998500 ± 0.233866 | -0.59% |

同二进制 A/A：build_ik_content 约 +0.03%，tokenize_ik -2.59%，tokenize_beng +3.91%，四项 query -1.48/-1.11/-2.17/-0.80%。Tokenize 噪声明显；两项 >1% 的不利 A/B 读数仍记录为未过，尚不能据此认定精确的代码根因。

Linux `/proc/PID/status` 的 VmHWM：v3 六次中位数 **407.86 MiB**，候选 **418.76 MiB**，增加 **10.90 MiB / 2.67%**。这是本次完整短查询 benchmark 的峰值指标，不与 macOS 的 RSS 混算，也不把它与此前 1024 词 DaaT 的额外内存直接等同。长查询内存随 tokens×batch_size 增长，超过 1024 词尚未验证。

## 5. 资源收尾与数据保护

阶段前检查和 30 秒监测阈值：Windows D 至少 30 GiB、Linux 至少 15 GiB、可用 RAM 至少 3 GiB；本轮未触发阈值。最后检查 Windows D **140.44 GiB**、Linux **532.90 GiB**、可用 RAM **14.25 GiB**。Windows 发行版位置已核实为 `D:\WSL\Ubuntu-22.04`，不只依赖 Linux 虚拟磁盘的可用量判断宿主空间。

仅回收本轮 marker 标记的自有临时数据目录：精确子进程停止后，保存结果与各内部日志末 512 KiB，再回收数据。所有原始用户数据、其他实验目录、Git 历史和 benchmark 输入保留。最终 data 目录为空、2917/2918 无监听、当前用户无残留 tmux 会话；RUN 合计 **9.5 GB**，保留构建缓存和两份 SHA 验证二进制。未修改 SSH、网络、系统组件或其他工作树。

本机 23:42（+08:00）实测 2914 仍由 PID **31133** 监听，未重启/切换。它仍是原 v4，已知 MATCH 边界错误尚在该常驻实例中；不能将已验证修正版与常驻进程混为一谈。

## 6. 可复现资料、Checkpoint 与回滚

- 产品修正：`194150497`；规范及恢复检查点：`e0eb06c82`；本报告所属提交封存 Linux 验收结果。阶段详见 [CHECKPOINTS_2026-11-06.md](CHECKPOINTS_2026-11-06.md)。
- 仓内精简证据：`linux-v4-20261106/`，包括汇总 JSON、16 份原始官方报告、RSS、首次失败日志（无损 gzip，原始字节不改）、源码/二进制 SHA、实际工具链、资源策略、验证/构建驱动和 `manifest.sha256.json`。
- 本机完整目录：`/Users/xiazihan/seekdb-work/results/fts-opt-v4-20261106/linux/`。`evidence-linux-recovered.tar.gz`、`supplement-evidence.tar.gz`、`tokenize-evidence.tar.gz` 保存已取回原始结果/日志、复现输入和脚本；SHA256/大小见仓内 `evidence-archives.json`。未打包数据库和大型二进制；内部服务器日志保留限定尾部。
- 远端源码/缓存/二进制：上述 RUN；二进制位于 `saved-binaries/seekdb-{v3,fixed}`，禁止只按文件名认定版本。

在仓库根目录可重算精简证据（只读取文件，不连接节点）：

```sh
python3 handoff/fts-tuning/linux-v4-20261106/summarize_linux.py
```

精简目录保留了重算所需原始报告，输出必须仍为 `full_acceptance: False`。完整差分输出见三个本机档案，按相对路径解包至临时目录的 `collected/` 后核对各文件 SHA；勿覆盖已有实验结果。

若后续需复测：先检查远端资源/实例/源码 SHA，复用保存的两份二进制；复制并更换验证驱动的结果目录和 tag 后执行 `accept|aa|ab`，保持官方参数不变。AI 隔离复测在每版全新实例中只执行完整 `suite(..., 'ai_column')`。Tokenize 逐条补测驱动为 `probe_tokenize_statements.py`。**不要直接重跑 `resume_build.sh`**：它是本次恢复过程的记录，会从 v3 应用候选补丁，当前源码已经是候选；盲目重跑会失败或破坏版本归属。需要重建时先按保存的 fdb→v3→fixed 补丁和 SHA 在隔离源码目录恢复指定身份。

回滚代码前封存工作树差异，使用独立 revert 提交；二进制回滚使用已保存 SHA 对应版本及匹配数据目录，不用旧版本打开升级过的数据。证据/规范文档可独立 revert，但不删除原始实验档案。当前没有自动回滚或删除用户数据。

## 7. 下一阶段

优先定位候选的 RSS 增量和 `build_ik_content` 不利读数：保留当前精确 v3 与候选，逐项隔离 H2/H3/H7 的内存和建索引路径，固定负载后重测 Tokenize；避免将 WSL 墙钟噪声推断成已知根因。每个消融版本先做正确性差分，再按同一六对模板验收，保留负例。

另需厘清 fdb 底座 FTS fixture 的正式兼容要求及 AI 初始等待偶发失败，不能直接删除测试或修改金样以满足门禁。在门禁实际通过前保持 **不推送、不切换 2914、不宣称全面验收**。
