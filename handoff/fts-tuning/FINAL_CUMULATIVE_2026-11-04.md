# 最终累计 A/B（Linux x86-64，2026-11-04）

节点：legion（WSL2 Ubuntu 22.04，x86_64，24 核 / 15 GiB）
基线：`seekdb-base`（5bce9e0）
候选：`seekdb-cand-tok`（5bce9e0 + 查询补丁 + tokenize 默认属性快路径，等价于 PR head 的 FTS 改动）
方式：3 对交替完整官方负载，每轮全新数据目录。

> 注：v5 干净分支（`feat/fts-clean-v5`）已按队长要求移除 `fill_merge_heap`
> 与 BMW `get_iter` 两处 `get_data()` 改动。下表是包含这两处的完整 v4 补丁
> 测量，因此只能作为干净分支收益的上界参考；干净分支的查询收益应以
> `QOPT_BINCMP_RESULTS.md` / `QOPT_DAATCMP_RESULTS.md` 的比较器结果为准。

| 指标 | baseline 中位数 | candidate 中位数 | 改善 |
|---|---:|---:|---:|
| `build_ik_all_sec` | 3.3490 | 3.3480 | +0.03% |
| `build_ik_content_sec` | 2.4280 | 2.4310 | −0.12% |
| `build_beng_en_sec` | 1.4260 | 1.4230 | +0.21% |
| `tokenize_ik_avg_ms` | 0.2410 | 0.2310 | **+4.15%** |
| `tokenize_beng_avg_ms` | 0.1913 | 0.1918 | −0.26% |
| `query_cn_avg_ms` | 1.2004 | 1.1279 | **+6.04%** |
| `query_beng_avg_ms` | 1.6411 | 1.5699 | **+4.34%** |
| `query_mixed_avg_ms` | 1.3872 | 1.2640 | **+8.88%** |
| `query_limit_avg_ms` | 1.4464 | 1.4090 | **+2.59%** |

- hits 全部 `8001 / 11000 / 7332 / 20`。
- 官方九项指标无超过 1% 的退化，门禁通过。
- Query 四项平均约 **+5.5%**，tokenize_ik **+4.15%**，build 基本持平。
- `query_limit` 本对只有 +2.59%，单对波动仍较大；Query 其他三项更稳定。

## 证据

- JSON：`linux-x86-final-cumulative-evidence.json`
- 原始报告：`legion-evidence-2026-11-04/results-final/c{1..3}-{base,tok}.txt`
- 节点全部证据（含 results、results-tok、results-fdb、prof、关键日志、二进制 SHA）：
  `legion-evidence-2026-11-04/`
- 候选二进制 SHA256：`254346591c3756a7cd9d929b953af83278693d0657308259cef757e60ba6ac19`
- 基线二进制 SHA256：`0b9416ca50cc8b909d06ff43b4738bd5bd941b67f665fbbfb8287e6e96614676`

## 限制

- 本机 Linux 节点，不等同评测容器；最终以官方 CI 为准。
- 3 对样本，`query_limit`/`tokenize_beng` 仍有可见波动。
- 该候选是“干净基线 5bce9e0 + 两个补丁”的等价验证；PR head 实际基于 fdb8363，需要平台复测。
