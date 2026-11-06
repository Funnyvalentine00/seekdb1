# FTS Issue #6 工程现状与外部创意请求（2026-11-05）

> 这份文档用于交给外部硅基智能做新方向头脑风暴。前半部分是事实、约束和已做实验，后半部分是希望外部模型重点思考的问题。**请勿把“已做过的尝试”当成未验证方向重复提出。**

---

## 1. 任务与计分

- 赛事：OceanBase / seekdb 数据库大赛，Issue #6 全文索引调优。
- 三类官方指标，共 9 项等权：
  - **Build 3 项**：`build_ik_all_sec`、`build_ik_content_sec`、`build_beng_en_sec`（另有 `build_total_sec` 用于诊断）
  - **Tokenize 2 项**：`tokenize_ik_avg_ms`、`tokenize_beng_avg_ms`
  - **Query 4 项**：`query_cn_avg_ms`、`query_beng_avg_ms`、`query_mixed_avg_ms`、`query_limit_avg_ms`
- 排名/得分为三类提升率的综合，build 权重 3/9；平台 v4 实测 `fts_mean_improvement=+4.024%`，得分 **4.024 / 50**。
- 防负分硬门禁：任一官方指标相对基线退化 **>1%**，该单点必须回滚。
- 每次提测前：`fts_index` 13/15（两个既有失败）、`ai_column` 4/5（严格用例基线同样失败）、hits 必须精确 `8001 / 11000 / 7332 / 20`、tokenize 21 例逐字节一致。

## 2. 硬性约束

### 允许修改路径（FTS 独立分支）

- `src/storage/fts/**`
- `src/share/text_analysis/**`
- `src/sql/engine/expr/ob_expr_tokenize.{h,cpp}`
- `src/sql/das/iter/**`
- `src/sql/das/ob_das_domain_utils.{h,cpp}`
- `src/storage/retrieval/**`
- `src/observer/composition/retrieval/**`
- `src/sql/engine/expr/ob_expr_bm25.{h,cpp}`
- `src/data_plane/api/data_plane/fts/**`
- 条件路径：`src/sql/engine/table/ob_table_scan_op.{h,cpp}` 仅 `inner_get_next_fts_index_row` / `fetch_next_fts_index_rows` 及 private 成员，单独 commit，Hunk 围栏。

### 红线

- FTS 必须独立分支；严禁合入/推送到 `feat/seekdb-all`，严禁与其他赛道分支 rebase/merge。
- 严禁新增/删除 `.cpp`/`.h`；严禁碰 `.bzl`、`sql_parser_mysql_mode.*`、`ob_system_variable_init.json`。
- 严禁修改 `tools/benchmark/**`、压测 SQL、数据量、`.result` 金样。
- 禁止 `git reset --hard`、`git clean -fd`、强制推送、通配符删除；不得破坏基线数据目录。
- 改远端系统/驱动/CUDA/SSH/防火墙需授权。
- 不得要求、复制、读取或输出 SSH 私钥、密码、Tailscale 凭据。

## 3. 当前交付物

- 工作树：`~/seekdb-work/seekdb-clean`
- 分支：`feat/fts-clean-v5`
- **代码 commit**：`6b29a178d55e8524ada8a6d8cab2c4dc9cdb410d`
- 最新 tip：`b6360f55a`（之后都是负结果文档，代码未变）
- PR：#12 `https://github.com/Funnyvalentine00/seekdb1/pull/12`，base `feat/seekdb-all`
- 代码 diff vs `5bce9e0`：6 个文件，`+70/-6`
  1. `src/storage/retrieval/ob_sparse_daat_iter.{h,cpp}`：`ObSRMergeCmp` 对 binary doc id 用 `memcmp + length`，null/非 string 回退
  2. `src/storage/retrieval/ob_text_retrieval_token_iter.{h,cpp}`：DaaT token `advance_to` 同样的 binary 比较快路径
  3. `src/sql/engine/expr/ob_expr_tokenize.cpp`：`arg_cnt <= 2` 时跳过 `reform_parser_properties`
  4. `src/storage/fts/ob_fts_parser_helper.cpp`：空属性时直接初始化默认 parser property
- 已移除：`fill_merge_heap` 和 BMW `get_iter` 两处 `get_data()` 改动（队长要求，零收益/不保留）。

## 4. 已验证效果

