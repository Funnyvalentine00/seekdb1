# 2026-11-15 MiniVectorDB 四版本配对性能与拯救者验证

当前结论：CRC 单项和组合版都带来稳定的暖缓存恢复收益（默认负载约 50%）；默认写入/查询的整体收益较小，缓冲区单项未证明查询加速。完整首轮没有触发噪声感知回退规则，但两项 Linux 查询控制仍存在未排除的回退风险。针对性补测准备好后拯救者掉线，补测尚未执行。

## 冻结版本与执行环境

- 产品基线：`a285f2e67be0f6de2ac68c2b32ec6a2426984f01`；组合版：`9db50ce9b4ea698144a1410eaca6baf4de36df2f`。CRC 单项只替换 `src/common.h`，buffer 单项只替换 `src/main.cpp`，其余产品源码逐文件 SHA 一致。
- 本地：Apple M5 Pro、24 GiB，macOS arm64；四版均使用同一临时标量/fdatasync→fsync 兼容构建。
- 拯救者：Intel Core Ultra 9 275HX，WSL2 Linux x86_64；原始 CMake Release 构建，真实 fdatasync，性能数据位于 WSL 原生 ext4。数据库/驱动固定在不同 guest CPU 2/4；这不保证 Windows 物理核心隔离。编译器、CPU flags、文件系统、源码与二进制 SHA 均归档。
- 拯救者目录：`/home/xiazihan/benchmark/minivectordb-four-way-20261115-9db50ce`，通过 `ssh legion` 访问。四版源码和二进制都已保留在那里；没有向 GitHub 推送。

## 测量设计

每台机器 6 类负载、168 个平衡 ABBA/BAAB 区块、672 次独立进程运行；每负载每候选 6 块，同二进制 A/A 10 块。每一轮随机交错负载和候选，所有数据库进程串行执行。数据/请求种子固定，响应和 WAL 字节摘要按负载对照，所有样本保留。
默认负载为 10k×128、batch100、1000 次 half-filtered TopK。控制包含 batch10000、dim8、短响应及大于64KiB响应后跟短响应。Mac batch1 使用 10000 行；拯救者首轮发现该控制每次写入约21秒，完整控制需约40分钟，因此在未按候选效果选样的情况下中断并保留60次记录，独立重跑时只将 batch1 控制改为1000行。其余五类请求 SHA 跨主机一致；两个batch1规模不能直接合并。
恢复指标是已写入同样WAL后的暖OS缓存重开，包含进程启动到ready；每次检查WAL非空且小于8MiB、重开后WAL不变和完整ID集。它不是冷磁盘恢复或断电实验。写入和查询计时不含驱动端结果解析/哈希；启动和首查询单列。
耗时变化按每块 mean(log B)−mean(log A) 的中位数转换百分比；负值更快。描述性95%区间按区块bootstrap；A/A噪声按区块相对变化绝对值P95。回退筛查规则为区间下界超过 max(3%, A/A P95)，不是通过等价性检验，也不能排除小幅或尾部回退。未记录逐条查询P95/P99。

## 默认负载结果

|机器|版本|写入耗时变化|查询耗时变化|暖恢复耗时变化|
|---|---|---:|---:|---:|
|Mac|CRC|-2.31%|-0.61%|-50.29%|
|Mac|buffer|-0.09%|-0.05%|-0.16%|
|Mac|组合|-2.04%|-0.11%|-50.80%|
|拯救者 WSL|CRC|-2.62%|-0.16%|-49.78%|
|拯救者 WSL|buffer|+0.49%|+0.61%|-0.45%|
|拯救者 WSL|组合|-1.98%|-0.88%|-50.31%|

默认write+query合并后的配对变化：Mac CRC −1.65%、组合 −1.46%；拯救者 CRC −1.71%、组合 −1.46%。这不是官方计分公式，不能投射为正式分数。

## 尚未排除的回退风险

|拯救者控制项|查询变化|描述性95%区间|A/A绝对变化P95|
|---|---:|---:|---:|
|短响应 / 组合|+5.64%|-13.40% 至 +23.22%|18.02%|
|batch10000 / CRC|+4.44%|-1.41% 至 +5.86%|13.88%|

这两项不能因未触发阈值而宣称无回退。短响应的组合版有4/6块更慢，batch10000的CRC有5/6块更慢；墙钟与全进程CPU并不总同步。所有Mac和Linux完整首轮的正式回退flags都是false，但独立审查发现分别有11、38个单块指标超过各自筛查阈值。

补测预案已固化：短响应 baseline↔combined 和 batch10000 baseline↔CRC 分别12个候选区块+12个同批A/A；查询数扩大10倍（50,000和10,000），新种子20261116，另外记录查询阶段schedstat CPU。扩大的负载只用于诊断，不与首轮混池，也不作为官方等价分数。拯救者离线导致v3上传失败，远端补测未启动。

