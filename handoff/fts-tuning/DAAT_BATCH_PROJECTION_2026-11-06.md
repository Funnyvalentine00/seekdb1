# H2 DaaT 批量投影：实现与验证（2026-11-06）

## 1. 结论

保留，两个独立提交：

| 阶段 | commit | 内容 |
|---|---|---|
| H2 计划 | `edd26ece78adf64ea4a5bb17aefb74a39dcebbc2` | 本计划文档 |
| H2 阶段 1 | `9fa0416e57600fb07802ac667a091f9e4ccd92c7` | projected 标志移出循环 + evaluated 位区间置位 |
| H2 阶段 2 | `f8819ff6dffab44c4ba7cc77dced2493cec5830d` | 批量 datum 定位 + 数组裸指针 |

相对 clean 基线 `7d04fbbe868a4e42c764173a36ba3053e0869abe`，阶段 2 的服务端指令数改善：`query_cn -5.36%`、`query_beng -4.91%`、`query_mixed -4.62%`；`query_limit`（BMW 对照）`-0.07%`。三轮轮间波动 <0.13%，远低于效应。墙钟 3 对配对中位数为 `+4.08% / +3.52% / +4.75%`，`query_limit +0.72%`（对照，噪声）。未改 build/tokenize 路径。

## 2. 热点复核（clean 二进制）

二进制：`seekdb-ik-clean-baseline`，SHA256 `dad8dbecc6502872ef9aa0a3f6a1caa4f2580632f1fed348f6c85c39ec2b8653`。20k 文档、三个官方 FTS 索引，macOS `sample` 8s/查询，`ReqWorker` 忙样本为分母。

| 查询 | `project_results` self | inclusive |
|---|---:|---:|
| query_cn | 2.3% | 4.0% |
| query_beng | 2.0% | 3.2% |
| query_mixed | 2.4% | 3.6% |
| query_limit | 0.2% | 0.3% |

query_cn 行级归因（258 个 inclusive 样本）：`set_evaluated_projected` 两行 85 个（1.31%）、逐行 bit-set 35 个（0.54%）、`locate_datum_for_write` 52 个（0.80%）、数组 `at` 47 个（0.72%）、`set_datum_func_`/`get_datum` 约 82 个（1.26%）。

临时计数器实测（5000 条 SQL/查询，源码已回退，补丁见证据目录）：

| 查询 | calls/query | 满批 256 | 尾批 | 平均 rows/call |
|---|---:|---:|---:|---:|
| query_cn | 32 | 31 | 65 | 250.0 |
| query_beng | ~43 | 42 | 248 | ~255.8 |
| query_mixed | ~29 | 28 | 164 | ~252.9 |
| query_limit | 0 | - | - | BMW 路径不调用 |

## 3. 改动

仅 `src/storage/retrieval/ob_sparse_daat_iter.cpp::ObSRDaaTIterImpl::project_results`，属于允许路径；未改头文件、benchmark、SQL、金样。

**阶段 1**：`set_evaluated_projected` 是表达式级标志，移出两个逐行循环、每批调用一次；evaluated 位由逐行 `set(i)` 改为 `ObBitVector::set_all(0, count)`，只覆盖实际写出的 `[0, count)`。

**阶段 2**：`is_batch_result()` 为真时用 `locate_datums_for_update(count)` 一次恢复整批 datum 指针，并通过 `buffered_domain_ids_.get_data()` / `buffered_relevances_.get_data()` 读取缓存；非 batch 表达式保留原逐行 `locate_datum_for_write` 路径。`set_evaluated_projected` 与区间置位保持不变。

## 4. 正确性

