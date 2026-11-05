# ObFTWord hash 缓存：负结果与回滚（2026-11-05）

## 结论

**回滚。** 在 `ObFTWord` 内缓存 hash 值后，`build_total` 持平（−0.06%），但 `tokenize_beng_avg_ms` 三对一致变慢，中位数 **+16.63%**，超过 1% 门禁。

## A/B

Linux 3 对交替，参数同标准 20k 负载。

| 指标 | base 中位数 | hash 缓存中位数 | 变化 |
|---|---:|---:|---:|
| `build_total_sec` | 8.7370 | 8.7320 | −0.06% |
| `tokenize_ik_avg_ms` | 0.2845 | 0.2697 | −5.20%（单对噪声大：+17/−14/+11） |
| `tokenize_beng_avg_ms` | 0.2068 | 0.2412 | **+16.63%**（三对 +17.6/+27.7/+5.6） |
| `query_cn_avg_ms` | 1.2903 | 1.2928 | +0.19% |
| `query_beng_avg_ms` | 1.7510 | 1.7935 | +2.43% |
| `query_mixed_avg_ms` | 1.4543 | 1.4620 | +0.53% |
| `query_limit_avg_ms` | 1.5387 | 1.5422 | +0.23% |

hits 六轮全部正确。`tokenize_beng` 是稳定退化，按门禁回滚。

## 原因判断

`ObFTWord` 增加 hash 缓存字段改变 key 大小/布局，beng 路径的 map key 复制与 cache locality 变差；收益不足以抵消。该方向不再继续。

## 产物

- 原始报告：`~/seekdb-work/results/wordhash-ab-20261105/`
- JSON：[`wordhash-ab-evidence.json`](wordhash-ab-evidence.json)
- 二进制 SHA256：base `dd6512...`，candidate `917dcb88...`
- 分支 `feat/fts-tokenize-word-hash-cache`，commit `afaccff1a`，本地未推送
