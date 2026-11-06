# H3 命中专用 MATCH：跳过评分与回滚边界（2026-11-06）

## 1. 结论

保留。代码提交 `853ccd8ed1366090f52952913c5b774683cdb02f`（用户已批准 optimizer 扩范围）。

以 H2 stage 2（`f8819ff6d`）为基线，本机墙钟 3 对配对中位数：

| 指标 | H3 vs H2 |
|---|---:|
| query_cn | **+19.50%** |
| query_beng | **+19.93%** |
| query_mixed | **+20.42%** |
| query_limit（对照，LIMIT 不适用 H3） | +0.79% |
| query 类均值 | **+14.87%** |

四条 hits 全部 `8001/11000/7332/20`；1734 行投影差分 + 17 行扩展差分逐字节一致；`fts_index` **14/15**（仅已知 `partitioned_simple_query`）。服务端指令数在 beng 上稳定 −25.3%；cn/mixed 的指令采样受后台线程与基线计划波动影响，故以墙钟定案。

## 2. 机制

- 恢复并补全 `ObTransformUtils::check_need_calc_match_score`：遍历语句 relation exprs，判断 MATCH 是否只出现在命中上下文（裸 MATCH、`BOOL(MATCH)`、`MATCH > 0`）；出现在 SELECT/ORDER/HAVING/聚合参数或 `MATCH > 非零` 时保持评分。
- `ObLogPlan::prepare_text_retrieval_info` 设置 `tr_info.need_calc_relevance_`；codegen 已有该标志：无评分时不生成 BM25 relevance expr、avg doc len、inv idx agg、doc agg 与 block max 扫描。
- 补齐无评分执行路径：
  - `ObSRDaaTNoScoreRelevanceCollector`：每个 merge 出的 doc 返回 relevance=1、valid=true；
  - legacy DaaT 跳过 doc_agg ctdef / token doc cnt 估计 / match_filter；
  - token iter 无评分时仍分配 skip 向量，并跳过 `estimate_token_doc_cnt`；
  - 无评分 TAAT 返回 `OB_NOT_SUPPORTED`，官方查询走 DaaT。
- 语义依据：自然语言 MATCH 的命中集合是各 token posting 的并集；无评分时索引返回的 doc 集合即该并集，`MATCH`/`MATCH > 0` 的真值不受影响。

## 3. 首轮适用范围（保守）

仅当：`ObSelectStmt`、单表、非 join、无子查询、无 window、自然语言模式、**多列 MATCH**、无 LIMIT/OFFSET、MATCH 只在命中上下文。否则保持原评分路径。

- **单列 MATCH 不启用**：`fts_index/simple_query` 金样包含 `calc_relevance=true` 的 EXPLAIN，禁止改 `.result`；单列命中查询保持评分，回归恢复 14/15。
- 布尔/查询扩展/短语/ES match、LIMIT/topk、子查询、join、window、函数索引 lookup 均不启用。
- 未处理 `minimum_should_match` 的 no-score 语义；官方自然语言查询不涉及。

## 4. 正确性

- EXPLAIN 上下文：官方多列 COUNT `calc_relevance=false`；分数输出/ORDER BY 分数/布尔模式/LIMIT 均 `true`；单列 COUNT `true`；额外 `category > 0` 过滤为 `false`（非 match 条件下推为普通 scan filter，数据差分一致）。
- 差分：`diff_projection.sql` 1734 行、`diff_intcmp_extra.sql` 345 行、`diff_h3_extra.sql` 17 行（EXISTS、布尔、阈值、额外过滤、分数排序、prepared 重执行、多 token），clean / H2 / H3 逐字节一致。
- hits：六份墙钟报告全部 `8001/11000/7332/20`。
- 回归：`fts_index` 14/15，唯一失败为基线同样存在的 `partitioned_simple_query`；`simple_query` 在加多列 guard 后通过；`cn_word`/`basic_dml`/DDL/parser_properties 通过。
- RSS：H3 721–728 MB，H2 725–729 MB，无系统性上升。

## 5. 性能

### 墙钟（SKIP_LOAD=1，QUERY_ROUNDS=200 × SAMPLES=3，3 对交替）

| 指标 | 三对改善 | 配对中位数 |
|---|---|---:|
| query_cn | +19.50 / +23.61 / +18.50 | **+19.50%** |
| query_beng | +17.98 / +21.41 / +19.93 | **+19.93%** |
| query_mixed | +18.42 / +21.17 / +20.42 | **+20.42%** |
| query_limit（对照） | +0.79 / +2.00 / +0.63 | +0.79% |

query 类均值配对中位数 **+14.87%**。

### 指令数（`proc_pid_rusage.ri_instructions`，3 轮 × 5 reps × 200 SQL）

| 指标 | 分轮中位数改善 | 备注 |
|---|---:|---|
| query_cn | +18.4% | 基线轮含后台指令离群 |
| query_beng | +25.3% | 轮间稳定 |
| query_mixed | +58.1% | 基线轮计划/后台噪声，墙钟 +20.4% |
| query_limit（对照） | -0.2% | 未改路径 |

## 6. Linux

- H2 的 Linux 3 对 A/B 已完成：query 配对中位数 **+5.87%/+5.15%/+6.06%**、query_limit +0.81%，hits 全对；tokenize_ik 配对中位数 −1.68% 属未改代码路径的噪声（三对 −2.66/−1.01/+7.65，p3 base 明显受系统波动）。
- H3 的 Linux guard 构建已启动，随后 SSH/Tailscale 链路再次超时，**H3 Linux A/B 与回归待链路恢复后执行**；本轮不宣称 Linux 已验证。

## 7. 限制

- H3 首轮只覆盖多列自然语言命中查询；单列、布尔、LIMIT/topk、函数索引、TAAT 无评分路径未覆盖。
- 无评分 collector 未实现 `minimum_should_match` 阈值；官方查询不涉及。
- macOS 本机筛选；Linux CI `/benchmark` 与 file_sql/ai_column 未跑。
- 指令采样受后台线程影响，query_mixed 的指令改善不可直接引用；墙钟结果与差分是主证据。

## 8. 证据

- 证据 JSON：`handoff/fts-tuning/skip-match-score-evidence.json`（40 位 commit、workload、μ/σ、A/A 轮间、A/B、差分 SHA、回归、EXPLAIN）。
- 原始数据：`~/seekdb-work/results/h3-skipscore-20261106/`（`h3.patch`、`instr-runs.json`、`bench-runs.json`、`p*/`、差分、EXPLAIN、回归、`wall-pre-guard/`）。
- 二进制：H2 `seekdb-h2-stage2-20261106`（`26ca0ebb…`）、H3 guard `seekdb-h3-skipscore-20261106`（`3edc082d…`）。
- Linux H2 证据：`~/seekdb-work/results/h2-daat-batch-20261106/linux-ab/`。
- 计划：`docs/superpowers/plans/2026-11-06-skip-match-score.md`。
