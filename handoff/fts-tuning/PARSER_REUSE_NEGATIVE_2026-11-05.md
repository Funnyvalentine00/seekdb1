# Parser 复用单点：负结果与回滚（2026-11-05）

## 结论

**回滚。** IK parser 复用没有降低 build 时间，反而让 `tokenize_ik` 中位数退化 **+8.78%**（三对全部为正），触发 >1% 防负分门禁。实验分支保留为负例记录，不进入 PR #12。

## 实验分支

- 基线：`feat/fts-clean-v5`，commit `558b4fccac944c0424a1b6edece798c05ff70a6e`
- 实验：`feat/fts-build-parser-reuse`，commit `df7cf6b11c87d7cb68aadcdf87811b17df3fcee4`
- 改动：`ObFTParseHelper` 持有缓存的 `ObIKFTParser`，跨文档保留词典/缓存，只重建 `TokenizeContext` 与 segmenter；同时修正 reset 的重复释放。

## Linux x86-64 A/B

节点：legion（WSL2 Ubuntu 22.04，x86_64，24 核 / 15 GiB）
方式：3 对交替、每轮全新数据目录、官方 `fts_large_bench.sh`，参数 `ROWS=20000 BATCH=500 ROUNDS=3000 QUERY_ROUNDS=200 SAMPLES=3 WARMUP=30 SKIP_LOAD=0`。

| 指标 | base 中位数 | parser 中位数 | 中位数变化 | 三对变化 |
|---|---:|---:|---:|---|
| `build_ik_all_sec` | 3.243 | 3.242 | −0.03% | +3.24% / +6.75% / −3.20% |
| `build_ik_content_sec` | 2.324 | 2.326 | +0.09% | 0.00% / +4.63% / +0.22% |
| `build_beng_en_sec` | 1.323 | 1.321 | −0.15% | −7.79% / −0.15% / −51.16% |
| `build_total_sec` | 6.898 | 6.895 | **−0.04%** | +0.03% / +4.63% / −17.92% |
| `tokenize_ik_avg_ms` | 0.2197 | 0.2390 | **+8.78%**（退化） | +2.28% / +8.78% / +9.29% |
| `tokenize_beng_avg_ms` | 0.1938 | 0.1873 | −3.35% | −8.00% / +100.38% / −3.35% |
| `query_cn_avg_ms` | 1.1262 | 1.1528 | +2.36% | −5.29% / +7.75% / +2.36% |
| `query_beng_avg_ms` | 1.5483 | 1.5427 | −0.36% | −8.05% / +10.12% / −0.36% |
| `query_mixed_avg_ms` | 1.2727 | 1.2659 | −0.53% | −6.17% / +5.93% / −0.53% |
| `query_limit_avg_ms` | 1.3118 | 1.3291 | +1.32% | −63.46% / +2.63% / +3.74% |

- hits 全部 `8001 / 11000 / 7332 / 20`。
- `build_total` 三对里两对持平、一对受 `build_beng_en` 的 base 异常样本（2.711s vs 正常 1.323s）拖到 −17.92%；中位数仍是 −0.04%，没有可确认的 build 收益。
- `tokenize_ik` 三对全部为正，是稳定退化，不是单点噪声。
- `query_limit` 的 −63.46% 来自 c1 的 base 3.4548ms 离群样本（其余 base 约 1.29–1.31ms），不作为代码结论。

## 原因判断

1. build profile 早已显示 FTS 自身符号只占约 2%，大头是通用排序/utf8mb4 collation/SSTable 编码；词典 cache 加载不是热点。
2. `ObFTParseHelper` 的一次性调用（`tokenize()` 表达式、MATCH 查询）原本从调用方 arena 分配 parser；改为 `ObMalloc` 常驻缓存后，一次性路径多了一次全局分配/释放，导致 `tokenize_ik` 稳定变慢。
3. 因此这个方向即使能保留，也需要把复用严格限制在“长生命周期 helper”上；但 build 收益为零，继续加工没有价值。

## 产物

- A/B 原始报告：`~/seekdb-work/results/parser-ab-20261105/`
- JSON 证据：[`parser-reuse-ab-evidence.json`](parser-reuse-ab-evidence.json)
- 二进制 SHA256：
  - clean base：`dd6512dea5a75d8cfc231deb1cf6fdff434be00bd4488b193e77ad6fa899eb76`
  - parser reuse：`ed1644fdcfcc0af7c8f7a81bae17ae73f413a5460eaa51954f3017dc0b068737`

## 并行度探测（同一数据目录，探索性）

在同一台测评机上用 clean base 二进制、20k 行 `docs` 表，对 `fti_content_ik` 做 drop/add 计时（单轮，含客户端与 DDL 开销）：

| 方式 | 耗时 |
|---|---:|
| default #1 | 2541 ms |
| `PARALLEL 8` | 2426 ms |
| default #2 | 2126 ms |
| `PARALLEL 24` | 2022 ms |
| default #3 | 2124 ms |

`PARALLEL 24` 相对 default 中位数约快 5%，没有双位数收益；该表很可能只有一个分区，DDL 并行度无法充分展开。这个方向不足以单独支撑 build 目标。

## 下一步

Parser 复用已排除。后续 build 方向应按收益上限重新排序：

1. 先做低风险的 `ObFTWordMap`/row buffer 复用，确认 build 是否还有 1% 级空间；
2. 若仍无收益，转向需要队长批准的排序/编码/DDL 并行度范围；
3. 不再在 `ObFTParseHelper` 初始化上投入。