### 平台 v4（`7accd1a`，完整 query+tokenize 补丁，含后来移除的 get_data）

| 指标 | 改善 |
|---|---:|
| `tokenize_ik` | **+9.5%** |
| `tokenize_beng` | **+8.2%** |
| `query_cn` | **+13.0%** |
| `query_beng` | **+8.8%** |
| `query_mixed` | **+8.2%** |
| `query_limit` | **+7.0%** |
| `build_total` | 持平（18.109 vs 18.115） |

`fts_mean_improvement=+4.024%`，得分 4.024/50。

### Linux x86-64 本地最终累计（v4 完整补丁）

- Query：`+2.59% ~ +8.88%`
- `tokenize_ik`：**+4.15%**
- build：基本持平
- 9 指标无 >1% 退化；hits 全部正确。

### 精确 clean 二进制（`6b29a178`）

- SHA256：`644a06e593120eeda2c81a8ad58f8dc834abaa027eaa58637d46e4610130868c`
- `fts_index`：candidate **13/15**，baseline **13/15**；失败同为 `basic_dml`（测试变量 `$rowkey_doc_table_id` 未解析）与 `partitioned_simple_query`（EXPLAIN bind `:0` vs `:1`），diff 仅时间戳。
- tokenize 21 例逐字节一致；20k hits 正确。
- **clean 分支本身尚未跑 Linux 3 对 A/B**（之前最终累计含已移除的 get_data）；平台也尚未提测。

## 5. 已做实验与结果（不要重复）

| # | 方向 | 改动/范围 | 结果 | 状态 |
|---|---|---|---|---|
| 1 | Stage1 五个 Retrieval 热路径批量改动 | fillheap/getiter/bincmp/daatcmp 等 | `query_limit` 墙钟 −12.87% | 整批回滚；后续单独验证 comparator |
| 2 | `ObSRMergeCmp` binary doc-id | 允许路径 | 指令 `+7.2~11.4%`，墙钟 `+4.5~7.3%` | **保留，已进 PR** |
| 3 | DaaT token binary `advance_to` | 允许路径 | 指令 `+8.4~16.0%`，墙钟 `+5.9~11.3%` | **保留，已进 PR** |
| 4 | Tokenize 空属性快路径 | `ob_expr_tokenize` + `ob_fts_parser_helper` | 平台 `tokenize_ik +9.5%`；21 例字节一致 | **保留，已进 PR** |
| 5 | MATCH 是否走 helper 快路径 | 代码/系统表核查 | 压测索引 `parser_properties` 非空，MATCH **不走**空属性快路径；query 收益来自 comparator | 结论入 PR |
| 6 | IK parser 跨文档复用 | helper 缓存 parser/词典 | `build_total −0.04%`；`tokenize_ik +8.78%` 退化（三对全正） | 回滚 |
| 7 | FTS DDL 并行度探测 | `PARALLEL 8/24` | 最好约 5%，无双位数；表可能单分区 | 不作为方向 |
| 8 | hidden word 列 collation=46 | `ob_fts_index_builder_util` | 仅改列 → DDL 4016；补表达式 meta → DDL 通过，多 token MATCH abort；再归一 query range → 点查 `ObMemtable::get` abort | 放弃 |
| 9 | `ObFTWordMap` 跨行复用 | helper 缓存 997 桶 map | `build_total +16.65%`（三对 +19.6/18.3/16.6） | 回滚 |
| 10 | comparator override 设计 | sort + storage rowkey + flag | btree `ObStoreRowkeyWrapper::compare → ObRowkey::fast_compare → ObObj` meta，flag 无法注入；三文件不足 | 设计证伪，build 收摊 |
| 11 | DaaT `advance_to` 二分 | 批内 lower_bound | `query_limit +13.18%`（三对 +20.2/12.0/13.1） | 回滚 |
| 12 | `ObFTWord` hash 缓存 | key 内缓存 hash | build 持平；`tokenize_beng +16.63%`（三对 +17.6/27.7/5.6） | 回滚 |
| 13 | no-lock `ObFTWordMap` | `NoPthreadDefendMode` | 编译被 `ob_join_order.cpp` 显式默认 map 类型挡住 | 已申请该文件两行范围，等待批复 |

负结果证据都在 `handoff/fts-tuning/`：`PARSER_REUSE_NEGATIVE...`、`WORD_COLLATION_CRASH...`、`WORDMAP_REUSE_NEGATIVE...`、`DAAT_BSEARCH_NEGATIVE...`、`WORDHASH_NEGATIVE...`、对应 JSON/崩溃 `.ips`。

