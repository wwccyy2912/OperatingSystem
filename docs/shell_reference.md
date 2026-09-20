# OpSys Shell 命令参考

> 适用版本：OpSys v0.9-dev（HEAD 105805d + 工作区）　|　最后更新：2026-09-20
>
> `shell` 是 OpSys 的命令行前端（`user/services/shell/shell.c`，约 4.3k 行）。本文列出它的**全部内置命令**、行编辑快捷键、路径语法、权限行为与典型输出。所有条目均以源码中的 `ShellRegisterCommand()` 注册表与各 `Cmd*()` 实现为准。

---

## 一、基本使用

### 1.1 进入 Shell

`shell` 由 `manager` 在**最后**拉起（`user/services/manager/manager.c`），因此它的提示符出现即代表全部系统服务已就绪：

```text
OpSys ... (启动画面)
login: admin
password: *****

opsys:/$ 
```

| 项 | 值 |
| --- | --- |
| 默认账户 | `admin` / `admin`（角色 OWNER） |
| 提示符 | `opsys:<当前目录>$`（例如 `opsys:/Volumes/System$`） |
| 命令来源 | 内置注册表（`ShellRegisterCommand`），线性 `strcmp` 派发 |
| 输入通道 | `IpcCall(READ_BLOCK)` 到 `keyboard` 端口（阻塞式） |
| 输出通道 | `IpcCall(WRITE)` 到 `term` 端口 → 渲染到**帧缓冲** |

> ⚠️ **shell 的输出不在串口上**。串口只承载各服务 `printf` 的调试日志。自动化脚本需要 `screendump` 截图 + `tools/vga_decode.py` 解码才能读屏（见 [testing_guide.md](testing_guide.md)）。

### 1.2 行编辑与快捷键

| 按键 | 功能 |
| --- | --- |
| `↑` / `↓` | 历史记录回退 / 前进（`HIST_MAX = 16` 条，可编辑后再执行） |
| `PgUp` / `PgDn` | 跳到最早 / 最新一条历史 |
| `←` / `→` | 光标左右移动（**按 UTF-8 码位**，不会切碎多字节字符） |
| `Home` / `Ctrl-A` | 行首 |
| `End` / `Ctrl-E` | 行尾 |
| `Ctrl-U` | 删除光标前全部内容 |
| `Ctrl-K` | 删除光标后全部内容 |
| `Ctrl-W` | 删除前一个单词（UTF-8 感知） |
| `Ctrl-L` | 清屏并重绘当前行 |
| `Ctrl-C` | 取消当前行 |
| `Ctrl-D` | 空行时退出 shell |
| `Tab` | 补全（命令名 + 路径，最多 64 个候选） |
| `Ctrl+Space` | 切换拼音输入法（同 `ime` 命令） |

### 1.3 路径与 URL 语法

OpSys 的文件系统使用**卷名前缀 URL**，而不是 POSIX 绝对路径：

```
/Volumes/<卷名>/<目录>/<文件>        完整写法
/<卷名>/<目录>/<文件>                 /Volumes 视图层可省略
<相对路径>                            相对于 shell 当前目录（cwd）
```

- `/` 与 `/Volumes` 是**卷列表视图**（`ls` 会列出所有已挂载卷）。
- 服务端解析时会剥离可选的 `Volumes` 层：`/Volumes/System/x` 与 `/System/x` 等价（`user/services/vfs/vfs_server.c:380`）。
- `cd` 会改变 cwd，之后所有路径参数都相对它解析（`ShellResolvePath()`，`shell.c:1696`）。

内置的两个卷（`vfs_server.c:116-118` 的挂载配置表）：

| 卷名 | 驱动 | 读写 | 说明 |
| --- | --- | --- | --- |
| `System` | `mem`（内存卷） | **只读** | 引导时由 `fs_mem_driver` 挂载，含系统示例文件 |
| `Disk` | `virtio_blk` | **读写** | 由 `fs_virtio_blk_driver` 挂载到 `disk.img`，跨重启持久化 |

---

## 二、命令总表

