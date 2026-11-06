# H3 Hit-Only MATCH: skip relevance scoring Implementation Plan

> 目标：在“只需命中判断”的自然语言 MATCH 查询中关闭 BM25 评分与相关扫描，保持命中集合与全部外部结果不变。按单点独立 commit、独立 A/B。

**Goal:** 为 `COUNT(*) WHERE MATCH(...)` 这类不使用分数的查询建立通用的“只需命中”执行契约，跳过 BM25 估算、评分表达式、inv/doc agg 扫描与分数复制。

**Architecture:** 恢复并补全 `ObTransformUtils::check_need_calc_match_score`（遍历语句 relation exprs 判定分数需求），在 `ObLogPlan::prepare_text_retrieval_info` 中设置 `tr_info.need_calc_relevance_`；codegen 已支持该标志。补齐无评分执行路径：legacy DaaT 使用 no-score collector、跳过 doc_agg/BM25 与 match_filter；token iter 跳过 token doc cnt 估计并按需分配 skip 向量。

**Tech Stack:** C++20；macOS arm64 本地筛选；Linux legion 节点与官方 CI 最终验收。

**Spec:** `/Users/xiazihan/seekdb-work/results/FTS_IDEATION_REVIEW_2026-11-06.md` H3；用户已批准 optimizer 扩范围。

## Scope and approvals

- 新增范围（用户批准）：`src/sql/optimizer/ob_log_plan.cpp`、`src/sql/rewrite/ob_transform_utils.{h,cpp}`。
- 既有允许路径：`src/observer/composition/retrieval/**`、`src/storage/retrieval/**`、`src/sql/das/iter/**`。
- 未改：benchmark、计时、SQL、数据量、`.result` 金样、parser、`.bzl`。
- `src/sql/das/iter/ob_das_iter_utils.cpp` 的模式强制被权限分类器拒绝，本轮未改；无评分 TAAT 返回 `OB_NOT_SUPPORTED`，官方查询走 DaaT。

## 首轮适用范围（刻意保守）

仅当以下全部成立时 `need_calc_relevance=false`：

- `ObSelectStmt`、单表、非 join、无子查询、无 window func；
- 自然语言模式（`NATURAL_LANGUAGE_MODE`），排除 BOOLEAN/QUERY EXPANSION/PHRASE/ES match；
- **多列 MATCH**（`match_columns.count() > 1`）；单列命中查询保持评分，避免 `simple_query` 金样中 EXPLAIN `calc_relevance` 注解差异；
- 无 LIMIT/OFFSET；
- 语句 relation exprs 中 MATCH 只出现在命中上下文（裸 MATCH/`BOOL(MATCH)`/`MATCH > 0`）；出现于 SELECT/ORDER/HAVING/聚合参数/`MATCH > 非零` 等即需要评分；
- 否则保持评分（保守）。

## 语义依据

- 自然语言 MATCH 的命中集合是各 query token posting 的并集；DaaT merge 返回同一集合。
- 无评分时 match filter 被忽略，索引返回的 doc 集合即命中集合；`MATCH > 0`/裸 MATCH 的真值不受影响。
- 无评分 collector 对每个 merge 出的 doc 返回 relevance=1、valid=true；`project_results` 写入 1（真值）。
- 任何需要分数的上下文（分数输出、排序、阈值、布尔模式、单列金样）由判定函数保持评分路径。

## Tasks

1. [x] 补全 wrapper 与 optimizer 下推；EXPLAIN 验证官方 COUNT `calc_relevance=false`，分数/ORDER/布尔/LIMIT 为 true。
2. [x] 修复无评分执行路径的空指针：doc_agg ctdef、token doc cnt 估计、skip 向量、match_filter。
3. [x] 语义差分：1734 行差分 + 345 行 intcmp 扩展 + 17 行 H3 边界（EXISTS/布尔/阈值/额外过滤/分数/prepared/多 token）全部逐字节一致；四条 hits `8001/11000/7332/20`。
4. [x] fts_index：加多列 guard 后预期 14/15（仅已知 partitioned_simple_query）。
5. [x] 本地指令/墙钟 A/B：H3 vs H2 stage 2；见结果。
6. [ ] Linux legion：构建 guard 版、3 对官方 A/B、fts_index；链路恢复后执行。
7. [ ] 证据 JSON、报告、独立 commit。

## Guard 说明

单列 MATCH 的命中查询保持评分路径，不是语义限制，而是首轮范围收窄：`fts_index/simple_query` 金样包含 `calc_relevance=true` 的 EXPLAIN 输出，禁止修改 `.result`；多列官方查询仍是优化目标。无评分 TAAT（>256 token/无 relevance projection 的路径）返回可控错误，未纳入本轮。

## Outcome

Kept. Code commit `853ccd8ed1366090f52952913c5b774683cdb02f`.

Local wall-clock paired medians versus H2 stage 2: query_cn **+19.50%**, query_beng **+19.93%**, query_mixed **+20.42%**, query_limit control +0.79%; query class mean **+14.87%**; hits exact. The 1734-line projection differential and 17-line extended differential are byte-identical; fts_index is 14/15 with only the known partitioned_simple_query failure. Server instructions are stable only for query_beng (+25.3%); cn/mixed samples contain background/plan noise, so wall clock is the primary evidence. Binary SHA256: H3 guard `3edc082d5ff431a1d18da1f3391b561ca7a7b8583c51476313f5a870447d68b7`.

Linux: the H2 A/B completed on legion (query +5.87/+5.15/+6.06%, query_limit +0.81%, hits pass); H3 Linux guard build started but the SSH/Tailscale link timed out, so Linux H3 A/B and regression remain pending. See `handoff/fts-tuning/SKIP_MATCH_SCORE_2026-11-06.md`.

