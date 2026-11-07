# FTS 诊断与提前停止审查交接

产品参考为 `194150497`，本轮没有候选产品提交。模型配置按用户选择保持，以下材料供后续独立审查。

## 最小复现与材料

- 节点会话位于 `/home/xiazihan/benchmark/fts-v4-integration-20261106/next-cycle-20261107-104032`。在隔离的新会话标识下复用 `reference-manifest.json` 的固定参数。旧运行名和数据均已占用。
- [命令与失败记录](commands.log)、[诊断源身份](cold-diagnostic-source.json)、无损 `cold-diagnostic.patch.gz`、两次冷诊断归档、A/A 归档和 [已验证结论](verified-results.json) 可用于复核。
- `warm-profile-v2-evidence.tar.gz` 包含 329 个软件 CPU 时间样本的原始 perf.data、leaf/inclusive 报告、退出码和哈希。第一次内联解码出现 DWARF 错误且未在合理时间完成，已停止；最终使用 `--no-inline` 成功解码原始采样。
- `final-reference-evidence.tar.gz` 包含四 hits、三套回归、逐句语义、所有用例返回状态及 80 并发响应摘要。包含历史失败的原始日志。旧证据在 `../v5-20261107/final-evidence.tar.gz`。
- 可变构建输出为诊断二进制。正式参考路径为节点 `saved-binaries/seekdb-fixed`，SHA 见 DELIVERY。大二进制和实例目录留在节点，复制时以哈希核验。

## 待审查的问题

1. 同版完整对在端点负载合格时仍显示 SELECT 1 与 Beng 超过 10% 的波动。检查预设门槛、样本口径和早停决定。第三次样本保留，不能补成完整配对。
2. `ob_ft_range_dict.cpp` 与 `ob_ft_dat_dict.cpp` 的诊断只记录阶段状态和分配计数。检查 per-range arena、cache payload、最高 VmHWM 的时间关系，以及为何这些数值不能证明限并发会降低正式峰值。
3. 0.30% 为输出投影函数的累计软件 CPU 样本占比。检查内联和并行时序的局限。该次 profile 没有为实现表达式映射缓存提供足够收益证据。
4. 下一次有可信测量窗口时，无评分 relevance 变更仍需验证 reset/reuse、scoreful/function lookup 和 255/256/257/1024 词边界。当前不存在该候选的代码或性能证据。
5. 回归签名比较允许日志头、文件位置和同一显示行的截断差异。请核对 `verify_evidence.py` 对完整差异内容和参考返回码的断言。

复算命令为 `python3 -B verify_evidence.py`。本次没有提交官方平台、硬件计数或独立模型审查结果。
