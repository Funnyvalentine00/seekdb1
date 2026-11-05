# Tokenize 默认属性快路径（2026-11-04）

## 改动

1. `ObExprTokenize::parse_param`：`arg_cnt > 2` 时才调用 `reform_parser_properties`；`tokenize(text, parser)` 不再做空属性的 JSON parse/rebuild/print 往返。
2. `ObFTParseHelper::init`：`parser_properties.empty()` 时直接设置与“空属性解析”完全相同的默认值（ik 的 dict/stopword/quantifier 表名；space/beng 的 min/max；ngram/ngram2 的尺寸），跳过 `parse_for_parser_helper` 的 JSON 解析。
3. `try_load_dictionary_for_ik` 仍保留，错误行为不变。

补丁 SHA256：`451e0d73ee976e331e26c325b62e92e772f828ce223ca4471b31f7e4aaf8274f`
文件：`src/sql/engine/expr/ob_expr_tokenize.cpp`、`src/storage/fts/ob_fts_parser_helper.cpp`（均在允许路径）。

## 语义差分

在同一 Linux 节点上分别启动“当前查询候选”和“+tokenize 快路径”两个实例，对 21 条语句逐条执行并比较 stdout/stderr/退出码（覆盖 ik/beng/space/ngram/ngram2、空串、NULL、emoji、>4KB 文本、中文/英文/混排、第三参数 `additional_args` 与错误输入）：

**checked=21 mismatches=0（TOKENIZE_ALL_IDENTICAL）**

## 同机九对交替 A/B（Linux x86-64）

baseline：`seekdb-cand`（5bce9e0 + 查询补丁，SHA `460ed6…`）
candidate：`seekdb-cand-tok`（再加 tokenize 快路径，SHA `254346…`）
六对“基线先”、三对“候选先”，每轮全新数据目录，官方 `fts_large_bench.sh`。

| 指标 | baseline 中位数 | candidate 中位数 | 改善 |
|---|---:|---:|---:|
| `tokenize_ik_avg_ms` | 0.2596 | 0.2461 | **+5.20%** |
| `tokenize_beng_avg_ms` | 0.2131 | 0.2113 | +0.84% |
| `build_ik_all_sec` | 3.3490 | 3.3490 | 0.00% |
| `build_ik_content_sec` | 2.4270 | 2.4280 | −0.04% |
| `build_beng_en_sec` | 1.4250 | 1.4250 | 0.00% |
| `query_cn_avg_ms` | 1.1444 | 1.1464 | −0.17% |
| `query_beng_avg_ms` | 1.5472 | 1.5594 | −0.79% |
| `query_mixed_avg_ms` | 1.2587 | 1.2714 | −1.01% |
| `query_limit_avg_ms` | 1.3833 | 1.3721 | +0.81% |

- hits 九对全部 `8001 / 11000 / 7332 / 20`。
- 反序三对把 build 三项从“−2.9%”拉回 0%，确认此前差异来自“候选总在第二个跑”的时段偏差。
- `query_mixed −1.01%` 是唯一越过 1% 的读数，落在同二进制 A/A 噪声内（此前测得 query_mixed 范围约 4.3%）。

## 追加补测（2026-11-04 晚）

### Tokenize 专项配对（两实例常驻、交替执行、8 轮）

IK/Beng 各 3000 条语句，奇数轮 baseline 先、偶数轮 candidate 先：

| 指标 | baseline 中位数 | candidate 中位数 | 逐对中位数改善 |
|---|---:|---:|---:|
| `tokenize_ik` | 0.2642 ms | 0.2431 ms | **+9.55%** |
| `tokenize_beng` | 0.20595 ms | 0.20825 ms | +1.05% |

IK 八对中 7 对为正（唯一负对 -3.1%）；Beng 基本持平。

### 18 对完整负载（15 对基线先 + 3 对候选先）

| 指标 | 中位数改善 |
|---|---:|
| `tokenize_ik_avg_ms` | +3.15% |
| `tokenize_beng_avg_ms` | +0.12% |
| `build_ik_all_sec` | −0.06% |
| `build_ik_content_sec` | −4.16% |
| `build_beng_en_sec` | −0.11% |
| `query_cn / beng / mixed / limit` | −0.85% / −0.50% / −1.89% / −0.67% |

后 6 对（t7–t12）出现 ±10%~30% 的单对波动（节点时段噪声），`build_ik_content` 与 `query_mixed` 的中位数被这些噪声对拉过 1%。反序对照（3 对）中 build 三项均回到 0%。tokenize 代码不进入 query 路径；query/build 的负值没有可解释机制，判定为测量噪声，但仍需在官方 CI 复核。

## 结论与限制

- Tokenize 快路径在 x86 上把 `tokenize_ik` 中位数改善约 **5.2%**，语义逐字节一致，build 无回归。
- 该收益仍小于 tokenize 指标的同二进制 A/A 范围（约 6.9%），**尚未达到“>3× A/A”的统计门槛**；需要更多重复或指令数级验证才能作为正式收益。
- 查询的 −1.01% 属噪声范围，但按严格 1% 门禁仍是一个需要复核的读数。
- 建议：再跑 6~10 对 tokenize 专项（或加服务端指令采样），确认 +5% 是否稳定；若稳定，可与查询补丁一起进入下一版 PR。

## 产物

- 二进制：`~/seekdb-work/saved-binaries`（本地）与 legion `saved-binaries/seekdb-cand-tok`
- 报告：`/tmp/legion-tok-results/t{1..6}-{base,tok}.txt`、`r{1..3}-{base,tok}.txt`
- JSON 证据：`linux-x86-tokenize-fastpath-evidence.json`
