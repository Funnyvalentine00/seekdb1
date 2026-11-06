# H2 DaaT 批量投影 Implementation Plan

> 目标：减少 `ObSRDaaTIterImpl::project_results` 每批逐行状态维护。按阶段独立 commit、独立 A/B；阶段 1 未过门禁不进入阶段 2。

**Goal:** 在不改变任何外部语义的前提下，把 `project_results` 中表达式级标志与 evaluated bit-vector 的逐行更新改为批次级一次更新。

**Architecture:** 阶段 1 仅动 `src/storage/retrieval/ob_sparse_daat_iter.cpp`：将 `set_evaluated_projected` 移出循环，并用现有 `ObBitVector::set_all(0, count)` 一次性置位 `[0, count)`。阶段 2 再评估 `locate_datums_for_update` 与数组裸指针，独立提测。

**Tech Stack:** C++20；macOS arm64 本地筛选；Linux CI/测评节点最终验收。

**Spec:** `/Users/xiazihan/seekdb-work/results/FTS_IDEATION_REVIEW_2026-11-06.md` H2。

## Constraints and review focus

- 只改既有文件，禁止新增/删除 `.cpp`/`.h`；不碰 benchmark、计时、SQL、数据量、`.result`。
- 本轮只允许 `src/storage/retrieval/ob_sparse_daat_iter.cpp`；不改头文件、不扩大对象。
- `ObExpr::set_evaluated_projected` 是**表达式级**标志；evaluated bit-vector 是**逐行**标志。批量置位只允许覆盖 `[0, count)`，保留区间外状态。
- 不得未经证明用 `locate_batch_datums()[i]` 替代 `locate_datum_for_write`；阶段 2 必须单独验证。
- 保留 scalar 分支、doc-id 缓存所有权、`OB_ITER_END` 与错误行为。
- 任一官方指标相对基线退化 `>1%` 立即回滚；hits 必须精确 `8001/11000/7332/20`。

## Recheck evidence（2026-11-06，clean 二进制）

对照二进制：`seekdb-ik-clean-baseline`（SHA256 `dad8dbecc6502872ef9aa0a3f6a1caa4f2580632f1fed348f6c85c39ec2b8653`，源码等价 `7d04fbbe8`）。20k 文档、三个官方 FTS 索引；macOS `sample` 8s/条；`ReqWorker` 忙样本为分母。

| 查询 | project_results self | inclusive | 结论 |
|---|---:|---:|---|
| query_cn | 2.3% | 4.0% | 目标 |
| query_beng | 2.0% | 3.2% | 目标 |
| query_mixed | 2.4% | 3.6% | 目标 |
| query_limit | 0.2% | 0.3% | BMW 路径，不适用 |

query_cn 的 project_results 行级样本（258 个 inclusive 样本）：

| 行 | 代码 | 样本 | 阶段 1 可去除 |
|---|---|---:|---|
| 444 | id `locate_datum_for_write` | 26 | 否（阶段 2） |
| 445 | `set_datum_func_` | 62 | 否 |
| 446 | `id_evaluated_flags.set(i)` | 10 | **是** |
| 447 | id `set_evaluated_projected` | 37 | **是** |
| 453 | relevance `locate_datum_for_write` | 26 | 否（阶段 2） |
| 454 | `relevance_proj_datum.set_double` | 24 | 否 |
| 455 | `relevance_evaluated_flags.set(i)` | 25 | **是** |
| 456 | relevance `set_evaluated_projected` | 48 | **是** |

阶段 1 可去除 query_cn 的 120/258 = 46.5% project_results inclusive，约 **1.85% 全进程忙样本**；query_beng/mixed 同量级（50.8% / 50.4%）。旧 11-02 profile 亦为 self 3.2%/2.6%/2.4%/0.2%，方向一致；旧 profile 只作定位，不作收益背书。

### 调用次数与 batch 分布（临时计数器实测，5000 条 SQL/查询）

| 查询 | calls/query | 满批 256 | 尾批 | 平均 rows/call |
|---|---:|---:|---:|---:|
| query_cn | 32 | 31 | 65 | 250.0 |
| query_beng | ~43 | 42 | 248 | ~255.8 |
| query_mixed | ~29 | 28 | 164 | ~252.9 |
| query_limit | 0 | - | - | BMW 路径 |

`max_batch_size_ = eval_ctx->max_batch_size_ = 256`。计数器源码已回退，补丁仅存 `results/h2-daat-batch-20261106/h2-instrumentation.patch`。

## Task 1: 阶段 1 实现与语义等价

