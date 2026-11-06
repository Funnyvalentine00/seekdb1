# V5 research queue

1. P0 AA-V5-01：校准194150497的官方完整负载与RSS；解释build/content/beng噪声。
2. E01：set_or_update单查找，保留最后等价拼写；独立实验，无批量移植。
3. E02：字典只读命中快路径+is_load原子状态，需并发测试，依赖分析后执行。
4. H3长查询：scoreless DaaT tokens×batch内存，先分配归因再改。
5. 当前候选profile后的结果物化/拷贝；不得重做H2/H7本身。

停用词字节长度短路暂不直接移植：同charset不意味着collation等价词字节长度相同，先覆盖重音/全角等价词。Beng caller-owned ASCII已做过且效果不确定/RSS增长；997桶map、hash缓存、batch二分、整数比较、COUNT-over-limit、storage小拷贝均已失败，不机械重试。

## 关闭本轮队列

E01门禁失败；E02字典读锁、停用词安全短路、H3长查询内存和其他查询方向均未启动，不计实验数。由于当前测量不能稳定区分预期效应，继续实现/测量的成本高于可验证收益，本轮不继续消费额度。任何后续实验须用户明确开启新周期。优先在可控Linux环境建立可信AA，再考虑复验E01及这些候选；不要直接移植v2字节长度短路或覆盖v4。
