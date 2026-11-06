# V5 baseline and protocol

ORIGINAL_BASELINE=5bce9e06d1281347ad61344e4b01fab9158c58a2（仅历史）；WORKING_REFERENCE=f5fe975382f4b151f7181a70d7f7e05ff9b234fd/产品194150497。BEST_VALIDATED完整门禁=未建立；不得把之前未过门禁的版本重命名为已通过。

Linux before SHA=9544ca38c4d6cc336adb079f362eff1fa220739d58ce70bc2d0f1d948d5ade32，Clang17/Rust1.98.1/CMake RelWithDebInfo -O2 -g -DNDEBUG；make/Cargo/lld=2并发。复用原构建缓存，候选只改变本轮源码。

AA-V5-01预登记：同一fixed二进制四次完整负载，AA1a/AA1b/AA2b/AA2a；全新自有目录。官方ROWS20000 BATCH500 ROUNDS3000 QUERY_ROUNDS200 SAMPLES3 WARMUP30 SKIP_LOAD0不变；4 hits严格核对。记录每次官方9项、select1、RSS、资源和退出码。诊断不替换旧6对。

噪声若高于候选预期收益，先固定线程/亲和性或增加成对重复形成独立新协议，不删样本。未稳定前不晋升。

E01预登记：仅groupby_word单次set_or_update，保留末次等价词拼写/初次计数1/后续word_freq；与194150497对照。语义/FTS先通过无新增回归，最多3对平衡AB/BA、相同配置与完整负载。9项任一配对中位数退化>1%不能晋升；已知13/15失败保留双版本证据。

P95/P99另做逐语句固定语料诊断，保留全部样本，与AA尾延迟波动比较；缺少此验证时只记PARTIAL。新数据保留，资源阈值WindowsD>=30GiB/Linux>=15GiB/RAM>=3GiB，每阶段前及每30秒检查；只有单实例。

## AA-V5-01结果及独立新窗口

同fixed四次完整负载：select1在一对变动17.05%，IK分词12.12%，beng9.84%，build_content5.62%；不支持1%级判定。全部结果保留。AA-V5-CONTROLLED-01：server进程限定4个guest CPU，client限定第5个guest CPU，实例cpu_count=4；其余官方参数不变，四次全新目录。仅进程级设置，不改系统；后续AB必须相同设置，不与旧窗口混池。

## 补充分词CPU/尾延迟诊断

受控AA仍出现IK约5%、beng约8%、SELECT1约8–11%变化，另有单次RSS438MiB。增加一次bounded CPU-AA诊断：同fixed实例，3块×每项1000条SELECT1/IK/Beng，记录ReqWorker各线程schedstat运行ns及逐句P95/P99。它不改官方测量，不计正式AB，不能替代官方九指标。若仍无法区分预期效应与噪声，按用户环境不可信停止新优化；不无限调整协议。正式AB最多3对，每实例官方测量后另采1000条/目标的P95/P99，独立保存。

E01正式顺序固定AB/BA/AB，共3对，不增加到6对。每次官方完整负载后再以持久客户端采每项1000条逐句延迟，原始列表全部保存；该诊断不会改动已结束的官方计时。尾延迟默认P95<=5%、P99<=10%不利变化，若A/A噪声使结论不可判定则不晋升。官方RSS在额外诊断前后分开记录。

## 校准与验收封存

两种AA窗口和E01正式3对分开保存。共有17份官方完整报告：8次AA、2次正确性装载、6次正式AB、1次最终参考复核；装载/复核读数未纳入正式配对统计。正式候选AB上限未突破。

任务4小时是上限而非必须耗尽，触发早停后按实际用时计算最终阶段占比，至少20%；时间与计算在BUDGET.json。所有二进制SHA与输入/配置按窗口记录，不用启动版本字符串认定身份。
