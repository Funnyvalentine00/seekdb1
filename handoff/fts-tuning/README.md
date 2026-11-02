# #6 Beng ASCII 归一化优化：整合版探索性 PR

## 1. 题目与验收标准

Issue #6。本组只交付 FTS；目标分支 feat/seekdb-all，固定基线 fdb8363f64363819fcf2307be95bcd10a5a7e4ba，产品候选 9ed8c5f5b59e9ad48c17a695b64f681b5ccbf6f6。状态：整合构建和功能回归通过，性能结果仅为探索，PR 保持 Draft，不建议据此直接合并或宣称比赛提分。

## 2. 接口契约

TOKENIZE/MATCH、parser名称/属性、词频/数组顺序保持原语义。Beng 归一化输出由调用方 allocator 持有；ASCII且mbminlen=1/casedn_multiply=1时直接生成，其他文本私有scratch归一化后按实际长度复制。scratch每token复用，调用方token可在parser销毁后继续消费；不跨调用缓存输入。默认分析路径不变，Beng关闭grouping。

#2已有SQL行源、enable_file_sql和secure_file_priv接线不改。使用整合版定义及生成链，不能搬入独立FileSQL旧变量ID/生成文件。文件文本通过现有SQL列进入TOKENIZE/存储FTS，无新增跨模块API。

## 3. 改动文件清单

五个产品文件：src/share/text_analysis/ob_text_analyzer.{h,cpp}、ob_token_stream.{h,cpp}，src/storage/fts/ob_beng_ft_parser.cpp。两项真实回归位于unittest/storage/fts/test_beng_owned_output.cpp与test_beng_owned_capacity.cpp。changes.patch只含这七个文件，SHA256 d585a97cac2c4bc0fe2bed0544ada72bf2fbd0d364053789fab5c63305216246。其余本PR文件都是本handoff的证据/文档；无#2/AI/benchmark/金样改动。

## 4. 构建与运行命令

真实构建环境：/opt/codex-seekdb-20261030，Ubuntu22.04，正常Release make -C build_release -j4。基线先执行src/sql/parser/gen_parser.sh和内表generate_inner_table_schema.py生成ignored构建输入；未改受版本控制的基线源码。完整命令与退出码见logs/build.log。

基线二进制SHA256：c93470673e5a5e911e0418877078d370fc9ff6c5cb3d1061867762f6f7d31b6c。
候选二进制SHA256：5d9a18895b8d1f7d4606f46308bc40203a108ada94934115948d7cf4c0fff5cd。
源码产品提交与后续仅文档提交分开记录，后者不改变受测产品树。

独立实例/opt/codex-fts-integration-pr-20261102，端口2891。固定ERROR日志、server CPU0-3/client4、每次原始init与回归后20s冷却。实际服务器/客户端命令、环境及退出码均在证据包；旧2881未切换。

复测时在干净目标±本补丁两版正常构建，依次原始init、官方mysqltest fts_index/file_sql，再运行原脚本：

```bash
LABEL=before ROWS=20000 BATCH=500 ROUNDS=3000 QUERY_ROUNDS=200 SAMPLES=3 WARMUP=30 SKIP_LOAD=0 OUTPUT=/tmp/fts-before.txt bash tools/benchmark/fts_large_bench.sh
python3 tools/benchmark/fts_large_bench_score.py /tmp/fts-before.txt
```

MYSQL连接参数按新实例设置。after使用同参LABEL=after；不得覆盖历史输出或直接重放封存的一次性runner。

## 5. 官方与自测结果

两版正常构建退出0；候选真实容量/ownership测试通过。8次完整重启均官方FTS15/15；FileSQL全部通过：True。有序SQL及错误差分一致，45项官方输入不变。继承的LOB/不支持字符集错误保留，不算功能扩展。

独立补充的file_rows_check对CSV实际Alice/Bob/Chen逐行验证：非空TOKENIZE、实际预期命中ID和正分数，与等价字面量数据完全一致，原版/候选输出一致。不能仅凭早先查询额外字面量hello的basic-smoke标记宣称此链路通过。

## 6. 性能结果与日志

一对同版A/A诊断：+9.10%，单对不能建立噪声置信区间。三对AB/BA/AB完整固定负载，综合配对耗时改善均值 **+0.99%**，95% Student-t区间 **[-0.71%, +2.69%]**。正数表示耗时降低，负数表示回退；全部样本保留，不与旧窗口混池。

| 指标 | 三对平均耗时改善 |
| --- | ---: |
| build_ik_all_sec | -3.90% |
| build_ik_content_sec | -0.91% |
| build_beng_en_sec | -9.91% |
| tokenize_ik_avg_ms | +2.63% |
| tokenize_beng_avg_ms | -0.63% |
| query_cn_avg_ms | +20.08% |
| query_beng_avg_ms | +1.55% |
| query_mixed_avg_ms | +3.45% |
| query_limit_avg_ms | +2.40% |

四类hits每份报告均8001/11000/7332/20。服务器生命周期峰值RSS均值相对变化 +8.58%，包含重启后的回归准备阶段，不等于仅benchmark峰值或分配器容量。CPU与逐份RSS在benchmark-before/after.json及原始resources.json。

官方CI参照分：before 100.0，after 100.0；本地参照分不等于正式平台得分。benchmark-before/after.json保留三份报告、聚合定义、源码和二进制身份，evidence/reports包含原始文本和官方评分输出。

logs/build.log、verify.log、test.log、notes.log提供命令/退出码和结果索引；evidence/integration-trial.zip保存全部原始回归/bench/server日志、失败/审查记录、脚本及逐文件SHA清单。

## 7. 已知风险与结论边界

本批只有3对、1对A/A，属于快速效果试验，performance_qualified=false；不替代更强环境校准。即使某些门槛满足，也不升级为已证实综合收益。旧16.8%只属于已修正前的局部路径，不适用于此版本。

ASCII扫描本身有成本；Unicode非ASCII回退精确复制是避免保留容量回退的取舍。有限词/charset/OOM覆盖不保证穷尽；没有所有grouping组合的生命周期保证。当前数据只支持本次测试范围。独立审查无产品Major/Blocker；一项文件行索引覆盖缺口由上述补充测试修正，见notes.log。

## 8. 回滚与交付状态

基线 fdb8363f64363819fcf2307be95bcd10a5a7e4ba 保持不变。若不接受此候选，继续使用目标基线；已合入时可单独revert产品提交 9ed8c5f5b59e9ad48c17a695b64f681b5ccbf6f6，再正常重建，不回退其他任务。不要reset现有工作或删除数据。

新实例已停止、数据保留；旧2881/PID7902启动身份与二进制未变。仅推送专用codex/fts-beng-pr-20261102分支并创建Draft PR，未合并或提交比赛平台。后续由队长/CI根据独立环境复测决定是否接受#6优化。
