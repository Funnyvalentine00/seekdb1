# 环境资格检查记录

## DQ-01：文件型 Unix listener 启动门槛

- 日期：2026-11-07。
- 起点：工作 HEAD `0548d153fe5a9e958e8843c6232702d15d921ff5`；产品 `194150497aca9a69e395317bd6d5a64f26340a61`。
- 输入：用户转述云端预检，文件型 Unix socket 被拒绝；缺确切阶段和 errno。
- 源码证据：`ob_srv_network_frame.cpp:83` 的 `mysql_port_mode` 只控制 TCP；`reactor.rs:1004` 在 Unix 下调用 `UnixListener::bind` 并以 `?` 传播错误；`:1205` 的错误分支输出 `NIO_START_EIO` 并返回空指针。
- 假设：若拒绝跨工作区与 `/tmp` 成立，当前参考不能直接启动 SQL 监听器。该判断等待云端复核，不视为已观测的 SeekDB 启动失败。
- 最小验证：两个新建独占目录、相对路径 `run/sql.sock`、各阶段 errno 与 TCP 控制。用户代传指令，云端尚未执行。
- 在线源码身份：GitHub 可读 v3 `7accd1ace8ee5d035b3ef8aaf426c46b1d0a595e`；两个监听文件在 v3 与当前产品提交逐字节相同。产品提交查询 HTTP 422，分支列表未包含 v4/v5。
- 联通结论：本地工具未取得 Dot 目标会话；用户已选择代传。未创建其他会话、未给无关会话发消息、未改变安全设置。
- 范围裁定：先做轻量监听门槛；暂停大二进制传输和构建，以免在已知启动依赖未满足时消耗资源。
- Checkpoint DQ-P0：仅新增交接文件；产品源码未动，累计实验仍 1/8。下一步待云端探针原始结果。性能实验门禁未打开。

### DQ-01 收尾：原始证据已核验

- 云端探针时间：2026-11-07 12:29:55 +08:00。用户提供 `seekdb-gate-bkv6cbe6.zip`，SHA256 `f0a8c1c15f9493b679d5657d02b0210883ebe7bc7f892b0ccc56a08ea4f73a97`，68,174 字节。
- 归档原包，未执行包内脚本、未解压到产品目录。离线复算器核对 ZIP CRC、10 项文件哈希、两个源码 Git blob 与 SHA256、v3/产品/当前文件字节身份、探针自身哈希、76 条日志和 stdout 一致性。
- 两处 AF_UNIX 测试都在 `socket.socket()` 构造阶段报 `PermissionError`，errno=1/EPERM；每处后续 13 个阶段未执行，包含 bind/listen/connect/收发。两处 TCP 控制双向收发成功并关闭 socket。
- 统计按独立协议用例为 2 通过、2 未通过。原日志 4 个 failed 含 2 个失败汇总，不能计为 4 次构造失败。26 条 not_attempted 是下游阶段，不是 26 个独立测试。
- 条件结论：当前云环境没有通过保持产品不变所需的启动前提。若 Rust 进程受相同 AF_UNIX 限制，当前 Unix listener 路径会启动失败。未实际运行 SeekDB，限制来自哪层仍未知。
- Checkpoint DQ-P1：关闭本轮 Dot 运行/性能测试路径；无工具链安装、构建、数据库启动、A/A、A/B 或产品修改。全局实验仍 1/8；原 V4 门禁状态不变。下一步需具备 AF_UNIX 能力并通过同版噪声校准的 Linux 环境。
