# Dot 云环境启动资格检查收尾

日期：2026-11-07。状态：**本轮环境资格复核未通过，停止该环境上的数据库与性能测试。**

## 1. 施工摘要

核对用户代传的云端探针、原始输出与固定提交源码，归档原始 ZIP 并新增离线复算入口。当前云端 Python 进程无法构造 AF_UNIX/SOCK_STREAM socket；当前 SeekDB Unix 启动路径要求创建该类 listener。未以兼容性改动绕过门槛，未启动产品优化实验。

## 2. 修改清单

本目录保存原派发说明和 P0 状态，补充本报告、实验日志、`verify_archive.py`、`verified-archive.json` 与原始 `seekdb-gate-bkv6cbe6.zip`。压缩包保持原字节，未解压到工作区源码路径，未执行其中脚本。Downloads 原文件保留。产品、官方测试、两份既有计划以及其他工作区未修改。

## 3. 测试与证据报告

| 用例 | 结果 | 阶段与证据 |
| --- | --- | --- |
| 工作区独占目录 AF_UNIX | 未通过 | socket 构造报 EPERM；bind/listen/连接收发未执行 |
| `/tmp` 独占目录 AF_UNIX | 未通过 | socket 构造报 EPERM；bind/listen/连接收发未执行 |
| 工作区 localhost TCP | 通过 | 完成双向字节校验和 socket 关闭 |
| `/tmp` localhost TCP | 通过 | 完成双向字节校验和 socket 关闭 |

共 4 个协议用例：2 通过、2 未通过；两处目录准备均通过。每个 AF_UNIX 用例的后续 13 阶段未执行，共 26 阶段。原始 76 条日志包含 42 条 passed、4 条 failed、26 条 not_attempted、4 条元数据；4 条 failed 中有 2 条汇总，不代表 4 次独立尝试。

离线核验通过：ZIP CRC、10 项归档文件 SHA256、两个源码 Git blob SHA1 和 SHA256、raw/stdout 字节一致、探针哈希与环境记录一致，以及两处 TCP 接收字节一致。两个源码文件与本地 `7accd1ace8ee5d035b3ef8aaf426c46b1d0a595e`、`194150497aca9a69e395317bd6d5a64f26340a61` 和当前工作文件均逐字节相同。

云端证据记录时间为 2026-11-07 12:29:55 +08:00。SeekDB 功能测试、三套回归、A/A、A/B 均未执行；没有安装工具链或编译数据库。没有新增产品性能数据。

## 4. 验收结论

本轮环境检查已经收尾。对于保持当前产品不变的运行测试，Dot 当前执行环境未通过前置门槛，不再安排预编译程序传输或数据库测试。

直接实测只证明 Python 进程构造 AF_UNIX socket 返回 `PermissionError: [Errno 1] Operation not permitted`。源码表明，在 Unix 常规启动路径且已通过更早检查的条件下，TCP 开启仍会执行 `UnixListener::bind("run/sql.sock")`，其失败会返回 reactor 启动错误。若 Rust 进程受同样限制，则不能启动这条路径。未实际运行 SeekDB，不将上述条件推断记为数据库启动实测。

## 5. 已知问题与独立审查边界

- EPERM 的具体限制来源未知，不归因于 seccomp、AppArmor、目录权限或某一服务配置。
- OS errno=1 与源码 FFI 启动状态 `NIO_START_EIO=5` 属于不同层，不混用。实际失败早于 bind，不能记录成 bind 失败。
- 文件身份与原始记录均可复核；未独立重跑云端探针，未验证其他进程或不同权限域。
- 本次仅核对两个网络文件的指定控制流，不扩展成整个数据库进程退出、所有网络子系统或平台普遍能力的结论。
- 全局 BEST_VALIDATED 与原 V4 已知回归失败保持原状态。本轮环境探测不计入产品优化实验，累计仍 1/8。

## 6. Checkpoint 与复现

DQ-P1：证据收尾。起始工作 HEAD `0548d153fe5a9e958e8843c6232702d15d921ff5`，产品参考 `194150497aca9a69e395317bd6d5a64f26340a61`。证据本地提交可用 `git log -1 --format='%H' -- handoff/fts-tuning/dot-qualification-20261107/CLOSEOUT.md` 定位。

原始包 SHA256：`f0a8c1c15f9493b679d5657d02b0210883ebe7bc7f892b0ccc56a08ea4f73a97`。从仓库根目录离线复算：

```sh
python3 -B handoff/fts-tuning/dot-qualification-20261107/verify_archive.py
```

输出应与 `verified-archive.json` 相同。此命令只读取归档和 Git/source 字节，不执行探针。未发生产品修改，无需代码回滚；不删除原始包和失败记录。

## 7. 下一阶段

恢复性能施工需要允许 AF_UNIX、资源满足实例运行要求，且同版 A/A 校准通过的 Linux 环境。当前 legion 的既有噪声失败证据仍有效，不能因 Dot 路径关闭就直接跳过测量门禁。取得新的可验证环境控制条件前，不启动新产品实验；继续遵守暂不推送约束。
