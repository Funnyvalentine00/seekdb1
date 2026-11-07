# 已授权应用：两项 FTS 回归测试修复

2026-11-07，用户批准仅修订 basic_dml.test、basic_dml.result 与 partitioned_simple_query.result。已应用先前完整验证的同一补丁；应用前后的 SHA 见 APPLICATION.json。没有改变 SeekDB 产品代码、官方 benchmark、数据规模或计时逻辑。

| 本地提交 | 修复 |
| --- | --- |
| 71ec3001bf8189d7dc2e8b0ec97f98fec3e7105c | basic_dml：删表重建后重新获取 ID，检查真实的隐藏主键/13、14 索引布局，增加插入、更新、删除后 MATCH 断言 |
| 8fc409d17021c0c3dbd501d84b7a9c7fd2bacfc2 | partitioned_simple_query：只修正同一动态参数绑定与范围引用的两行预期；SQL、计划结构及结果行不变 |

完整三套回归使用与本地应用字节一致的副本，**FTS15/15、File SQL1/1、AI5/5，21通过、0失败、0跳过**。正式应用后另从工作区复制这三个文件到隔离实例，两项定向复测均通过。原始测试的两个失败完整保留；修订副本中间的预期长度比较失败也保留。两次 SQL 探针已验证隐藏主键 DML 结果及24行连接独立对照。File SQL 沿用既有的本地数据部署适配。

本地可复算：

```sh
python3 -B verify_evidence.py
python3 -B verify_application.py
```

输出分别与 verified-results.json、verified-application.json 相同。主证据 108 文件；应用后证据 18 文件、52,417 字节，SHA256 5a07181834e193a0164fa00fdd99902d006390152927f583aab1d1571ce2dd53。节点保留一份自有数据约2.52GiB；本轮4次实例均正常 SIGUSR1 退出，2920 无监听、无本轮残留 tmux。节点最终 Windows D/Linux/RAM 可用 140.44/465.16/14.25 GiB。无构建和大二进制传输。

工作起点 493dc5bd9b0bb2b7669aadc23035dc54b5a2f12e，产品仍 194150497aca9a69e395317bd6d5a64f26340a61。测试修复仅本地提交，PR15 仍固定原产品提交与原测试。原 macOS 兼容修改、v3 工作区、两份未跟踪计划和历史结果保持。回退时仅审查这两个新提交或限定三文件反向补丁，先校验前像和后像，不重置主工作区。

本轮两项正确性修订已完成。未重新做性能 A/A、候选 A/B、平台CI或专用内存安全构建；不据此晋升 BEST_VALIDATED。累计产品性能实验仍 1/8；下一轮若继续性能优化，沿用原测量门槛，并保持正式发布与本地回归结果的区分。