- **差分 SQL**：hits 四条、`LIMIT 1/63/64/65/255/256/257`、`OFFSET 50`、越界 offset、空结果、id-only、id+BM25 分数、`ORDER BY score`、prepared 重复执行；clean / 阶段 1 / 阶段 2 输出均为 **1734 行、SHA256 `539253c0017d7375c482d2173b716d54d6cec5674d282597cd85de88613d0815`，逐字节一致**。
- **hits**：6 份阶段 2 A/B 报告全部 `8001 / 11000 / 7332 / 20`。
- **fts_index**：阶段 2 **14/15**，唯一失败为基线同样存在的 `partitioned_simple_query` EXPLAIN `:0/:1` 差异；`cn_word`、`basic_dml`、DDL、parser_properties 等均通过。基线同环境仅 `partitioned_simple_query` 失败。
- **tokenize**：21 例 stdout 与 stderr 与 clean 逐字节一致。
- **RSS**：基线峰值 495984/497184/452384 KB，候选 454752/494400/492112 KB，无系统性上升。

## 5. 性能

### 5.1 服务端指令数（`proc_pid_rusage.ri_instructions`，3 轮交替 × 5 reps × 200 SQL）

| 指标 | 阶段 1 vs clean | 阶段 2 vs clean | 轮间波动（base/cand） |
|---|---:|---:|---|
| query_cn | **-3.10%** | **-5.36%** | 0.13%/0.05% |
| query_beng | **-2.83%** | **-4.91%** | 0.09%/0.01% |
| query_mixed | **-2.67%** | **-4.61%** | 0.09%/0.03% |
| query_limit（对照） | -0.03% | -0.07% | 0.09%/0.08% |

阶段 2 比阶段 1 再降约 1.95–2.26 个百分点，方向与行级归因一致。

### 5.2 官方脚本墙钟（SKIP_LOAD=1，QUERY_ROUNDS=200 × SAMPLES=3，3 对交替）

| 指标 | 阶段 2 配对中位数 | 三对改善 |
|---|---:|---|
| query_cn | **+4.08%** | +1.95 / +5.69 / +4.08 |
| query_beng | **+3.52%** | +3.52 / +4.94 / +1.66 |
| query_mixed | **+4.75%** | +4.75 / +6.13 / +3.87 |
| query_limit（对照） | +0.72% | +0.72 / +1.24 / -1.62 |

query 类均值配对中位数 **+2.73%**。`query_limit` 单对出现 -1.62% 属墙钟噪声：该路径为 BMW，`project_results` 调用次数为 0，指令数对照仅 -0.07%。阶段 1 墙钟噪声更大（对照 -0.11% ~ +2.50%），故以指令数定案。

### 5.3 机制确认（profile）

`project_results` inclusive：clean **4.0%** → 阶段 1 **2.3%** → 阶段 2 **1.7%**；self：2.3% → 1.4% → 0.6%。阶段 2 剩余成本主要是 `set_datum_func_` 间接调用（38/5904 样本）、批量 `locate_datums_for_update`（40 样本）与 `set_double`（14 样本），属于必要写入。

## 6. 限制与未完成

- 本机 macOS arm64 仅作筛选；未跑 Linux CI `/benchmark`，未在测评节点复现。
- 只跑了 `fts_index`；`file_sql`、`ai_column` 未跑（本改动不涉及对应路径，但正式提测前必须补）。
- 阶段 2 的非 batch 回退分支未被测试路径覆盖；正式提测前建议补一个非 batch 投影用例，或经代码审查确认该分支不可达后删除。
- `query_limit` 的 BMW top-k 路径未优化；query 类目标 45% 仍需后续方向（H3/BMW）。

## 7. 证据索引

- 证据 JSON：`handoff/fts-tuning/daat-batch-projection-evidence.json`（含 40 位 commit SHA、workload、指令/墙钟 μ 与轮间范围、A/A 对照、差分 SHA、回归结果）。
- 原始数据：`~/seekdb-work/results/h2-daat-batch-20261106/`（stage1/、stage2/、diff-*.out、h2-instrumentation.patch、prof/h2-stage1、prof/h2-stage2）。
- 计时代码：`stage2/instr_h2.py`、`stage2/bench_h2.py`；回归：`run_regress_h2.sh`。
- 二进制：clean `dad8dbe…`、阶段 1 `8dd7dc3…`、阶段 2 `26ca0eb…`（均存于 `~/seekdb-work/saved-binaries/`）。
- 构建日志：`~/seekdb-work/work-logs/h2-stage1-build.log`、`h2-stage2-build.log`。
