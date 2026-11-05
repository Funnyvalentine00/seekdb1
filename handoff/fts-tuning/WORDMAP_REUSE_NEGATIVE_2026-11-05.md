# ObFTWordMap 跨行复用：负结果与回滚（2026-11-05）

## 结论

**回滚。** 把 `ObFTWordMap` 缓存在 `ObFTParseHelper` 中、每行只 `clear()` 复用的实现，使 `build_total_sec` 中位数 **+16.65%**（三对 `+19.56% / +18.28% / +16.60%`），所有 build 子项明显变慢。按 >1% 门禁直接回滚。

## 实现

- `ObFTParseHelper` 增加私有 `ObFTWordMap reusable_word_map_`
- `ObDASDomainUtils::generate_fulltext_word_rows` 优先使用 helper 的 map，首次 `create(997)`，之后依赖 `ObFTParseHelper::segment` 内的 `words.reuse()` 清理
- 分支 `feat/fts-build-wordmap-reuse`，commit `3ffc4224e`（本地未推送）

## Linux 3 对交替 A/B

节点：legion（WSL2 Ubuntu 22.04，x86_64，24 核 / 15 GiB）
方式：3 对交替、每轮全新数据目录、官方 `fts_large_bench.sh`，`ROWS=20000 BATCH=500 ROUNDS=3000 QUERY_ROUNDS=200 SAMPLES=3 WARMUP=30 SKIP_LOAD=0`。

| 指标 | base 中位数 | wordmap 中位数 | 变化 |
|---|---:|---:|---:|
| `build_ik_all_sec` | 3.3470 | 3.7460 | **+11.92%** |
| `build_ik_content_sec` | 2.4280 | 2.8280 | **+16.47%** |
| `build_beng_en_sec` | 1.4240 | 1.8270 | **+28.30%** |
| `build_total_sec` | 7.2080 | 8.4080 | **+16.65%** |
| `tokenize_ik_avg_ms` | 0.2413 | 0.2483 | +2.90% |
| `tokenize_beng_avg_ms` | 0.1947 | 0.1968 | +1.08% |
| `query_cn_avg_ms` | 1.1624 | 1.1607 | −0.15% |
| `query_beng_avg_ms` | 1.6021 | 1.5659 | −2.26% |
| `query_mixed_avg_ms` | 1.2932 | 1.3061 | +1.00% |
| `query_limit_avg_ms` | 1.3850 | 1.3769 | −0.58% |

- hits 六轮全部 `8001 / 11000 / 7332 / 20`。
- `build` 三对都显著退化，不是单对噪声。

## 原因

复用 map 固定为 997 桶；每行 `clear()` 需要遍历整个 bucket 数组。基准文档很短，原实现按 `fulltext.length()/10` 创建小 map（通常十几个桶），create/destroy 反而比清空 997 桶便宜。复用放大了 clear 成本。

如果要继续 map 方向，需要按每行实际 token 数维护小容量并避免全桶 clear；但收益上限仍很低，暂不继续。

## 产物

- A/B 原始报告：`~/seekdb-work/results/wordmap-ab-20261105/`
- JSON 证据：[`wordmap-reuse-ab-evidence.json`](wordmap-reuse-ab-evidence.json)
- 二进制 SHA256：
  - clean base：`dd6512dea5a75d8cfc231deb1cf6fdff434be00bd4488b193e77ad6fa899eb76`
  - wordmap：`6fdfd58fef59a3b019fd3def94350c5332f7bfd4a22acddaee3b8de84dbb65fd`
