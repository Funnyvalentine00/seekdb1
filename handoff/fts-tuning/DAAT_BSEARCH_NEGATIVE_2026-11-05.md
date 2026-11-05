# DaaT advance_to 二分：负结果与回滚（2026-11-05）

## 结论

**回滚。** 把 `ObTextRetrievalDaaTTokenIter::advance_to` 对缓冲 doc id batch 的线性扫描改成 lower_bound 二分后，`query_limit_avg_ms` 三对一致退化 **+20.16% / +12.02% / +13.10%**（中位数 **+13.18%**），超过 1% 门禁。

## 实现

- 分支 `feat/fts-query-daat-bsearch`，commit `4c9655c15`（本地未推送）
- 在 `advance_to` 的 `cur_idx_ < count_` 分支内，对 `[cur_idx_, count_)` 做二分；比较仍用现有 `cmp_ids`（binary doc-id 快路径），排序与 hits 语义不变

## Linux 3 对交替 A/B

| 指标 | base 中位数 | bsearch 中位数 | 变化 |
|---|---:|---:|---:|
| `query_cn_avg_ms` | 1.1461 | 1.1571 | +0.96% |
| `query_beng_avg_ms` | 1.5651 | 1.5767 | +0.74% |
| `query_mixed_avg_ms` | 1.2944 | 1.2730 | −1.65% |
| `query_limit_avg_ms` | 1.3919 | 1.5754 | **+13.18%** |
| `build_total_sec` | 7.2130 | 7.2100 | −0.04% |
| `tokenize_ik_avg_ms` | 0.2391 | 0.2441 | +2.09% |

- hits 六轮全部 `8001 / 11000 / 7332 / 20`。
- `query_limit` 三对全部为正，是稳定退化，不是噪声。

## 原因判断

- DaaT 的 buffered batch 往往很小，`cur_idx_` 又经常已经接近目标；线性扫描的早停比二分更便宜。
- 二分增加了分支和 cache miss，尤其 `query_limit` 的候选 doc 序列更短/更跳跃时更明显。
- 结论：该路径不适合二分，回到线性；后续 query 优化不再碰 `advance_to` 的批内搜索。

## 产物

- 原始报告：`~/seekdb-work/results/daat-ab-20261105/`
- JSON 证据：[`daat-bsearch-ab-evidence.json`](daat-bsearch-ab-evidence.json)
- 二进制 SHA256：
  - clean base：`dd6512dea5a75d8cfc231deb1cf6fdff434be00bd4488b193e77ad6fa899eb76`
  - bsearch：`32edd761251a814cc12f9f81d4fa9de73cadd8469767ac3cd4f7ebe37f8d1e52`
