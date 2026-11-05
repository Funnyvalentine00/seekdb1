# Hidden word 列 binary collation：DDL 可建，但多 token MATCH abort（2026-11-05）

## 结论

**当前实现不可提交。** 只改 `generate_word_segment_column` 的 collation 会让 hidden word 列与 `WORD_SEGMENT` 表达式 meta 不一致，DDL 返回 4016；同时改 `ObExprWordSegment::calc_result_typeN` 后 DDL 可以通过，但多 token MATCH 在 memtable 行数估算阶段触发 `ObObjCmpFuncs::compare_nullsafe` abort。

这验证了队长护栏第 1 条：build/scan/iterate 的 word 比较必须两侧成对一致，仅改 hidden 列 schema 不够。

## 尝试 1：只改 `ob_fts_index_builder_util.cpp`

`generate_word_segment_column` 把 utf8mb4 hidden word 列设为 `CS_TYPE_UTF8MB4_BIN`（46），charset 保持 UTF8MB4。

结果：`CREATE FULLTEXT INDEX` 失败 `ERROR 4016 Internal error`，DDL inner SQL 的 ret=-4016。原因：生成列表达式 `WORD_SEGMENT(...)` 的输出 collation 仍是源列 45，与列 schema 46 不一致。

## 尝试 2：追加 `ObExprWordSegment::calc_result_typeN` 输出 46

该文件不在原批准文件清单内，仅做本地验证，未提交。改动后：

- `CREATE FULLTEXT INDEX` 成功
- `__all_column`：`__word_segment` = 46，`__doc_id` = 63，base table 与 index aux 一致
- 单 token `MATCH` 可以返回结果
- 多 token `MATCH` 崩溃

最小复现：

```sql
create table t(a int primary key, b varchar(100), c varchar(100),
               fulltext index fts1(b));
insert into t values(1,'a','a'),(2,'bb','bb'),(3,'ccc','ccc');
create fulltext index fts2 on t(c);
select * from t where match(b) against("a bb ccc dddd");
```

结果：`Lost connection to MySQL server during query`，进程 abort。

崩溃栈（macOS `.ips`）：

```text
ObObjCmpFuncs::compare_nullsafe
ObRowkey::fast_compare
ObRowkey::compare
ObStoreRowkey::compare
ObStoreRowkeyWrapper::compare
keybtree::CompHelper<...>::compare
...
ObMemtable::ObQueryEngine::sample_rows
ObQueryEngine::estimate_row_count
ObTableEstimator::estimate_memtable_scan_row_count
ObTextRetrievalTokenIter::estimate_token_doc_cnt
ObTextRetrievalTokenIter::get_next_batch
```

完整报告：[`wordbin-crash-2026-11-05.ips`](wordbin-crash-2026-11-05.ips)

## 根因判断

hidden 列 schema 改成 46 后，**DML 生成 word 行时仍使用源列 meta 45**（`ObAddWord`/`ObDASDomainUtils` 的 `ft_obj_meta` 来自源列），而 index table 的 hidden 列 schema 是 46。memtable 中的 rowkey datum 因此混有 45 和 46；多 token 估算做 rowkey 比较时 `ObObjCmpFuncs` 找不到 45/46 组合的比较函数并 abort。

单 token 路径没有触发该 rowkey 比较组合，所以看上去正常。

## 要安全落地，需要额外范围

至少还需要把 word 生成/查询 token 的 meta 统一到 hidden 列 meta，候选点：

- `ObDASDomainUtils::generate_fulltext_word_rows` / `ObFTParseHelper::segment`：把 `ft_obj_meta` 归一为 hidden word 列 meta；
- `ObTextRetrievalTokenIter::update_scan_param`：query token meta 统一为 index hidden 列 meta；
- `ObFTWord`/`ObAddWord` 的 groupby/hash meta 是否跟随 hidden 列，需要逐点确认；
- 旧 schema 45 / 新 schema 46 的升级路径。

这已超出“一个 DDL hunk”的时间盒，且涉及 DML/query 两侧，风险显著高于原估计。

## 产物

- 本地实验 patch：`/tmp/wordbin-experiment.patch`（未提交）
- 崩溃报告：`handoff/fts-tuning/wordbin-crash-2026-11-05.ips`
- 实验分支：`feat/fts-build-word-bin`（本地未推送，工作树已回滚到 clean 分支）