- [ ] 仅改 `project_results` 向量化分支：
  - id 循环内删除 `id_evaluated_flags.set(i)` 与 `id_proj_expr->set_evaluated_projected(*eval_ctx)`；
  - 循环后执行 `id_evaluated_flags.set_all(0, count)` 与一次 `set_evaluated_projected`；
  - relevance 循环同样处理。
- [ ] 保持 `guard.set_batch_idx(i)`、`locate_datum_for_write`、`set_datum_func_`、`set_double` 原样。
- [ ] `git diff -U0` 审计：改动只在 `project_results`，未触碰其他函数。
- [ ] Release 构建成功；封存二进制并记录 SHA256。

## Task 2: 正确性差分

- [ ] 同一 20k 数据集上，clean 与 candidate 各起独立实例（顺序执行，避免同目录并发）。
- [ ] 覆盖：
  - `LIMIT 1/63/64/65/255/256/257`（跨满批与尾批边界）；
  - `LIMIT ... OFFSET ...`；
  - 只输出 id、输出 id+MATCH 分数、`COUNT(*)`；
  - 同一 SQL 重复执行/重新执行（rescan）；
  - 官方四条查询与 hits。
- [ ] 完整结果集逐字节 diff（含 6 位小数 BM25）；错误码与零行结果一致。
- [ ] `fts_index` 14/15（仅已知 EXPLAIN 差异）；tokenize 21 例不变（本改动不应影响 tokenize）。

## Task 3: 性能筛选

- [ ] 先做 query-focused A/B：`SKIP_LOAD=1 ROUNDS=1 QUERY_ROUNDS=200 SAMPLES=3 WARMUP=30`，3 对交替 clean/candidate；每对核对 hits。
- [ ] 若 query 类均值改善 >1% 且单项无 >1% 退化，再跑完整官方负载 3 对（build+tokenize+query）。
- [ ] 记录 `μ/σ`、配对改善、峰值 RSS；`ab_instr.py` 服务端指令数作为低噪声旁证。
- [ ] 任一项退化 >1% → 回滚并记录负结果。

## Task 4: 阶段 2（仅阶段 1 通过后）

- [ ] 用 `id_proj_expr->locate_datums_for_update(*eval_ctx, count)` 批量恢复 datum 指针；`is_batch_result()` 为 false 时保留原逐行路径。
- [ ] 用 `buffered_domain_ids_.get_data()` / `buffered_relevances_.get_data()` 替代逐行 `operator[]`（bounds check 在批量样本中占约 0.83%）。
- [ ] 独立 commit、独立 A/B；若最终 diff 只剩裸指针替换且收益 <1%，按 H2 文档淘汰。

## Execution decisions

- 用户在交接中明确要求从 H2 开始并先复核热点，视为已授权执行；不再额外等待计划审批。
- 当前分支 `feat/fts-daat-batch-projection`，基于 `c9e536002`（代码等价 `7d04fbbe8`）。
- 本机只做筛选；最终以官方 Linux CI `/benchmark` 为准。
- 不自动 push / 不提测，除非用户明确要求。

## Outcome

Both stages kept and committed separately.

- Stage 1 `9fa0416e5`: hoisted the expression-level projected flags and replaced per-row evaluated-bit sets with `set_all(0, count)`.
- Stage 2 `f8819ff6d`: bulk `locate_datums_for_update` plus raw array data pointers; legacy per-row path retained for non-batch expressions.

Measured against clean `7d04fbbe8` (binary `dad8dbe…`):

- Server instructions: stage 1 `-3.10% / -2.83% / -2.67%` and stage 2 `-5.36% / -4.91% / -4.61%` for query_cn/beng/mixed; query_limit control `-0.07%`; round-to-round spread below 0.13%.
- Wall clock (3 alternating pairs): stage 2 paired medians `+4.08% / +3.52% / +4.75%`; query_limit control `+0.72%`; query class mean `+2.73%`.
- Profile: `project_results` inclusive `4.0% -> 2.3% -> 1.7%`; self `2.3% -> 1.4% -> 0.6%`.
- Correctness: 1734-line differential byte-identical (SHA256 `539253c0…`); hits exact; fts_index 14/15 with only the known `partitioned_simple_query` EXPLAIN diff; tokenize 21 cases identical.
- Binary SHA256: stage 1 `8dd7dc33362ab37869e8a389dddf4e43321113cedb2b1b83c8c6f93a7a638cf7`; stage 2 `26ca0ebb86d66cef26dec73d18e5febd2278c467709040d0526b8bb8f20dcbe3`.

Limits: macOS local screening only; Linux CI, file_sql and ai_column remain pending. See `handoff/fts-tuning/DAAT_BATCH_PROJECTION_2026-11-06.md` and the evidence JSON.