| 命令 | 语法 | 一句话说明 |
| --- | --- | --- |
| [help](#31-会话与信息) | `help` | 列出全部命令与其帮助文本 |
| [echo](#31-会话与信息) | `echo <text...>` | 原样打印 |
| [pid](#31-会话与信息) | `pid` | 显示当前进程 PID |
| [free](#31-会话与信息) | `free` | 显示空闲物理内存页数 |
| [uptime](#31-会话与信息) | `uptime` | 显示系统 tick 计数 |
| [clear](#31-会话与信息) | `clear` | 清屏 |
| [exit](#31-会话与信息) | `exit` | 退出 shell |
| [reboot](#31-会话与信息) | `reboot` | 停机 |
| [shutdown](#31-会话与信息) | `shutdown` | 关机（需 `ATOM_SYS_SHUTDOWN`） |
| [ps](#32-进程与内核) | `ps` | 列出进程 |
| [kill](#32-进程与内核) | `kill [pid] [signum]` | 发信号（无 pid 时弹 TUI 选择器） |
| [stop](#32-进程与内核) | `stop <svc>` | 停止系统服务（需确认 + 密码） |
| [threads](#32-进程与内核) | `threads` | 创建测试工作线程 |
| [mutex](#32-进程与内核) | `mutex` | 内核互斥锁演示 |
| [sleep](#32-进程与内核) | `sleep <ticks>` | 睡眠 N 个 tick |
| [exec](#32-进程与内核) | `exec [blob\|path]` | 从内核 blob 或 VFS 文件启动进程 |
| [cap](#32-进程与内核) | `cap` | 创建能力（测试） |
| [ports](#32-进程与内核) | `ports` | 列出已注册的 IPC 端口 |
| [ls](#33-文件操作) | `ls [url]` | 列目录 / 卷 |
| [cat](#33-文件操作) | `cat <url>` | 显示文件内容 |
| [stat](#33-文件操作) | `stat [url]` | 卷容量与使用量 |
| [tee](#33-文件操作) | `tee <url> <text>` | 写文件 |
| [fallocate](#33-文件操作) | `fallocate <url>` | 持续写入直到 ENOSPC（压力测试） |
| [mkdir](#33-文件操作) | `mkdir <url>` | 建目录 |
| [rm](#33-文件操作) | `rm <url>` | 删除条目 |
| [mv](#33-文件操作) | `mv <src> <dst-dir\|?> [new-name]` | 移动 / 重命名 |
| [cd](#33-文件操作) | `cd [dir]` | 切换目录 |
| [pwd](#33-文件操作) | `pwd` | 打印当前目录 |
| [fm](#33-文件操作) | `fm [dir]` | 全屏 TUI 文件管理器 |
| [scroll](#33-文件操作) | `scroll [lines\|end]` | 翻看终端回滚缓冲 |
| [bm_create](#34-权限与书签) | `bm_create <url> [r\|w\|rw]` | 创建安全作用域书签（触发 Powerbox） |
| [bm_resolve](#34-权限与书签) | `bm_resolve` | 解析缓存的书签为句柄 |
| [bm_revoke](#34-权限与书签) | `bm_revoke` | 服务端丢弃书签 |
| [bm](#34-权限与书签) | `bm <create\|resolve\|revoke> ...` | 上述三条的 UNIX 风格分发 |
| [perm_answer](#34-权限与书签) | `perm_answer <id> <y\|n>` | 应答 Powerbox 询问 |
| [perm_query](#34-权限与书签) | `perm_query [id]` | 查看待处理询问 |
| [perm_revoke](#34-权限与书签) | `perm_revoke [subject_id]` | 撤销授权 |
| [perm](#34-权限与书签) | `perm <answer\|query\|revoke> ...` | 上述三条的 UNIX 风格分发 |
| [login](#35-账户与角色) | `login [name] [password]` | 登录 |
| [logout](#35-账户与角色) | `logout` | 登出 |
| [whoami](#35-账户与角色) | `whoami` | 显示当前账户与角色 |
| [passwd](#35-账户与角色) | `passwd [name]` | 改密 |
| [useradd](#35-账户与角色) | `useradd <name> <role> [password]` | 建号（管理员） |
| [userdel](#35-账户与角色) | `userdel <name>` | 删号（管理员） |
| [user_lock / userlock](#35-账户与角色) | `user_lock <name>` | 禁用账户（管理员） |
| [user_unlock / userunlock](#35-账户与角色) | `user_unlock <name>` | 启用账户（管理员） |
| [users](#35-账户与角色) | `users` | 列出账户（管理员） |
| [pkg](#36-包管理) | `pkg <install\|list\|run\|remove>` | `.ops` 包管理 |
| [policy_set](#37-策略与环境) | `policy_set <role> <cmd> <allow\|deny\|unset>` | 热更新命令策略（管理员） |
| [policy_dump](#37-策略与环境) | `policy_dump` | 导出策略表（管理员） |
| [policy](#37-策略与环境) | `policy <set\|dump> ...` | 上述两条的分发 |
| [export](#37-策略与环境) | `export NAME=value` | 设置环境变量 |
| [unset](#37-策略与环境) | `unset NAME` | 删除环境变量 |
| [env](#37-策略与环境) | `env` | 打印环境变量 |
| [net](#38-设备与界面) | `net <mac\|arp\|ping\|tcp\|recv\|stats>` | 网卡与协议栈（底层诊断） |
| [gui](#38-设备与界面) | `gui` | 进入像素桌面（`gui_demo`） |
| [mouse](#38-设备与界面) | `mouse` | 读取 PS/2 鼠标状态 |
| [ime](#38-设备与界面) | `ime [on\|off]` | 拼音输入法开关 |
| [df](#39-v09-工具族) | `df [vol\|url]` | 卷容量/已用/可用/使用率 |
| [du](#39-v09-工具族) | `du [url]` | 递归统计子树大小 |
| [cp](#39-v09-工具族) | `cp <src> <dst>` | 复制文件或目录树 |
| [touch](#39-v09-工具族) | `touch <url>` | 创建空文件 |
| [head](#39-v09-工具族) | `head <url> [n]` | 查看文件前 n 行（默认 10） |
| [hexdump](#39-v09-工具族) | `hexdump <url> [bytes]` | 十六进制 + ASCII 转储 |
| [tree](#39-v09-工具族) | `tree [url]` | 目录层级视图 |
| [wc](#39-v09-工具族) | `wc <url>` | 行/词/字节统计 |
| [disk](#38-设备与界面) | `disk <list\|info\|sync\|check\|read\|mount\|unmount\|format\|fill>` | 磁盘/卷管理（管理员） |
| [power](#39-v09-工具族) | `power [status\|sync\|off\|reboot\|halt] [-f] [-s] [-n]` | 电源管理与优雅关机 |
| [poweroff](#39-v09-工具族) | `poweroff` | 刷盘后关机（power off 的别名） |
| [halt](#39-v09-工具族) | `halt` | 刷盘后停机（CPU 停住，不复位） |
| [restart](#39-v09-工具族) | `restart` | 刷盘后重启 |
| [svc](#39-v09-工具族) | `svc <list\|status\|start\|stop\|restart> [name]` | 服务监管（管理员） |
| [ip](#39-v09-工具族) | `ip [show\|mac] \| ip set <a.b.c.d> [gw]` | 网络接口状态与静态配置 |
| [netstat](#39-v09-工具族) | `netstat` | 接口概要 + 收发包计数 |
| [udp](#39-v09-工具族) | `udp <bind\|send\|recv> ...` | UDP 数据报收发 |
| [dns](#39-v09-工具族) | `dns <name> [server]` | A 记录解析（UDP/53） |
| [http](#39-v09-工具族) | `http <ip> <port> [path]` | 用 TCP 主动连接抓取 HTTP 对象 |
| [perm audit](#310-权限引擎管理v10) | `perm audit [n] [allow|deny] [subject=<id>] [since=<tick>]` | 查看审计环（可过滤） |
| [perm ctx](#310-权限引擎管理v10) | `perm ctx [list|<subject|pid> fg|bg]` | 前台/后台上下文表与绑定 |
| [perm freq](#310-权限引擎管理v10) | `perm freq [subject|all] | perm freq release <subject|all>` | 频率计数与隔离解除 |
| [perm save / load](#310-权限引擎管理v10) | `perm save [url] [all] | perm load [url]` | 策略快照导出 / 导入（管理员） |
| [permguard](#310-权限引擎管理v10) | `permguard [list|<subject|pid> [release]]` | 隔离状态管理入口 |

---

## 三、命令详解

### 3.1 会话与信息

#### `help`

遍历命令注册链表并打印"命令名（左对齐 12 列）+ 帮助文本"，输出顺序 = 注册顺序。

```text
opsys:/$ help
Available commands:
  help          Show this help
  echo          Print text: echo <message>
  pid           Show current process PID
  ...
```

#### `echo <text...>`

把参数以空格连接后输出。无参数时输出空行。

```text
opsys:/$ echo hello opsys
hello opsys
```

#### `pid` / `free` / `uptime`

| 命令 | 输出 | 数据来源 |
| --- | --- | --- |
| `pid` | 当前 shell 进程的 PID | `SYS_GET_PID` |
| `free` | 空闲物理内存页数与 MB 数 | `SYS_GET_FREE_PAGES` |
| `uptime` | 系统启动以来的 tick 数（PIT 100 Hz） | `SYS_GET_TIME` |

#### `clear`

向 `term` 发送清屏操作并把光标复位到左上角。

#### `exit` / `reboot` / `shutdown`

| 命令 | 行为 | 权限 |
| --- | --- | --- |
| `exit` | 结束 shell 进程（`Ctrl-D` 空行等价） | — |
| `reboot` | 调用停机路径，系统停在 halt 循环 | — |
| `shutdown` | 调用 `SYS_SHUTDOWN` 关闭机器 | 需要 `ATOM_SYS_SHUTDOWN` 能力，未授权返回 `ERR_NOCAP` |

### 3.2 进程与内核

#### `ps`

调用 `SYS_PROCESS_LIST` 打印进程表（PID、名字、状态、线程数、退出码）。

```text
opsys:/$ ps
  PID  STATE      THR  NAME
    1  RUNNING      1  init
    2  RUNNING      2  manager
    3  RUNNING      3  serial
    ...
```

#### `kill [pid] [signum]`

- **不带参数**：弹出 TUI 进程选择器（`j/k` 移动，`Enter` 确认，`q` 取消），默认信号 `SIGKILL`。
- **带参数**：`kill <pid> [signum]`，`signum` 取值范围 `1..NSIG-1`（`NSIG = 64`）。
- **管理员路径**：若当前登录账户是 OWNER/ADMIN 且信号为 `SIGKILL`，shell 会**经 `user` 服务代理**杀进程（shell 自身没有 `ATOM_SERVICE_MANAGE`，直接跨进程 `SIGKILL` 会被内核以 `ERR_NOCAP` 拒绝）；代理会二次校验管理员身份并保护系统关键服务。
- **非管理员路径**：走直接 `Kill()`，只能对自己发信号，对外部 PID 会被内核门控拒绝。

```text
opsys:/$ kill 27
kill: PID 27 SIGKILL sent by admin
```

#### `stop <svc-name>`

停止一个系统服务进程，带**三重保护**（`user/services/user/main.c`、`shell.c:CmdStop`）：

1. TUI 确认框（`y` 继续）；
2. 显示当前账户（`whoami`）；
3. **掩码密码输入** + `USER_OP_VERIFY` 校验；
4. 校验通过后 `user` 服务执行 `USER_OP_STOP`，**再次校验**调用者角色为 OWNER/ADMIN，并拒绝关闭关键服务。

```text
opsys:/$ stop pkg
Stop system program 'pkg'? (y/n) y
Account: admin (OWNER)
Password: *****
stop: pkg stopped
```

#### `threads` / `mutex` / `sleep <ticks>` / `cap` / `ports`

| 命令 | 用途 |
| --- | --- |
| `threads` | 创建若干测试工作线程并观察调度 |
| `mutex` | 多线程竞争内核互斥锁的计数演示（验证锁语义） |
| `sleep <ticks>` | 当前线程睡眠 N 个 tick（`SYS_SLEEP`；0 会被钳到 1） |
| `cap` | 创建一个能力句柄并打印句柄值（能力系统测试） |
| `ports` | 枚举内核端口表，显示端口号 → 属主 PID/名字 |

#### `exec [blob|path]`

启动一个用户进程，两种来源：

| 形式 | 来源 | 示例 |
| --- | --- | --- |
| `exec <name>` | 内核内嵌 blob（`SYS_BLOB_GET`） | `exec hello`、`exec wm_demo`、`exec gui_demo` |
| `exec <path>` | VFS 中的 ELF 文件（路径含 `/` 或以 `.` 开头） | `exec /Volumes/Disk/app.elf`、`exec ./app.elf` |

可用 blob 名（`Makefile` 的 `SVC_NAMES`，去掉 `_blob` 后缀）：`init` `manager` `serial` `keyboard` `term` `shell` `flaky` `crashpeer` `canarytest` `hello` `vfs` `fs_mem_driver` `fs_virtio_blk_driver` `perm` `device_mgr` `pkg` `sbox_demo` `runtime_demo` `tui_demo` `window_demo` `user` `wm` `wm_demo` `policy` `gui` `gui_demo` `net`。

```text
opsys:/$ exec hello
exec: fetched hello.elf blob from kernel (12345 bytes)
exec: created PID 31
```

> 无参数时默认启动 `hello`。缓冲区上限 256 KiB，须容纳最大的服务 ELF。

### 3.3 文件操作

#### `ls [url]`

| 参数 | 行为 |
| --- | --- |
| 无参数 | 列出 cwd |
| `/`、`/Volumes`、`/Volumes/` | 调 `FsListVolumes` 列出**已挂载卷**及读写属性 |
| 目录 URL | 走 `VFS_OP_ENUM_BEGIN` / `ENUM_NEXT`（分页）列出条目 |

```text
opsys:/$ ls
System  (ro)
Disk
opsys:/$ ls /Volumes/System
docs/        hello.txt    notes.txt
```

#### `cat <url>`

打开文件（只读）并连续读取打印。若文件在磁盘卷上且**尚未授权**，会触发 Powerbox。

#### `stat [url]`

显示卷的总容量 / 已用容量 / 是否只读（`VFS_OP_STAT_VOLUME`）。无参数时默认统计 `/System` 与 `/Disk`（源码中 `vols[2] = {"/Volumes/System", "/Volumes/Users"}`）。

#### `tee <url> <text>`

写入文本到文件（`VFS_OP_WRITE`）。**首次写磁盘卷会触发 Powerbox 授权**：询问面板弹出后，shell 命令返回 `-105 (EACCES)`，需要先 `perm_answer <id> y`，然后**重新执行** `tee`。

```text
opsys:/$ tee /Volumes/Disk/hello.txt hi
tee: FAILED (-105) — permission required (query 3)
opsys:/$ perm_answer 3 y
perm: query 3 ALLOWED
opsys:/$ tee /Volumes/Disk/hello.txt hi
tee: wrote 2 bytes to /Volumes/Disk/hello.txt
```

#### `fallocate <url>`

对目标文件持续写入直到卷满（`ENOSPC`），用于验证容量边界与错误路径。属于**破坏性**的容量压力命令，请只在 `Disk` 卷上使用。

#### `mkdir <url>` / `rm <url>`

建目录 / 删除条目（`VFS_OP_CREATE_DIR` / `VFS_OP_DELETE_ITEM`，删除带递归参数）。两者都受能力门控，未授权同样会走 Powerbox。

#### `mv <src> <dst-dir|?> [new-name]`

移动或重命名条目（`VFS_OP_MOVE`）。设计要点：移动后 **`ItemID` 不变**，因此已签发的书签在移动后依然可用（`docs/vfs_design.md` §5）。

- `mv <src> <dst-dir>`：保持原名移动；
- `mv <src> <dst-dir> <new-name>`：移动并改名；
- 目标目录传 `?` 时会弹出 TUI 目录选择器。

#### `cd [dir]` / `pwd`

`cd` 支持 `cd`（回 `/`）、`cd ..`、`cd /Volumes/Disk`、`cd sub`。cwd 会反映在提示符里（`opsys:<cwd>$`）。`pwd` 打印当前目录。

#### `fm [dir]`

全屏 **TUI 文件管理器**（基于 `TuiMenu`）：

| 按键 | 功能 |
| --- | --- |
| `j` / `k` | 上下移动 |
| `Enter` | 进入目录 / 查看文件 |
| `v` | 查看（`cat`）选中文件 |
| `d` | 删除选中条目 |
| `q` | 退出 |

在 `/` 下运行时先显示卷列表（`Volumes (j/k, Enter, q)`），选中后进入该卷。

#### `scroll [lines|end]`

进入**终端回滚缓冲**分页视图（`TERM_OP_SCROLLVIEW`）：

| 参数 | 行为 |
| --- | --- |
| 无参数 | 默认回退 20 行 |
| `scroll <n>` | 回退 n 行 |
| `scroll end` | 跳到缓冲末尾（回到底部，等价于退出视图） |
| 视图中 `PgUp`/`↑` | 上翻 20 行 |
| 视图中 `PgDn`/`↓` | 下翻 20 行 |
| 视图中 `q` / `Enter` | 退出回滚视图 |

> 该命令自身**不输出任何文本**——任何一次终端写入都会把视图复位到实时画面。

### 3.4 权限与书签

#### `bm_create <url> [r|w|rw]`

向 vfs 申请创建一个**安全作用域书签**。这是受保护操作：未授权时会触发 Powerbox，授权后服务端返回一个不透明 blob（书签），shell 把它缓存起来。

| 权限参数 | 含义 |
| --- | --- |
| `r`（默认） | 只读 |
| `w` | 只写 |
| `rw` | 读写 |

#### `bm_resolve` / `bm_revoke`

- `bm_resolve`：把缓存的书签解析为临时文件句柄，验证"移动后书签仍有效"。
- `bm_revoke`：让服务端丢弃该书签。

#### `perm_answer <id> <y|n>` / `perm_query [id]` / `perm_revoke [subject_id]`

| 命令 | 作用 |
| --- | --- |
| `perm_answer <id> y` | 批准询问 → `perm` 签发能力（`CapGrantToSubject`） |
| `perm_answer <id> n` | 拒绝 → 发起方拿到 `EACCES` |
| `perm_query` | 显示当前待处理的询问（无参数显示全部） |
| `perm_revoke [subject]` | 撤销授权（按 subject 或全部），底层是 `SYS_CAP_REVOKE_BY_ATOM` |

#### `bm` / `perm` 分发

`bm create|resolve|revoke ...` 与 `perm answer|query|revoke ...` 是上述命令的 UNIX 风格入口，语义完全相同；下划线形式保留为兼容别名。

### 3.5 账户与角色

| 命令 | 语法 | 说明 |
| --- | --- | --- |
| `login` | `login [name] [password]` | 不带参数时交互式提示（密码走**掩码输入**）；成功后内核 subject 绑定账户并把角色同步进权限引擎 |
| `logout` | `logout` | 解绑当前 subject，重新加载命令策略过滤表 |
| `whoami` | `whoami` | 显示账户名与角色（OWNER/ADMIN/STANDARD/CHILD/GUEST/AUDITOR） |
| `passwd` | `passwd [name]` | 改自己的密码；管理员可改他人 |
| `useradd` | `useradd <name> <role> [password]` | 建号（需 OWNER/ADMIN） |
| `userdel` | `userdel <name>` | 删号（不得删自己或最后一个管理员） |
| `user_lock` / `userlock` | `<name>` | 禁用账户 |
| `user_unlock` / `userunlock` | `<name>` | 启用账户 |
| `users` | `users` | 列出账户（管理员） |

**自动锁定策略**：连续 5 次登录失败会锁定账户（`USER_MAX_LOGIN_ATTEMPTS = 5`，`user/services/user/user.h`），需管理员 `user_unlock` 解锁。

```text
opsys:/$ login admin admin
login: admin (OWNER)
opsys:/$ useradd bob standard bobpw
useradd: 'bob' created (STANDARD)
opsys:/$ users
  admin   OWNER
  bob     STANDARD
```

### 3.6 包管理

`pkg <install <name> [--perms=a,b,c] | list | run <app_id> | remove <app_id>>`

| 子命令 | 说明 |
| --- | --- |
| `install <name>` | 安装 `.ops` 包；`--perms=` 显式指定申请的原子权限子集 |
| `list` | 列出已安装应用（app_id、名字、已签发能力） |
| `run <app_id>` | 以沙盒身份启动应用 |
| `remove <app_id>` | 卸载并回收其能力 |

`.ops` 包由宿主机的 `scripts/ops_pack.py pack <elf> <manifest> <out.ops>` 生成；格式与权限映射见 [ops_format.md](ops_format.md)。

```text
opsys:/$ pkg list
  hello.app     hello    1.0   perms: docs.read
opsys:/$ pkg run hello.app
pkg: running hello.app (PID 34)
```

### 3.7 策略与环境

#### `policy_set <role> <cmd> <allow|deny|unset>`

热更新指定角色的命令策略（`policy` 服务，`POLICY_OP_SET`）。需要 OWNER/ADMIN；`user` 服务提供管理员代理通道。

```text
opsys:/$ policy_set guest exec deny
policy: GUEST exec = deny
```

#### `policy_dump`

导出整张策略表（`role cmd verdict` 行）。

**命令策略三层架构**（见 [permission_model.md](permission_model.md) §9.5.4）：

1. **Capability（内核）** —— 硬边界，不可伪造；
2. **Policy DB（`policy` 服务）** —— 角色 × 命令 的 allow/deny；
3. **Shell 覆盖** —— 登录后从服务拉取并过滤自己的命令表；服务不可用时回退到内置**救急命令列表**（保证 shell 仍可用）。

`guest` 登录后 `exec` / `kill` 会被 shell 在派发前拦截。

#### `export` / `unset` / `env`

```text
opsys:/$ export PS1=opsys
opsys:/$ export
PS1=opsys
opsys:/$ env
PS1=opsys
opsys:/$ unset PS1
```

> 环境变量是**纯用户偏好**，**不承载任何安全策略**——权限永远由能力/角色/策略决定（`policy/policy.h` 头部注释）。`PS1` 影响提示符显示。

### 3.8 设备与界面

#### `disk <list|mount|unmount|format|fill> [vol] [bytes]`

| 子命令 | 说明 |
| --- | --- |
| `disk list` | 列出卷及 `已用/总量 (KiB)`，只读卷标 `(ro)` |
| `disk mount <vol>` | 挂载卷 |
| `disk unmount <vol>` | 卸载卷 |
| `disk format <vol>` | 擦除并重新格式化（**破坏性**，要求输入大写 `YES` 确认） |
| `disk fill <vol> [bytes]` | 向 `<vol>/fill.bin` 写入直到 `ENOSPC` 或达到预算，打印写入 KiB |

> `mount/unmount/format/fill` 都经 `user` 服务**管理员代理**执行：服务端持有 `ATOM_SERVICE_MANAGE`（驱动控制面要求该能力）并再次校验调用者是 OWNER/ADMIN。**shell 从不直接与驱动对话。**

```text
opsys:/$ disk list
System  128 KiB used / 32768 KiB total (ro)
Disk    4 KiB used / 8192 KiB total
disk: 2 volume(s)
opsys:/$ disk fill Disk 65536
disk: 64 KiB written to Disk/fill.bin
```

#### `net <mac|arp|ping <ip>|tcp <ip> <port> <msg>|recv|stats>`

| 子命令 | 说明 |
| --- | --- |
| `net mac` | 读取网卡 MAC 地址（`NET_OP_GET_MAC`） |
| `net arp` | 手工构造 ARP who-has `10.0.2.2`（slirp 网关）并轮询等待应答 |
| `net ping <ip>` | ICMP echo（`NET_OP_PING`） |
| `net tcp <ip> <port> <msg>` | TCP 连接、发送消息、收回应答（单连接状态机） |
| `net recv` | 轮询接收原始以太网帧 |
| `net stats` | 打印收/发/错误计数 |

协议细节与 QEMU 网卡配置见 [net_design.md](net_design.md)。

#### `gui`

启动像素桌面演示（`gui_demo`）：激活合成器 → 创建 Keys / Canvas / Info 三个窗口 → 键盘输入跟随焦点窗口、鼠标点击可在 Canvas 上作画 → 按 `q` 或 `Esc` 退出，合成器关闭后 `term` 重新绘制文本屏。详见 [gui_design.md](gui_design.md)。

#### `mouse`

直接向 `keyboard` 服务读取 PS/2 鼠标状态（位移增量与按键），用于在文本界面下验证鼠标驱动。

#### `ime [on|off]`

显示或切换拼音输入法（等价于 `Ctrl+Space`）。开启后：输入拼音字母 → `Space` 或数字键 `1-9` 选择候选字。实现见 [i18n_design.md](i18n_design.md)。

### 3.9 v0.9 工具族

这一节收录随「磁盘工具 / 开关机工具 / 网络工具」一起加入的命令。它们分布在四个模块里（`user/services/shell/cmd_fs.c`、`cmd_disk.c`、`cmd_power.c`、`cmd_net.c`），每个模块在自己的注册函数里登记命令，shell 核心只负责在建表时调用它们。

#### 文件系统工具（cmd_fs.c）

| 命令 | 语法 | 说明 |
| --- | --- | --- |
| `df` | `df [vol\|url]` | 列出每个已挂载卷的 **总/已用/可用/使用率**；带参数时只显示匹配的那一个（支持卷名或 `/Volumes/...` URL），未挂载则打印 `(stat unavailable)` |
| `du` | `du [url]` | 递归统计子树（默认当前目录）的文件数、目录数、总字节，输出 `<KiB>\t<url>` 加一行汇总；深度上限 `FS_MAX_DEPTH = 8`，触顶会打印 "result truncated" |
| `cp` | `cp <src> <dst>` | 复制文件；`dst` 若是已存在目录（或以 `/` 结尾）则复制到该目录下并沿用源文件名。**源是目录时递归复制**（`CopyTree`，同样受深度限制） |
| `touch` | `touch <url>` | 文件不存在则创建空文件；已存在则报告大小、不做修改 |
| `head` | `head <url> [n=10]` | 输出前 n 行；**文件末尾没有换行时最后一段也算一行** |
| `hexdump` | `hexdump <url> [bytes=256]` | 每行 16 字节：偏移 + 十六进制 + `\|ASCII\|`；非可打印字符显示为 `.` |
| `tree` | `tree [url]` | 递归打印目录层级（目录加 `/`、文件带字节数），最后汇总目录/文件数 |
| `wc` | `wc <url>` | 行数 / 词数 / 字节数 |

> 这些命令都是 libfs 客户端：所有 I/O 都经 `vfs` 端口，未授权的读写会返回 `-105 (EACCES)` 并提示走 Powerbox，与 `ls`/`cat`/`tee` 完全一致。

#### 磁盘工具（disk，cmd_disk.c）

`disk` 命令族在 v0.9 扩展为完整的管理面：

| 子命令 | 说明 |
| --- | --- |
| `disk list` | 卷 + 驱动名 + 容量/已用 + 挂载状态（表格式） |
| `disk info [vol]` | 卷详情：驱动、是否持久化、只读位、块大小、块/索引节点用量、容量、UUID。**默认 `Disk`**；对内存卷（`System`）会打印 vfs 侧的容量与根条目并明确说明"没有控制面" |
| `disk sync [vol]` | 刷盘。不带参数时经 `VFS_OP_SYNC` 刷新**所有**已挂载卷；带卷名时经管理员代理刷该卷 |
| `disk check [vol]` | **只读**一致性扫描（磁盘卷）：签名、索引节点表、父子链、extent 范围与重叠、名字终止；输出 `clean` 或问题数与第一处错误 |
| `disk read <lba> [sectors] [vol]` | 原始扇区转储（调试用），一次最多 64 扇区、每扇区 512 字节，输出格式同 `hexdump` |
| `disk mount\|unmount <vol>` | 重新注册 / 注销卷 |
| `disk format <vol>` | 擦除并重新格式化（**破坏性**，要求输入大写 `YES`） |
| `disk fill <vol> [bytes]` | 向 `fill.bin` 持续写入直到 `ENOSPC` 或预算用尽 |

> `info/sync/check/read/mount/unmount/format/fill` 全部走**管理员代理链**：shell → `user` 服务（校验 OWNER/ADMIN）→ 块设备驱动控制面（再次以 `ATOM_SERVICE_MANAGE` 门控）。**shell 从不直接与驱动对话。**

#### 开关机与服务监管（cmd_power.c）

| 命令 | 说明 |
| --- | --- |
| `power status` | 运行时长（tick/秒）、空闲内存、进程数、卷列表、服务在跑数 |
| `power sync` | 刷新所有卷（等同 `sync`） |
| `power off` | 确认 → 刷盘 → ACPI S5 断电 |
| `power reboot` | 确认 → 刷盘 → 复位 |
| `power halt` | 确认 → 刷盘 → 停住 CPU（`SYS_HALT`，不复位；QEMU 里表现为客户机停住） |
| `poweroff` / `halt` / `restart` | 上面三条的免参数别名（仍会要求确认） |
| `svc list` | 服务清单：名字、PID、运行状态、是否被监控自动重启 |
| `svc status <name>` | 单个服务的状态 |
| `svc start <name>` | 拉起一个已停止的服务（从内核 blob 重新创建进程） |
| `svc stop <name>` | 停止服务且**不触发自动重启**（manager 的 `stop_requested`）；拒绝停止 `manager`/`init`，shell 侧还会拒绝 `shell`/`user` 自身 |
| `svc restart <name>` | 停止后重新拉起 |

选项：`-f/--force` 跳过确认，`-s/--strict` 刷盘失败即中止，`-n/--no-sync` 跳过刷盘。`halt` 与 `reboot`/`shutdown` 都需要 `ATOM_SYS_SHUTDOWN`（OWNER/ADMIN 登录时由 `user` 服务签发）。

`svc` 同样经管理员代理：shell → `user` 服务 → manager 的 `manager` 控制端口（`svc_req_t`/`svc_resp_t`，见 [service_reference.md](service_reference.md)）。

#### 网络工具（cmd_net.c）

| 命令 | 说明 |
| --- | --- |
| `ip` / `ip show` | 显示接口名、IPv4 地址、网关、MAC 与链路状态 |
| `ip mac` | 只打印 MAC |
| `ip set <a.b.c.d> [gw]` | 设置静态地址与网关（`NET_OP_SET_IP`）。**注意**：协议栈当前仍以 `10.0.2.2`（slirp 网关）静态路由，命令会如实提示这一点 |
| `netstat` | 接口概要 + 收发/错误计数 |
| `udp bind <port>` | 绑定本地 UDP 端口 |
| `udp send <ip> <sport> <dport> <text>` | 发送一个数据报（文本后自动补换行） |
| `udp recv <port> [tries]` | 轮询接收（默认 50 次 × 10 ms），收到后打印内容并解绑 |
| `dns <name> [server]` | 手工构造 DNS A 查询（默认服务器 `10.0.2.3`，即 slirp 解析器），解析首个 A 记录；带超时与事务 ID 校验 |
| `http <ip> <port> [path]` | 用 `NET_OP_TCP_CONNECT` 主动连接，发送 HTTP/1.0 GET，打印原始响应（头 + 体，最多 2 KB） |

`.ops` 应用、Powerbox 与这些工具共用同一套端口协议；网络命令需要 QEMU 真的挂了网卡 —— `make run` 现在默认附带 `-netdev user,id=n0 -device pcnet,netdev=n0`，若手动启动请自行加上（详见 [getting_started.md](getting_started.md)）。

#### 用法示例

```text
opsys:/$ df
Filesystem   Driver          Size      Used      Avail  Use%
----------   ------------  --------  --------  --------  ----
System       mem              32768K        128K     32640K    0%  ro
Disk         virtio_blk        8192K          4K      8188K    0%
df: 2 volume(s) mounted

opsys:/$ disk info
volume      : Disk
driver      : virtio_blk
persistent  : yes (survives reboot)
blocks      : 16384 total, 258 used, 16126 free
uuid        : 6f707379732d7666-000001fd564245b6

opsys:/$ svc list
Service                PID   State      Supervision
---------------------  ----  ---------  --------------------
  serial                  3   running    -
  perm                    9   running    auto-restart
  ...
svc: 16/16 running

opsys:/$ http 10.0.2.2 8088 /
http: connecting to 10.0.2.2:8088 ...
http: GET / (78 bytes sent), waiting for the reply...
http: 121 byte(s) received
---- response ----
HTTP/1.0 200 OK
...
```

### 3.10 权限引擎管理（v1.0）

`perm` 命令族是权限引擎的管理面。它直连 `perm` 端口（shell 本身持 `ATOM_SERVICE_MANAGE`），
但**改变状态**的动词仍要人类身份：shell 先问 `user` 服务 `USER_OP_WHOAMI`，只有 OWNER/ADMIN 才能执行。
只读查询对任何调用者开放。完整语义见 [permission_reference.md](permission_reference.md)。

| 命令 | 语法 | 门控 | 说明 |
| --- | --- | --- | --- |
| `perm audit` | `perm audit [n] [allow\|deny] [subject=<id>] [since=<tick>]` | — | 审计环（最旧在前）：tick / subject / 事件 / atom / 裁决 / 资源。`n` 限制条数，`allow`/`deny` 过滤裁决，`subject=`/`since=` 进一步过滤 |
| `perm audit save` | `perm audit save [url]` | 管理员 | 审计导出为文本文件（默认 `/Volumes/Disk/perm.audit`） |
| `perm ctx` | `perm ctx [list]` | — | 前台/后台表（含隔离状态），`list` 可省略 |
| `perm ctx` | `perm ctx <subject\|pid> fg\|bg` | 管理员 | 绑定上下文。**后台主体被默认拒绝时不会弹出询问面板** |
| `perm freq` | `perm freq [subject\|all]` | — | 命中/拒绝计数、是否处于隔离、隔离剩余 tick |
| `perm freq release` | `perm freq release <subject\|all>` | 管理员 | 解除隔离 |
| `perm save` | `perm save [url] [all]` | 管理员 | 导出策略快照（默认 `/Volumes/Disk/perm.policy`）；`all` 连过期授权一起导出 |
| `perm load` | `perm load [url]` | 管理员 | 读回快照并热加载（全有或全无；失败时策略保持不变） |
| `permguard` | `permguard [list\|<subject\|pid> [release]]` | release 需管理员 | 隔离状态的专用入口（等价于 `perm freq` / `perm freq release`） |

> `perm answer` / `perm query` / `perm revoke`（Powerbox 裁决、查看待裁决询问、撤销授权）见 §3.4。

参数解析规则（`perm ctx` / `permguard` / `perm freq` 的 subject 参数）：**先按十进制 subject 解析并用
`ProcInfoBySubject` 验证该主体仍然存活**；否则当作 PID，用 `ProcessList` 确认存活后再从上下文表反查 subject；
两种情况都不成立就明确报错 —— 命令不会猜测，避免把 PID 当成 subject 误操作。

典型用法：

```text
opsys:/$ perm audit 6
  tick  subject  event          atom  verdict  resource
   812        1  CHECK_ALLOW       9  granted  6f70.../3
   815       24  CHECK_DENY        9  denied   6f70.../3
   816       24  POWERBOX          9  denied   6f70.../3
  ...
audit: 6 entries shown, ring capacity 64

opsys:/$ perm freq 24
perm freq: subject 24 - shell (PID 24)
  hits 12   denies 3   quarantined no   left 0 ticks
  policy: quarantine after 8 denials inside 1000 ticks (3000 ticks)

opsys:/$ perm ctx 24 bg
subject 24 is now background   (its default-deny requests will no longer prompt)

opsys:/$ perm save
3728-byte snapshot -> /Volumes/Disk/perm.policy
opsys:/$ perm load
3728-byte snapshot from /Volumes/Disk/perm.policy applied (roles + grants)
```

---

## 四、行为约定与注意事项

| 主题 | 说明 |
| --- | --- |
| **输出目标** | 命令输出走 `term` 端口（帧缓冲）；只有 libc `printf` 走串口调试通道 |
| **Powerbox 阻塞** | 询问面板会**抢占键盘焦点**，被拒绝/待授权的命令返回 `-105 (EACCES)`；应答后需**重新执行该命令** |
| **错误码** | 命令失败时打印负错误码：`-1 ERR_NOMEM`、`-2 ERR_INVAL`、`-3 ERR_NOCAP`、`-4 ERR_NOENT`、`-5 ERR_BUSY`、`-6 ERR_AGAIN`、`-7 ERR_FAULT`、`-8 ERR_OVERFLOW`、`-9 ERR_DENIED`、`-105 EACCES`（VFS 侧语义） |
| **路径大小写** | 卷名大小写敏感（`System` / `Disk`） |
| **条目名长度** | VFS 名称上限 64 字节（含 NUL），`fm` 每屏最多 64 项 |
| **历史容量** | `HIST_MAX = 16` 条 |
| **命令长度** | 行缓冲 `LINE_BUF_SIZE = 256` 字节，参数最多 `MAX_ARGS = 16` 个 |
| **管理员判定** | 多数管理命令通过 `user` 服务代理执行，服务端以 `IpcRecvFrom` 取到的真实 subject 绑定账户角色判定，**不信任命令行参数里的身份** |

---

## 五、典型操作流程

### 5.1 第一次写磁盘文件（含授权）

```text
opsys:/$ disk list
System  128 KiB used / 32768 KiB total (ro)
Disk    0 KiB used / 8192 KiB total
opsys:/$ tee /Volumes/Disk/note.txt hello
tee: FAILED (-105) permission required
opsys:/$ perm_query
query 1: shell (PID 12) WRITE /Disk/note.txt  [pending]
opsys:/$ perm_answer 1 y
perm: query 1 ALLOWED
opsys:/$ tee /Volumes/Disk/note.txt hello
tee: wrote 5 bytes
opsys:/$ cat /Volumes/Disk/note.txt
hello
```

### 5.2 建号、降权与策略限制

```text
opsys:/$ useradd guest guest gst
useradd: 'guest' created (GUEST)
opsys:/$ policy_set guest exec deny
policy: GUEST exec = deny
opsys:/$ logout
opsys:/$ login guest gst
login: guest (GUEST)
opsys:/$ exec hello
shell: command 'exec' is not permitted for role GUEST
opsys:/$ whoami
guest (GUEST)
```

### 5.3 磁盘持久化验证（跨重启）

```text
# 第一次启动
opsys:/$ tee /Volumes/Disk/persist.txt v1
opsys:/$ shutdown
# 重新 make run
opsys:/$ cat /Volumes/Disk/persist.txt
v1
```

### 5.4 界面演示

| 目标 | 命令 |
| --- | --- |
| 像素桌面（三窗口，鼠标作画） | `gui` |
| v0.4 文本窗口管理器 | `exec wm_demo` |
| 最小窗口闭环 | `exec window_demo` |
| TUI 组件演示 | `exec tui_demo` |
| Runtime 演示（malloc/信号/atexit） | `exec runtime_demo` |
| 沙盒应用演示 | `exec sbox_demo` |
| 全屏文件管理器 | `fm` |
| 终端回滚 | `scroll 40` |

---

## 六、源码索引

| 内容 | 位置 |
| --- | --- |
| 命令注册表（命令名 + 帮助文本 + 函数） | `user/services/shell/shell.c:3768-3856` |
| 行编辑与快捷键 | `shell.c` 的 `ReadLine()`（约 1050-1300 行） |
| 路径解析 | `shell.c:1696 ShellResolvePath()` |
| 命令策略过滤 | `shell.c:262-380 CmdFilter*` |
| 命令实现 | `shell.c` 中各 `Cmd<Name>()`（见 `grep -n "static int Cmd"`） |

相关文档：[architecture.md](architecture.md)（全局架构）、[service_reference.md](service_reference.md)（IPC 协议）、[permission_model.md](permission_model.md)（权限模型）、[vfs_design.md](vfs_design.md)（文件系统）、[ops_format.md](ops_format.md)（`.ops` 包）。

> 返回 [文档索引](README.md)