## 6. Profile 事实

### Build（20k docs，连续重建 3 轮，自顶向下 self）

| 符号 | self |
|---|---:|
| `ObNullSafeDatumStrCmp<45>`（utf8mb4） | 6.76% |
| `memcmp` | 3.84% |
| `ob_strnncollsp_utf8mb4` | 3.26% |
| `ObOperator::get_next_batch` | 3.16% |
| `ob_mb_wc_utf8mb4` | 2.82% |
| `ObNullSafeDatumStrCmp<63>`（binary） | 2.65% |
| `ObSortOpImpl::Compare` | 1.93% |
| FTS 自身符号合计 | ~2% |

结论：build 大头是通用排序/字符集比较/SSTable 编码；FTS-only 代码上限很低。

### Tokenize（60k IK + 60k Beng）

| 路径/符号 | 数值 |
|---|---:|
| `ObExprTokenize::eval_tokenize` | 33.55% incl |
| `ObExprTokenize::tokenize_fulltext` | 25.85% incl |
| `ObFTParseHelper::segment` | 22.86% incl |
| `ObIKFTParser::get_next_token` | 11.98% incl |
| `ObIKFTParser::process_next_batch` | 11.57% incl |
| `ObIKCJKProcessor::do_process` | 7.83% incl |
| `ObAddWord::process_word` | 4.19% incl |
| `ObAddWord::groupby_word` | 2.72% incl |
| `ObArrayHashMap::find` | 2.53% self |
| `ObFTDATReader::match_with_hit` | 1.75% self |
| `ObLatch::wrlock` | 1.29% self |
| `PageArena::_alloc_aligned` | 0.74% self |

### Query（40k query_limit）

| 路径/符号 | 数值 |
|---|---:|
| `top_k_search` | 58.68% incl |
| `evaluate_pivot` | 43.67% incl |
| `advance_dim_iters_for_next_round` | 29.05% incl |
| BlockMax `advance_to` | 24.14% incl |
| DaaT `advance_to` | 20.36% incl |
| `fill_merge_heap` | 9.65% incl |
| `ObExprBM25::eval_batch_bm25_relevance_expr` | 2.71% self |
| `ObStringDiffDecoder::batch_decode` | 2.64% self |
| `batch_load_data_to_datum` | 2.43% self |
| DaaT `advance_to` self | 1.87% |
| `save_relevances_and_docids` | 1.72% self |
| `ObSRMergeCmp::cmp` | 1.60% self |
| `next_pivot` | 1.47% self |
| `SimpleRowsMerger::push` | 1.28% self（`src/storage/access`，不在允许路径） |
| `ObDocIdExt::from_datum` | 1.08% |
| `ObIArrayWrap::at` | 1.07% |

## 7. 结构性发现（重要，避免走错路）

1. **btree 的 word 比较由 `ObObj` meta 决定**：`ObStoreRowkeyWrapper::compare → ObStoreRowkey::compare → ObRowkey::fast_compare → ObObjCmpFuncs`。`ObTableReadInfo`/`ObStorageDatumUtils` 只服务 datum rowkey 的 scan/estimate，不进入 btree。想改物理序只能改 schema/meta 或核心 key 表示；flag/comparator override 无法安全传导。
2. **改 hidden word collation 46 会触发 45/46 混用 abort**：DML 用源列 meta 45 造 rowkey，schema 46；`ObObjCmpFuncs` 查表失败。只归一 query range 不够，点查仍 abort。
3. **DaaT buffered batch 通常很小**：线性早停优于二分；不要重复二分。
4. **map 常数项敏感**：固定 997 桶 `clear()` 比按行建小 map 更贵；缓存 parser/map/hash 都容易反向。
5. **build 子指标噪声大**：平台 `build_total` 稳定（18.1s），子项 ±50%；主看 `build_total`，≥3 对交替。

## 8. 目前还开放的方向（按建议优先级）

1. **先提测 + 精确 clean A/B**（非优化）
   - 平台提交 `feat/fts-clean-v5 @ b6360f55a`（代码同 6b29a178）。
   - 用测评机跑 `5bce9e0 vs clean` 的 3 对 A/B，补齐 clean 分支独立证据。
