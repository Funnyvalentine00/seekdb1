# 契约与验收核查表

以本目录在线 issue 原文及18:00:10+08的固定快照为准。状态“待补”表示
尚无本轮可追溯证据，不等于断言实现一定失败；不从源码或日志摘要推算分数。

| #1 要求 | 核查结论 | 交接责任 |
| --- | --- | --- |
| 明确任务归属 | 已按用户最新分工划定：本聊天协调；另一个 agent 完整负责#2/#6 | 已完成本地记录 |
| 共同基线 | 已确认5bce9e06d1281347ad61344e4b01fab9158c58a2 | 后续候选标明相对基线的完整差异 |
| 独立源码、构建、实例 | 权限边界已明确；共享构建的历史混入已记录 | 实现方独占 native/2882；本聊天不再写入 |
| 接口不可擅改 | 已列下表；本轮未修改 | 实现方验收输出需逐项对应 |
| 公共文件串行合入 | 冲突清单已建立；没有执行合并 | 队长审查合并 |
| 合并顺序FTS→File SQL→AI列 | 已登记；未开始合并 | 若FTS保持原版，记录无新增候选，不制造优化提交 |
| 四类完整日志 | #2四个文件存在，但原始命令/退出/runner证据不齐 | 实现方补完整原始日志 |
| 八节交接README | native handoff/file-sql/README.md缺失；旧FTS README需更新 | 实现方完成 |
| 官方测试与干净构建 | 本轮未运行；当前#2交付身份未闭合 | 实现方在最终提交上验证 |
| 子issue摘要与最终提交 | #2/#6在线评论均为空；本轮未发布 | 交付时由获授权者发布；平台提交由队长负责 |

## #2 File SQL（50分）

| 契约 | 当前证据判断 |
| --- | --- |
| read_csv('path'[,header=true,sample_size=-1]) | 历史CSV检查点可追溯；不能自动移植为当前dade8da工作区成绩 |
| read_json('path',format='newline_delimited') | 历史JSONL检查点可追溯；当前同上 |
| read_parquet('path') | 当前提交有实现；原始官方完整运行及边界证据待补 |
| glob('path/*')，单列file | 仍有相关未提交接线；提交与运行身份待闭合 |
| DESCRIBE SELECT返回六列元数据 | 固定代码快照有权限和TINYINT类型问题，见REVIEW.md |
| COPY (query) TO 'path' (format csv, header, nullstr 'NULL') | 枚举/代码生成接线仍未提交，需自洽最终提交及完整运行证据 |
| BIGINT/DOUBLE/BOOLEAN/VARCHAR/DATE/DATETIME/NULL | 需覆盖全NULL、重复列id__2、ISO日期时间；不以单个fixture代替所有类型边界 |
| 单文件SQL、双文件JOIN | test.log有示例输出，缺最终提交/二进制身份和完整执行退出码 |
| enable_file_sql默认OFF、1235/1064、secure_file_priv | 本轮仅记录契约；测试应证明允许路径与拒绝路径，不能只在secure_file_priv='/'下给出正例 |
| 大文件流式处理、资源上限 | 历史CSV/JSONL1GiB诊断有效但不等于当前Parquet/glob/COPY或整服1GiB容器认证 |

#2的八项分值仍为7/10/7/6/6/6/4/4；单个官方测试中出现部分正确输出不能
换算为正式部分分数。禁止代执行、绕过安全检查、全量物化、改输入或硬编码。

## #6 FTS（50分）

- TOKENIZE(text,'ik'|'beng')、MATCH AGAINST语义、parser名称和属性不变。
- 每份报告核对hits：cn8001、beng11000、mixed7332、limit20。
- 固定负载：ROWS=20000 BATCH=500 ROUNDS=3000 QUERY_ROUNDS=200
  SAMPLES=3 WARMUP=30 SKIP_LOAD=0；评分器与基线不改。
- 只在issue#6允许路径优化：storage/fts、share/text_analysis、
  ob_expr_tokenize.cpp、sql/das/iter和ob_das_domain_utils.cpp。
- 当前历史最佳仍原版5bce9e0，保护分支codex/fts-best-20261031。
  原始FTS15/15和饱和的本地100分不能证明新代码有综合提升。
- 更新handoff/fts-tuning/README.md及其“当前最佳”入口，明确哪些是历史
  候选、哪些被回退。保留旧报告，不用覆盖文件消除负向或失败证据。

## 版本证据不可混用

| 版本 | 可使用的证据 | 本轮地位 |
| --- | --- | --- |
| 5bce9e0 | 原比赛基线与封存FTS结论 | 历史FTS最佳 |
| a888a899 | CSV检查点与对应9d23999e二进制/ZIP | 历史已验证功能 |
| e0c9b140 | JSONL16/16、CSV22/22、FTS15/15，对应e0a935二进制/ZIP | 历史已验证功能，File SQL总体仍0/1 |
| a7c89ce→dade8da及五个未提交文件 | 本次只读源码/交接日志快照 | 当前实现方工作，尚未由本聊天验收 |
| Windows三个未提交DESCRIBE文件 | 失败构建、独立静态审查、未运行的新测试 | 停放材料，不能晋升或直接覆盖对方 |
