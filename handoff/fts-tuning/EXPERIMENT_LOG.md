# V5 Experiment Log

## P0 — state protection

- Parent=f5fe975382f4b151f7181a70d7f7e05ff9b234fd，产品194150497；本地tag checkpoint/fts-v4-before-v5-20261107。
- 隔离feat/fts-opt-v5；原两个macOS改动SHA及二进制diff封存在v5-20261107/protected-state.json/protected-macos.patch，原工作树不动。
- 历史H2/H7收益、H3三处修正、parser/map/hash/bsearch/IK/BMW/storage负例已经接管；不重复已否定的机制。
- 当前步骤：AA-V5-01已启动（tmux fts-v5-aa；v5/results/v5-aa*，fixed二进制不变；新数据保留）；E01方案和用例先准备，禁止与基准并行编译。

## E01 — word aggregation one lookup

- Parent=f5fe975382f4b151f7181a70d7f7e05ff9b234fd（产品194150497）；BEST_VALIDATED完整门禁尚未建立。
- Hypothesis：当前groupby仍get_refactored+set_refactored；历史v2有单查找收益，当前版本需独立再验证。仅移植带末次拼写修正的updater，不批量cherry-pick。
- File=src/storage/fts/ob_fts_stop_word.cpp。风险=collation等价词拼写与词频、arena归属；先用基线建立重复/重音/全半角/长文本/分数/并发差分预期。
- Expected：减少hash/桶锁路径；幅度未知，不以调用次数声明收益。
- Rollback：只恢复本轮单文件的保存原始字节并SHA验证；不reset、不动既有优化/其他工作区。
- Build/Test/Benchmark/Memory/Decision/Commit：PENDING。

## AA-V5-01 — completed, noisy

4报告hits全对，已封存aa-evidence.tar.gz。单版tokenize和SQL-loop差异过大。决定不据此评定E01小效应；启动独立AA-V5-CONTROLLED-01，taskset server4 CPU/client1 CPU、cpu_count=4，guest affinity实际值每实例记录。旧样本不删除、不混池。

## 有限实验预算（用户最新约束）

最多8个独立优化假设，每项最多2次实现修正，每个有效候选最多3对正式AB。主要指标>=2%并超过AA噪声；正确性/尾延迟/RSS门禁均保留。连续3个有效实验无可重复收益、环境不可信或剩余预期收益不足即停止新实验并收尾。

任务时间未由用户给具体值，当前保守设总上限4小时（V5开始00:39至04:39，Asia/Shanghai），最后48分钟至少用于验收；03:51后不启动新优化。BUDGET.json为持久化账本。E01已登记，尚未改产品代码；AA校准不计优化数。原六对计划改为最多三对，尚未启动任何候选AB，因此没有超预算样本。

### Harness correction (no optimization experiment)

AA-V5-CONTROLLED-01结束后，baseline check在bind预探测返回EADDRINUSE，尚未创建实例/执行SQL。检查点不以bind失败推断监听存在；V2 helper改用有超时的connect_ex，仅在实际已有listener时拒绝启动。原control.py与失败日志保留；新control_v2.py/check_candidate_v2.py单独命名，不覆盖已执行脚本。

### E01 implementation v1

只修改ob_fts_stop_word.cpp；匿名WordCountUpdater在桶锁内替换key并加word_freq，首次仍插入1。未混入dictionary/stopword改动。preimage、patch与源码SHA已记录，产品代码尚未提交/晋升；编译等待基线验证及CPU校准完成。

### P0 calibration outcome and E01 build

受控四次AA仍有分词4–8%及控制项8–11%级波动，RSS383–438MiB。单实例ReqWorker CPU-AA：IK前两块约84.4/84.2us，但beng约44.6/49.6/55.5us，且第三块IK发生ReqWorker数量变化，标为不可比而非伪造CPU改进。所有样本保留。

基线E01-before验证完成：9边界均成功；22条原Tokenize+新聚合/DML/80并发有结果；FTS13/15同历史，FileSQL1/1，AI5/5。校准后无并行性能任务；E01 v1两并发增量构建已启动（tmux fts-v5-e01-build）。只允许最多3对正式AB，若效应不能超过噪声，不晋升，不追加追样本；后续按环境可信度与预期价值决定提前停止。

## E01 — FAIL_TO_PROMOTE / finalized (1 of 8)

Build exit0，20.91s，binary eff02f8fc44b0245ab33ddc54c61d002d854a77531014d52fa27f992f69e9e8d。源码仅stop_word.cpp，本轮实现修正0次。全部语义/80并发/9边界与before一致，三套回归两版FTS13/15、FileSQL1/1、AI5/5，无新增失败。

正式AB/BA/AB共3对，已到候选测量上限。配对改善中位数：build_all+3.84%、build_content+2.65%、build_beng0%、tokenize_ik+0.89%、tokenize_beng-5.36%、query_cn-1.75%、query_beng+2.67%、mixed-0.10%、limit+1.13%；RSS中位数-0.92%。Beng三对+20.05/-5.36/-6.87%，未可重复；两个官方单项<-1%，目标亦未达>=2%且超过AA噪声。尾延迟Beng P95/P99有正向信号，不能覆盖正式失败；全部raw samples保留。

决定：不晋升，FAIL_TO_PROMOTE。只按preimage精确恢复本轮单文件，先验证当前SHA=实验after，恢复后SHA=before；不用Git reset/clean/checkout，原v4/v3不动。候选二进制、补丁、前像、构建与所有结果保留。

提前停止新优化：用户条件4（测试环境无法为预期2–5%增益提供可信结果）。两种AA协议及CPU/尾延迟诊断仍高波动；不再加跑追好样本，不启动E02。当前进入最终验收/交付，保留足够收尾时间。总预算8、实际1、成功0、失败1、部分0、晋升0。


## 2026-11-07：发布当前 V4 候选并复核最高性能环境

用户批准推送；已非强推发布 `feat/fts-opt-v4-verify @ 194150497aca9a69e395317bd6d5a64f26340a61` 与证据分支 `feat/fts-opt-v5`，创建 PR15 并发送 `/benchmark`。未合并。用户确认 legion 已切最高性能且空闲后，有界复测保留7份同版报告、3完整配对及1未配对；追加最后一份结束读数16%触发原门槛，未继续补样本。三对IK分词变化 -6.81/+7.16/-3.08%，Beng -0.70/+6.93/-4.48%，SELECT1 +20.04/+8.86/+15.26%。不能用增加平均抹去波动或推断具体功耗根因。

本轮 FTS13/15、FileSQL1/1、AI5/5，19通过2历史失败，九组边界和三组语义差分无新增差异；官方CI尚无可见运行。产品仍194150497，新实验0，累计1/8；8次自有实例均正常退出、源码与金样未动。详见 `retest-20261107-130006/DELIVERY.md` 和离线复算器。


## 2026-11-07：两项 FTS 回归修订

两个原红灯已复现；实际隐藏主键 DML 与24行连接对照通过。修复属于测试契约：删建后表 ID/隐键布局，以及 EXPLAIN 关联编号。用户明确授权三文件局部修订；副本全三套21/21，应用后2/2。仅本地提交71ec3001b/8fc409d17，产品仍194150497，累计性能实验1/8，原PR不动。证据见 correctness-fixes-20261107-134703/APPLIED.md。
