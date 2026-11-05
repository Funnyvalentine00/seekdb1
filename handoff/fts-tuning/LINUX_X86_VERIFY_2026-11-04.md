# Linux x86-64 同机复核：PR #11 负优化结论

日期：2026-11-04　｜　节点：legion（WSL2 Ubuntu 22.04，x86_64，24 核 / 15 GiB）
构建：`./build.sh release --make -j4`（deps 来自 `/opt/codex-seekdb-20261030`）
基准：官方 `tools/benchmark/fts_large_bench.sh`，`ROWS=20000 BATCH=500 ROUNDS=3000 QUERY_ROUNDS=200 SAMPLES=3 WARMUP=30 SKIP_LOAD=0`，每轮全新数据目录。

## 实验一：5bce9e0 基线 vs 5bce9e0 + PR 补丁

| 指标 | base 中位数 | cand 中位数 | 改善 |
|---|---:|---:|---:|
| `build_ik_all_sec` | 4.0610 | 3.4480 | +15.09% |
| `build_ik_content_sec` | 3.0370 | 2.4300 | +19.99% |
| `build_beng_en_sec` | 1.4250 | 1.4250 | 0.00% |
| `tokenize_ik_avg_ms` | 0.2548 | 0.2541 | +0.27% |
| `tokenize_beng_avg_ms` | 0.2251 | 0.2067 | +8.17% |
| `query_cn_avg_ms` | 1.2084 | 1.1310 | **+6.41%** |
| `query_beng_avg_ms` | 1.6564 | 1.5236 | **+8.02%** |
| `query_mixed_avg_ms` | 1.3547 | 1.2843 | **+5.20%** |
| `query_limit_avg_ms` | 1.4619 | 1.3362 | **+8.60%** |

- 三对交替中 Query 四项**逐对全部为正**；九项指标无超过 1% 的退化。
- hits 全部为 `8001 / 11000 / 7332 / 20`。
- RSS：base 385,377 KB → cand 383,445 KB（略降）。
- 二进制 SHA256：base `0b9416ca50cc8b909d06ff43b4738bd5bd941b67f665fbbfb8287e6e96614676`；cand `460ed683121c97332827509574021a3a825f8fbe0ad8ad14a487910a15e4f3c2`。
- 补丁 SHA256：`344490884447ce04829f68acf75d53068a03edad1a4add691fc9b2a1a068b1db`。

## 实验二：5bce9e0 vs fdb8363（AI/FileSQL 整合底座）

| 指标 | 5bce9e0 | fdb8363 | fdb 相对 base |
|---|---:|---:|---:|
| `build_ik_all_sec` | 3.3500 | 3.3480 | +0.06% |
| `build_ik_content_sec` | 2.4340 | 2.4290 | +0.21% |
| `build_beng_en_sec` | 1.4260 | 1.4270 | −0.07% |
| `tokenize_ik_avg_ms` | 0.2413 | 0.2372 | +1.70% |
| `tokenize_beng_avg_ms` | 0.2045 | 0.2020 | +1.22% |
| `query_cn_avg_ms` | 1.1585 | 1.1877 | −2.52% |
| `query_beng_avg_ms` | 1.6544 | 1.6562 | −0.11% |
| `query_mixed_avg_ms` | 1.3315 | 1.3472 | −1.18% |
| `query_limit_avg_ms` | 1.4262 | 1.4875 | −4.30% |

底座 fdb8363 的 Query 平均约慢 2%，build/tokenize 基本持平，**远不足以解释平台反推的 query −22.5%**。

## 结论

1. 在 Linux x86-64、干净基线 5bce9e0 上，PR 的四处改动是**正优化**：Query 墙钟 +5.2%~8.6%，指令数此前 macOS 亦为正。
2. 分支底座 fdb8363 相对 5bce9e0 只有约 2% 的 query 退化，也不是平台 −7.5% 的主因。
3. 平台结果与本地同机结果方向相反，且平台 query 基线（16–24 ms）比本地（1.2–1.5 ms）慢约 12 倍，说明平台运行环境/基线口径与本地差异巨大。当前证据不足以把平台负优化归因到本 PR 代码，**更像平台单次运行波动、限流或基线口径问题**。
4. 仍需平台的逐项报告才能最终定位；建议在 PR 评论中附上本证据，并请求重跑平台 benchmark / 提供 per-metric 数据。

## 产物与复现

- 报告：`/tmp/legion-results/p{1,2,3}-{base,cand}.txt`（Mac 本地副本）
- 底座对照：`/tmp/legion-results-fdb/q{1,2,3}-{base,fdb}.txt`
- 节点上：`/home/bench/benchmark/seekdb-verify-20261104/{src-base,src-cand,src-fdb,saved-binaries,results,results-fdb,logs}`
- 复现命令见 `logs/` 与 `metadata.txt`；补丁 `/home/bench/benchmark/fts-opt.patch`
- JSON 证据：`linux-x86-ab-evidence.json`、`linux-x86-fdb-base-evidence.json`

## 限制

- 本地 WSL 节点不等同于评测容器；绝对值不可直接比较，但同机交替 A/B 的方向可信。
- build 指标在本地波动较大（尤其 `build_ik_all`），已用三对交替与中位数处理。
- 未复跑 `fts_index`/`ai_column`；本次只针对性能方向。
