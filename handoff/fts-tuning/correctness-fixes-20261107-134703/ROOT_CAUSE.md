# P1：两个原用例复现及根因

## basic_dml

原用例与原金样返回 1，1598 行把 `No such row` 拼入 SQL，产生 1064。1582–1589 行删表重建后继续使用旧 `$data_table_id`；新表名相同，表 ID 已不同。独立探针：旧 500004、新 500011，旧 ID 下辅助表数量 0。修复需先刷新 ID。

单补刷新仍不够。原金样的无显式主键段还假定 11/12 映射表存在，并预期上一段显式主键表的 `id`、`__doc_id_*`。实际新表存在隐藏 `__pk_increment`；11/12 不存在，13/14 均为 space.1，辅助表以 `__pk_increment` 和词项/词频/文档长度字段工作。显式主键对照仍有完整 11/12/13/14。

`ob_fts_index_builder_util.cpp` 的 `determine_docid_type` 对新隐藏主键且没有既有 docid 列的表选 `HIDDEN_INC_PK`；`ob_index_builder.cpp` 只在 `TABLET_SEQUENCE` 分配映射，隐键分支直接复用 rowkey。该行为是当前布局的设计。独立 DML 观察：插入后 alpha,beta 命中；更新后旧词仅 beta、新词仅 alpha；删除 beta 后旧词 hits=0。

修订：只更新无显式主键这一段的表身份和布局断言，增加上述 DML 断言；显式主键段和后续测试保持原字节。把旧金样与副本全量输出逐字节切分比较，差异仅在该段。

## partitioned_simple_query

原用例与原金样返回 1（结果文本差异），完整 reject 只有对应的两处 slot 变化：NLJ `t_dim_table.seq(:0)` → `(:1)`，内部范围 `t_ir_single_column.seq < :0` → `< :1`。其他所有输出、查询、计划结构和结果行一致。独立 SQL 在同一数据上输出 8 条单表 MATCH 结果与 5 条外表记录，在 Python 以整数范围条件展开，24 行和原连接逐字段一致。

动态绑定使用 query context 的问号计数器分配，见 `ObExecParamRawExpr::set_param_index`；NLJ 和范围条件引用同一索引。没有实证运行时绑定错误。数值编号是内部计划分配结果；本次不修改优化器、不对所有参数号做正则掩盖。仅对该一条计划的两个对应金样位置作精确修正。

## 边界

两个红灯均在封存产品 194150497 上复现，两个修订属于用例/预期失配；当前证据不支持改产品行为来使旧金样匹配。待审补丁三文件，通过 `git apply --check`（未应用），改动 67 增/36 删。产品文件、原测试/金样、性能脚本及远端 PR 保持。