## 完整控制矩阵

|机器|负载|候选|写入变化|查询变化|暖恢复变化|
|---|---|---|---:|---:|---:|
|Mac|batch1|crc|-2.17%|-0.42%|-31.21%|
|Mac|batch1|buffer|+1.33%|+0.20%|+0.22%|
|Mac|batch1|combined|-0.71%|+0.10%|-31.64%|
|Mac|dim8|crc|-6.32%|-0.81%|-33.22%|
|Mac|dim8|buffer|+0.39%|-0.23%|-0.33%|
|Mac|dim8|combined|-5.02%|-0.31%|-32.20%|
|Mac|responses|crc|-3.44%|-0.27%|-3.95%|
|Mac|responses|buffer|+0.12%|-0.86%|+1.40%|
|Mac|responses|combined|-5.24%|-1.31%|-6.55%|
|Mac|batch10000|crc|-1.40%|-0.51%|-49.07%|
|Mac|batch10000|buffer|-0.10%|-0.78%|+0.04%|
|Mac|batch10000|combined|-2.35%|-0.58%|-49.54%|
|Mac|default|crc|-2.31%|-0.61%|-50.29%|
|Mac|default|buffer|-0.09%|-0.05%|-0.16%|
|Mac|default|combined|-2.04%|-0.11%|-50.80%|
|Mac|large-response|crc|-0.97%|-0.08%|-15.00%|
|Mac|large-response|buffer|+0.64%|-0.19%|-1.44%|
|Mac|large-response|combined|-3.41%|+0.07%|-13.99%|
|WSL|batch1-1k|crc|-0.53%|-1.09%|-17.15%|
|WSL|batch1-1k|buffer|-0.04%|+0.21%|-1.84%|
|WSL|batch1-1k|combined|+0.12%|-4.52%|-16.44%|
|WSL|dim8|crc|+0.44%|+1.73%|-32.41%|
|WSL|dim8|buffer|-0.83%|-0.38%|-0.92%|
|WSL|dim8|combined|-0.65%|+0.07%|-34.06%|
|WSL|responses|crc|+0.35%|-0.47%|-6.95%|
|WSL|responses|buffer|+1.06%|-0.48%|+1.77%|
|WSL|responses|combined|+0.30%|+5.64%|-7.10%|
|WSL|batch10000|crc|-5.45%|+4.44%|-42.65%|
|WSL|batch10000|buffer|+1.42%|-3.15%|+0.03%|
|WSL|batch10000|combined|-4.84%|+0.08%|-42.79%|
|WSL|default|crc|-2.62%|-0.16%|-49.78%|
|WSL|default|buffer|+0.49%|+0.61%|-0.45%|
|WSL|default|combined|-1.98%|-0.88%|-50.31%|
|WSL|large-response|crc|-1.16%|+0.72%|-19.07%|
|WSL|large-response|buffer|+1.06%|+0.10%|+0.42%|
|WSL|large-response|combined|-4.85%|-0.16%|-20.22%|

## 正确性与证据

- 四版Mac正确性检查通过；四版拯救者各22项检查全部通过且无跳过，含实际20/20公开SQL、13/13回归、事务/随机模型、WAL兼容/检查点/崩溃恢复和两个Linux内存限制门槛。CRC正常及ASan/UBSan单测通过。此次没有构建并运行官方Docker协议检查器。
- 每台完整性能轮都得到一致的响应/WAL摘要，1,344份stderr为空。二进制运行前后SHA一致。独立审查复算了每台全部672条运行和212个汇总单元。Linux RSS使用数据库VmHWM，wait4全进程生命周期高水位另存。
- 测量器负向验证：提前退出、超时、Python -O 禁用断言均正确失败；人工±20%数据检验统计方向；补测工具间歇缺失CPU指标的故障已复现并修复。
- 原始证据根目录：`/Users/xiazihan/seekdb-work/handoff/mvd-four-way-20261115/`。Mac为`macos-paired`，拯救者为`legion-results/linux-paired-1k-control`；原中断数据为`legion-results/linux-paired`，exit130明确为操作员中断。
- 上传包SHA：`0deec5d3d27c9aa7f973f0017d3c663ba0b56a00c58ae9091eddbb275580c35d`，304个文件已在远端验证。首轮结果包SHA：`b29f8f34c9516c08f5839a11202008ef2dddf03bde57e89c1cd7bb164f8ecc12`，下载后校验通过。
- 机器离线前`exit-v2.code=0`，原始结果已完整拉回本地，数据目录为空。需要拯救者恢复 Tailscale/WSL 后继续补测。
