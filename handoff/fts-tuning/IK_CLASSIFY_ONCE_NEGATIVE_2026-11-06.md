# IK UTF8MB4 classify-once：本机筛选负结果与回滚（2026-11-06）

## 结论

不保留。3 对 macOS arm64、标准 20k 完整负载、交替顺序、每轮全新实例及数据目录，`tokenize_ik` 配对中位数改善 -1.97%，未通过 >1% 退化门禁。build_ik_all 也出现 -8.61% 的门禁失败读数。build_total +0.095% 基本持平。

这组结果没有证明正收益；三对 tokenize_ik 改善分别 -9.21%/-1.97%/+1.44%，不应称为“稳定退化”，也不能用“只改分词不该影响建索引”来豁免门禁。没有新增 Linux/平台数据，不作跨平台结论。源码已恢复到 7d04fbbe8（src 等价 6b29a178）。

## 单点

仅 `src/storage/fts/ik/ob_ik_char_util.h`，+14/-1。UTF8MB4 do_classify 解码一次后沿原顺序判断 alpha/arabic/chinese/other_cjk；UTF16/LE 原链不变。未改 parser/cache/map/浮点公式、benchmark、SQL、金样或核心存储。

候选只作为实验补丁保存，不合入 clean 提交候选。

## 机制、构建与正确性

- 原 release 二进制分类函数有 4 处 decode_unicode 调用；候选路径 1 处，实际重复解码没有被优化编译消除。
- 用真实生产 header、原编译选项、真实 server 静态库构建分类探针。5,756,801 条记录、28,784,005 字节逐字节一致；覆盖所有 Unicode 标量、UTF8 截断、全部单/双字节组合、UTF16/LE 双字节组合及 null 指针。该集合不等于所有可能非法字节串的穷举。
- 两份 corpus SHA256 均 a0bffffac4cd608646b53ff980b03b27eae6931f3a4713f8e20df9b80e19e9e4。
- 候选及精确 clean 源码分别完整构建成功。原 build_release 的旧二进制不是本轮对照，因此弃用；本轮二进制 SHA 固定如下。
- clean SHA256：dad8dbecc6502872ef9aa0a3f6a1caa4f2580632f1fed348f6c85c39ec2b8653。
- candidate SHA256：0517212ee5457e2ee7cb4145edbee391ce3130cab07c861ed2a81625d175e177。
- 注意：增量构建的启动版本元数据仍缓存旧 revision 558b4fcc；不能用启动字符串识别本轮代码。对照以源码差分、构建记录及上述 SHA 为准；未进行平台提交。
- 24 条 SQL tokenize 语句（含既有错误输入、混合字符和长文本），stdout/stderr 逐字节一致。
- fts_index：基线、候选均 14/15，唯一失败均为 partitioned_simple_query 的 EXPLAIN :0/:1；去掉日志时间/文件头后失败 diff 主体相同。basic_dml 本轮两边均通过，不能照抄旧环境的 13/15。
- 最终回归前修正了两实例测试 admin 的认证配置，且为两套回归分离日志目录；初轮环境失败及中断日志不作为最终结果。
- 六份性能报告 hits 都为 8001/11000/7332/20。
- 独立静态代码审查未发现 Major/Blocker；审查者未独立复跑测试。
- ai_column、本轮 Linux file_sql 和官方平台未跑；本单点已因性能门禁失败淘汰，不进入正式验收。

## A/B

三对执行顺序 base/candidate、candidate/base、base/candidate。固定 ROWS=20000 BATCH=500 ROUNDS=3000 QUERY_ROUNDS=200 SAMPLES=3 WARMUP=30 SKIP_LOAD=0；未修改官方脚本。改善为 100*(1-candidate/base)，正值表示更快；表格改善是逐对改善的中位数，与“两个耗时中位数之比”可能不同。

| 指标 | base 耗时中位数 | candidate 耗时中位数 | 配对改善中位数 |
|---|---:|---:|---:|
| `build_ik_all_sec` | 2.3470 | 2.4480 | -8.61% |
| `build_ik_content_sec` | 1.8410 | 1.7410 | +5.43% |
| `build_beng_en_sec` | 1.0350 | 1.0300 | +0.48% |
| `tokenize_ik_avg_ms` | 0.0711 | 0.0725 | -1.97% |
| `tokenize_beng_avg_ms` | 0.0470 | 0.0469 | +0.64% |
| `query_cn_avg_ms` | 0.8341 | 0.8310 | -0.96% |
| `query_beng_avg_ms` | 1.1949 | 1.1968 | -0.16% |
| `query_mixed_avg_ms` | 0.9202 | 0.9159 | -0.16% |
| `query_limit_avg_ms` | 0.9846 | 0.9850 | +0.03% |
| `build_total_sec` | 5.2410 | 5.2440 | +0.10% |

## 证据与限制

- 结构化 A/B：`ik-classify-once-ab-evidence.json`。
- 完整原始数据：`/Users/xiazihan/seekdb-work/results/ik-classify-once-20261106/`，含 p{1..3}-{base,candidate}.txt/.stderr、独立 server 日志、回归日志、SQL 差分、分类 corpus、汇编与 driver。
- 构建日志：`/Users/xiazihan/seekdb-work/work-logs/ik-classify-once-build.log`、`ik-classify-clean-build.log`。
- 补丁：`IK_CLASSIFY_ONCE_EXPERIMENT.patch`，仅供复现，当前源码未保留。
- 未完成的首次性能启动与回归重叠，已主动中止并丢弃；最终六份报告使用另一组全新目录，均在回归完成后运行。测试创建的数据目录保留，没有覆盖既有基线目录。
- 本轮只证实该实现未取得可接受的端到端收益。调用次数下降不能直接推出 tokenize 加速；布局/内联或测量波动是否参与，尚未归因。