2. **no-lock `ObFTWordMap`**（已获原则批准；等 `ob_join_order.cpp` 两行范围）
   - profile `ObLatch::wrlock` 1.29%；预期 tokenize +1~2%，约 +0.4 分。
   - 安全确认：每个 map 是否单线程/单 helper；DDL PARALLEL 是否共享 map；latch 是否另有语义。
3. **用当前 clean 二进制重新 profile**
   - 旧 profile 是 comparator 改动前；用 `perf record -g` 重新找 self >2% 的新热点。
4. **IK 字典查找缓存**（tokenize）
   - `ObArrayHashMap::find` 2.53% + `ObFTDATReader::match_with_hit` 1.75%。
   - 可考虑 per-parser last-hit/直接映射缓存；预期 tokenize +0.5~1%，风险中。
5. **BM25 batch 精确优化**（query）
   - `eval_batch_bm25_relevance_expr` 2.71% self；只能做 bitvector skip、空跑分支等语义精确改动；**不要**改 `1/avg` 除法，BM25 6 位小数有风险。
6. **BMW/save_relevances 数组路径**
   - `next_pivot` 1.47%、`save_relevances` 1.72%、`ObIArrayWrap::at` 1.07%；可做循环内裸指针/减少 bounds check，但过去 `get_data` 类改动被判零收益，风险收益一般。
7. **storage/encoding 范围（需新批准）**
   - `ObStringDiffDecoder::batch_decode` 2.64%、`batch_load_data_to_datum` 2.43%，是 query profile 剩余较大的通用项，但在允许路径外。
8. **结构性 build 改动（需新批准，不建议轻易开）**
   - 减少 build sort 输入行（全局聚合/预排序）、tablet 切分/并行、SSTable 编码；都会碰核心存储/DDL。

## 9. 明确不要重复的方向

- parser 复用（build 持平、tokenize 退化）
- hidden word collation 46 / schema meta 变更（DDL 4016、45/46 abort）
- WordMap 跨行复用（build +16.65%）
- DaaT `advance_to` 二分（query_limit +13.18%）
- `ObFTWord` hash 缓存（tokenize_beng +16.63%）
- build sort comparator override（btree `ObObj` meta 无法注入 flag）
- `fill_merge_heap` / BMW `get_iter` 的 `get_data` 裸指针（已移除，零收益）
- 固定 997 桶 map 复用/全桶 clear

## 10. 希望外部模型回答的问题

请基于以上约束和负结果，提出 **5~10 个新的、可执行的优化假设**，每条包含：

1. 具体热点/代码路径（尽量落到文件/函数级）；
2. 为什么它没有被上面 13 个实验覆盖；
3. 预期收益量级（build/tokenize/query 哪一类、百分比）；
4. 需要的修改范围（是否在允许路径内；若越界，最小文件清单）；
5. 语义/正确性风险（hits、6 位小数、旧 schema、并发）；
6. 最小验证方案（A/B 对数、指标、回归）。

特别欢迎：

- build 侧不依赖 schema/collation、不改 btree key 表示的**结构性**降本思路；
- query 侧在 comparator 已优化后仍能拿到 >1% 的批量/循环级优化；
- tokenize 侧字典/处理器层面的缓存或数据结构优化；
- 任何能绕开“FTS-only 上限低”的合规架构级想法（例如 build 行数/排序输入缩减）。

## 11. 证据索引

- 当前 clean 说明：`CLEAN_BRANCH_2026-11-05.md`
- 归一/负结果：
  - `PARSER_REUSE_NEGATIVE_2026-11-05.md`
  - `WORD_COLLATION_CRASH_2026-11-05.md` + `wordbin-crash-*.ips`
  - `WORDMAP_REUSE_NEGATIVE_2026-11-05.md`
  - `DAAT_BSEARCH_NEGATIVE_2026-11-05.md`
  - `WORDHASH_NEGATIVE_2026-11-05.md`
- 正收益：
  - `FINAL_CUMULATIVE_2026-11-04.md`
  - `LINUX_X86_VERIFY_2026-11-04.md`
  - `TOKENIZE_FASTPATH_2026-11-04.md`
  - `QOPT_BINCMP_RESULTS.md`、`QOPT_DAATCMP_RESULTS.md`
- Profile 原始数据：`legion-evidence-2026-11-04/prof/build.data`、`tokenize.data`、`query_limit.data`
