# Dot 云环境资格复核与代传指令

更新：已收到云端原始证据并完成离线核验，本轮资格复核未通过，最新结论见 [CLOSEOUT.md](CLOSEOUT.md)。本文件和 `STATE.json` 保留派发前 P0 状态。

日期：2026-11-07。当前工作 HEAD：`0548d153fe5a9e958e8843c6232702d15d921ff5`。产品参考：`194150497aca9a69e395317bd6d5a64f26340a61`。

## 1. 本轮摘要

用户提供 Dot 云环境预检，并选择由用户代传指令。当前工作区未直接连接到该云容器。以下环境信息来自用户转述，尚未取得原始命令输出：Debian 13.6、Linux 6.18.57、x86_64、AMD EPYC 9V74、9 个可见 CPU、约 9.7 GiB 内存（可用约 7.1 GiB）、工作区约 58.5 GiB 可用；cgroup 配额不可见。Python 3.12.14 可用，未找到 Clang/CMake/Rust/Cargo。同一命令内后台进程 12 秒和 localhost TCP 成功，进程不跨调用存活，文件型 Unix socket 创建被拒绝。

当前源码显示，启用 TCP 时仍无条件创建文件型 Unix listener。Unix bind 失败会传播为整个 NIO 启动失败。因此先区分路径限制与 AF_UNIX 限制，再决定是否传预编译程序。当前不安装工具链、不传大二进制、不启动新性能实验。

## 2. 可直接代传给 Dot

> 请执行 SeekDB 云环境的启动门槛复核，限 5 分钟；本轮只下载两个源码文件并执行小型 socket 探针，不 clone 全仓库、不安装工具链、不编译、不改产品源码。
>
> 仓库：`https://github.com/Funnyvalentine00/seekdb1`。
>
> 固定读取提交 `7accd1ace8ee5d035b3ef8aaf426c46b1d0a595e`。当前本地 v4 产品提交为 `194150497aca9a69e395317bd6d5a64f26340a61`，GitHub API 当前不可读取。已在本地逐字节确认下面两个文件在两提交间相同，所以 v3 只用于这次监听器门槛检查，不作为 v4 的性能验收版本。
>
> 1. 从 GitHub raw 下载 `rust/sql-nio/src/reactor.rs` 和 `src/observer/ob_srv_network_frame.cpp` 到新建目录，记录下载结果及 SHA256。预期分别为 `57a6882d041e4eedb169d824d11e2f9b1b590b521ae9562a24f1bc4eed183c09`、`3e27cd28795585abffd1e481c153cdfa58e2aca652f5f872a33e8ca1d8104c41`。检查 `nio_start_in_dir` 是否在 TCP 启用时仍调用 `UnixListener::bind`，以及错误是否传播为启动失败。只读审查，不修改。
> 2. 使用 Python 标准库，在工作区新建的独占短目录和 `/tmp` 下新建的独占短目录分别探测文件型 `AF_UNIX/SOCK_STREAM`。先 chdir 到新目录，创建 `run/`，使用与 SeekDB 相同的相对路径 `run/sql.sock`；记录 socket 构造、bind、listen、一次连接收发各阶段的成功/失败。设置短超时。失败记录完整异常、数字 errno、errno 名称、阶段和实际目录。增加 localhost TCP 控制。不用抽象 socket 或 socketpair 替代文件型 socket；只移除本探针自己创建的 socket。
> 3. 保存并返回探针源码、原始输出和结论。若两目录均失败，到此结束；若某目录成功，返回可用位置，下一轮再接收精确版本的预编译测试包。既有的进程跨调用限制可以通过单个持续命令内完成启动、SQL 和关闭来处理，不必为了持久化改变安全设置。
>
> 这次不运行 SeekDB、不跑 A/A，不使用其他 GitHub 分支的计时代表当前 v4。后续即便能启动，也要另做同版校准；未知配额与共享负载仍需实际测量评估。结果请区分“源码核对通过”“socket 门槛通过”“SeekDB 未执行”。

## 3. 修改清单

仅新增本目录交接与实验记录。不修改产品文件、分支、历史、旧证据或受保护工作区。不生成兼容性补丁，不推送。

## 4. 已完成检查与未执行测试

- 两文件分别从本地 v3 和产品提交导出并 SHA256 核对：2/2 相同；当前工作文件与产品提交的 diff 为零。
- GitHub 只读查询：v3 分支 tip 为 `7accd1ace8ee5d035b3ef8aaf426c46b1d0a595e`；查询产品提交返回 HTTP 422 `No commit found for SHA`。当次分支列表没有 `feat/fts-opt-v4` 或 `feat/fts-opt-v5`。
- 直接联通检查未获得 Dot 会话。应用会话列表不含目标，CUA 无 Dot 浏览器，CUA 明确禁止操作 `com.openai.codex`；未绕过限制。用户随后选择手工代传。
- 云端 socket 复核、二进制依赖检查、SeekDB 功能初筛、正式回归、A/A、A/B：本轮均未执行。

## 5. 验收结论与已知问题

源码门槛已核对。Dot 是否可运行当前参考仍未验收。文件型 socket 拒绝的具体阶段和 errno 缺失；若为环境普遍限制，TCP 客户端不能绕开当前服务器的 Unix listener 启动要求。缺工具链使源码构建尚不具备已确认条件。原有 V4 全套回归与性能门禁状态不变。

## 6. Checkpoint 与回退

P0：`0548d153fe5a9e958e8843c6232702d15d921ff5`，仅本地交接文件新增。产品维持 `194150497aca9a69e395317bd6d5a64f26340a61`。本轮新产品实验 0，累计 1/8，正式候选 A/B 0 对。无需代码回退；保留本目录与所有历史证据。

## 7. 下一阶段

用户代传第 2 节指令，取得原始 errno 和可用目录。门槛通过后，再制作或传递精确参考二进制及客户端，核对动态依赖与资源，然后在单个命令内做有界功能初筛。GitHub 上缺失的 v4 代码需要单独对齐，继续遵守暂不推送约束。
