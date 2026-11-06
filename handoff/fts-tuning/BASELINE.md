# V5 baseline and protocol

ORIGINAL_BASELINE=5bce9e06d1281347ad61344e4b01fab9158c58a2（仅历史）；WORKING_REFERENCE=f5fe975382f4b151f7181a70d7f7e05ff9b234fd/产品194150497。BEST_VALIDATED完整门禁=未建立；不得把之前未过门禁的版本重命名为已通过。

Linux before SHA=9544ca38c4d6cc336adb079f362eff1fa220739d58ce70bc2d0f1d948d5ade32，Clang17/Rust1.98.1/CMake RelWithDebInfo -O2 -g -DNDEBUG；make/Cargo/lld=2并发。复用原构建缓存，候选只改变本轮源码。

AA-V5-01预登记：同一fixed二进制四次完整负载，AA1a/AA1b/AA2b/AA2a；全新自有目录。官方ROWS20000 BATCH500 ROUNDS3000 QUERY_ROUNDS200 SAMPLES3 WARMUP30 SKIP_LOAD0不变；4 hits严格核对。记录每次官方9项、select1、RSS、资源和退出码。诊断不替换旧6对。

噪声若高于候选预期收益，先固定线程/亲和性或增加成对重复形成独立新协议，不删样本。未稳定前不晋升。

E01预登记：仅groupby_word单次set_or_update，保留末次等价词拼写/初次计数1/后续word_freq；与194150497对照。语义/FTS先通过无新增回归，再6对平衡AB/BA、相同配置与完整负载。9项任一配对中位数退化>1%不能晋升；已知13/15失败保留双版本证据。

P95/P99另做逐语句固定语料诊断，保留全部样本，与AA尾延迟波动比较；缺少此验证时只记PARTIAL。新数据保留，资源阈值WindowsD>=30GiB/Linux>=15GiB/RAM>=3GiB，每阶段前及每30秒检查；只有单实例。
