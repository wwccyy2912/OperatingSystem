# OpSys 用户态服务与 IPC 协议参考

> 适用版本：OpSys v0.8-dev（git HEAD 105805d）　|　最后更新：2026-09-19
>
> 本文逐项列出 OpSys 全部用户态服务（user-space service）的进程映像、注册端口、启动顺序、IPC 报文体布局与关键流程时序，所有结论均标注源码出处。

---

## 0. 阅读约定与适用范围

### 0.1 事实来源

本文只描述**代码里真实存在**的东西。表格中的每个字段都可在下列位置核对：

| 事实类别 | 权威来源 |
| --- | --- |
| 服务清单、blob 名、启动顺序、重启策略 | `user/services/manager/manager.c` 的 `s_services[]`（132-149 行）与 `main()`（491-671 行） |
| blob 名的另一份权威副本 | `Makefile` 的 `SVC_NAMES`（206 行）与 `SVC_LINK_RULE`（258-292 行） |
| 端口名字面串 | 各服务的 `PortRegister(...)` 调用点（见 §2 表） |
| opcode / 报文体布局 | 各服务目录下的 `*.h`（`perm.h`、`pkg.h`、`user.h`、`policy.h`、`gui.h`、`net.h`、`vfs.h`）与 `user/lib/libwm/wm_proto.h` |
| 4096 字节上限 | `kernel/include/kernel/types.h:125`（`#define MAX_MSG_SIZE 4096`） |
| 发送方身份 | `kernel/ipc/ipc.c:210-216`（`IpcSenderSubject`）、`kernel/ipc/ipc.c:438-505`（`IpcRecvFrom`） |
| 管理原子（atom）播种 | `kernel/syscall/process_desc.c:290-333` |

### 0.2 术语对照

| 英文 | 中文 | 说明 |
| --- | --- | --- |
| blob | 内嵌 ELF 映像 | 服务 ELF 以二进制对象链接进 `kernel.elf`，用 `SYS_BLOB_GET` 取出（`Makefile:294-305`） |
| port | 端口 | 内核 IPC 端点，`IpcPortCreate()` 创建，`PortRegister()` 挂名 |
| atom | 权限原子 | `kernel/include/kernel/atom.h` 的 `ATOM_*` 枚举，能力（capability）上的语义索引 |
| subject | 主体 | 内核对每个进程签发的不可伪造身份（`subject_id_t`），`IpcRecvFrom` 填充 |
| Powerbox | 权限面板 | perm 服务在授权前向用户弹出确认面板的流程 |
| TUI | 文本用户界面 | term 服务的字符单元（cell）界面 |

### 0.3 总体地图

```text
                    +--------------------------------------------------+
                    |                    Ring 0                        |
                    |  kernel.elf 内嵌 27 个用户态 ELF blob             |
                    |  IPC: 端口表 MAX_PORTS=256，名字表 64 项           |
                    +--------------------------------------------------+
                                        ^  SYS_BLOB_GET / SYS_PROCESS_CREATE
                                        |  SYS_IPC_* / SYS_PROCESS_WAIT
   +-----------------------------------------------------------------------------------------+
   |                                      Ring 3                                              |
   |                                                                                          |
   |  init(PID 1) --spawn--> manager(进程监督者, ATOM_SERVICE_MANAGE)                          |
   |                          |                                                               |
   |                          +-- serial   (串口, IRQ4, 最先起)                                 |
   |                          +-- term     (framebuffer 终端 + "perm.ui" 面板)                  |
   |                          +-- keyboard (PS/2, IRQ1)                                        |
   |                          +-- gui      (像素合成器)  +-- net (PCnet-FIII 驱动)              |
   |                          +-- flaky    (演示: 退出即重启, 直到 FAILED)                       |
   |                          +-- vfs -> fs_mem_driver / fs_virtio_blk_driver                  |
   |                          +-- perm     (Powerbox 唯一事实源)                                |
   |                          +-- device_mgr (PCI 快照)                                        |
   |                          +-- pkg      (.ops 应用容器)                                     |
   |                          +-- user     (账户/登录/退出保护)                                  |
   |                          +-- wm       (桌面注册表)  +-- policy (命令策略表)                 |
   |                          +-- shell    (最后起, 独占串口输入)                                |
   +-----------------------------------------------------------------------------------------+
```

---

## 1. 总体架构

### 1.1 每个服务 = 一个独立进程

OpSys 的服务模型是「一个服务一个进程」，而不是「一个进程多个线程」：
`manager` 通过 `SYS_BLOB_GET` 取回目标服务的 ELF，再用 `SYS_PROCESS_CREATE`
拉起（`user/services/manager/manager.c:402-426` 的 `SpawnService()`）。

```c
/* manager.c:403-425（节选） */
char *blob_buf = malloc(524288); /* 必须容纳最大的服务 ELF
                                  * （term 内嵌 230KB CJK 字体） */
int size = BlobGet(svc->name, blob_buf, 524288);
int pid  = ProcessCreate(svc->name, blob_buf, size);
svc->pid = pid;
```

要点：

- **blob 名 == 进程名**：`BlobGet(svc->name, ...)` 与 `ProcessCreate(svc->name, ...)`
  用的是同一个 `svc->name`，因此 `manager.c` 的服务表名就是内核 blob 表键名，
  也是 `ProcessList()` 里能看到的进程名。
- **缓冲区按调用分配**：多个监控线程（perm/pkg/device_mgr/shell/user）可能同时重启服务，
  共享的 static 缓冲区会竞争，所以 `SpawnService()` 每次 `malloc`，
  内核在系统调用期间完成拷贝后立刻 `free`（`manager.c:394-401`）。
- **init(PID 1) 只拉起 manager**：init 先跑完全部自检，再
  `BlobGet("manager")` + `ProcessCreate("manager")`
  （`user/services/init/main.c:2509-2529`），此后不再直接管服务生命周期。

### 1.2 管理原子（ATOM_SERVICE_MANAGE）与 blob 身份

管理面（management plane）的判定不靠名字，而靠**内容**：`SYS_PROCESS_CREATE`
会把新进程的 ELF 与内核内嵌的若干服务 blob 逐字节比较（`blob_size` + `memcmp`），
命中则向新进程签发 `ATOM_SERVICE_MANAGE`（`kernel/syscall/process_desc.c:290-333`）：

```c
/* process_desc.c:311-317（节选） */
static const char *const s_svc_blobs[] = {
    "manager", "perm", "pkg", "term", "vfs", "fs_mem_driver", "user",
    "policy", "fs_virtio_blk_driver", "gui", "net", "serial",
    "keyboard", "shell", "wm", "device_mgr"};
```

即：**这 16 个 blob 的映像**被拉起时持有 `ATOM_SERVICE_MANAGE`；名字叫 "perm" 的
自造程序不会得到该原子。同时 spawner 自身也必须已持有该原子，否则拒绝播种
（`process_desc.c:305-312`），防止沙盒应用用 `BlobGet("perm")` 复制一份服务映像来提权。

`ATOM_SERVICE_MANAGE` 是下列操作的门槛（全部是内核侧 `CapLookupByAtom` 扫描，零 IPC）：

| 受门槛保护的路径 | 出处 |
| --- | --- |
| `SYS_CAP_GRANT_TO_SUBJECT`（签发原子能力） | `kernel/syscall/syscall.c:382-389` |
| `SYS_CAP_REVOKE_BY_ATOM` | `kernel/syscall/syscall.c:352-355` |
| `SYS_CAP_HAS_ATOM`（窥探他主体能力） | `kernel/syscall/syscall.c:417-425` |
| `SYS_SHM_CREATE` / `SYS_SHM_MAP`（帧缓冲、DMA 池映射） | `kernel/mm/shm.c:113`、`kernel/mm/shm.c:177` |
| `SYS_PCI_*` 管理面查询 | `kernel/syscall/pci.c:259-271` |
| `SYS_KILL` | `kernel/syscall/syscall.c:666-671` |

> shell **也**在播种名单里，所以 shell 持有 `ATOM_SERVICE_MANAGE`；
> 但 shell 的 `kill` 仍走 user 服务的代理路径（见 §7.4），
> 因为 shell 端把敏感操作统一交给账户角色（role）再判一次。

### 1.3 服务与服务之间不共享地址空间

服务之间只能通过端口通信；显示、存储、网络分别由**唯一拥有者**持有：

| 资源 | 唯一拥有者 | 其他服务如何取得 |
| --- | --- | --- |
| 帧缓冲（framebuffer） | `term`（`fb_map` 到 `TERM_FB_VA = 0x400000000`，`user/services/term/term.c:149`、`term.c:2089`）与 `gui`（经 `libgui` 的 `GuiFbOpen()`） | wm / window_demo 只画字符，通过 `term` 的 IPC 渲染（`user/services/wm/main.c:15-30`） |
| 串口 COM1 | `serial`（IRQ4） | 全部日志经 `IpcCall(serial_port, WRITE)` |
| PS/2 键盘鼠标 | `keyboard`（IRQ1 + 0x60-0x64） | `KBD_OP_READ` 等，且受焦点（focus）门控 |
| VFS 命名空间 | `vfs` | 存储驱动用 `VFS_OP_MOUNT` 挂载，客户端用 `libfs` |
| 权限判定 | `perm` | `vfs` 每次 open/bookmark 调 `PERM_OP_CHECK` |
| 账户 | `user` | shell 调 `USER_OP_*` |
| PCI 枚举快照 | `device_mgr` | `DMGR_OP_GET_*` |
| PCnet 网卡 | `net` | `NET_OP_*` |

---

## 2. 服务全清单

### 2.1 核心服务

下表「blob / 进程名」两列总是相同（见 §1.1）；「manager 拉起」一列来自
`manager.c` 的 `main()` 调用序列；「自动重启」一列来自 `s_services[]` 第 4 个字段
`restartable`（`manager.c:132-149`）。

| 服务目录 | blob / 进程名 | 注册端口名字面串 | manager 拉起 | 自动重启 | 职责（一句话） | 源码（行数） |
| --- | --- | --- | --- | --- | --- | --- |
| `init/` | `init`（PID 1） | `init`（`init/main.c:1074`）；测试期另有 `test_svc`（`init/main.c:214`） | 否（内核首个用户进程） | 否（不在 `s_services[]` 中） | 启动自检 + 拉起 manager，随后常驻空闲循环 | `user/services/init/main.c`（2586） |
| `manager/` | `manager` | 无（纯客户端） | 否（由 init 拉起） | 否（监督者自身） | 取 blob、`ProcessCreate` 各服务、`ProcessWait` 监控并按策略重启 | `user/services/manager/manager.c`（671） |
| `serial/` | `serial` | `"serial"`（`serial.c:533`） | 是（第一个） | 否（`restartable = 0`） | 拥有 COM1 + IRQ4 的 ring-3 串口驱动，WRITE/READ/READ_BLOCK | `user/services/serial/serial.c`（565） |
| `term/` | `term` | `"term"`（`term.c:2158`）；`"perm.ui"`（`term.c:1988`） | 是 | 否（`restartable = 0`） | 拥有 framebuffer 的字符终端；同时是 Powerbox UI 代理，负责渲染 `perm.ui` 面板 | `user/services/term/term.c`（2191） |
| `keyboard/` | `keyboard` | `"keyboard"`（`keyboard.c:824`） | 是 | 否（`restartable = 0`） | 拥有 IRQ1 与 PS/2 端口 0x60-0x64；键盘/鼠标读取 + 焦点路由 | `user/services/keyboard/keyboard.c`（847） |
| `vfs/`（server） | `vfs` | `"vfs"`（`vfs_server.c:1983`） | 是 | 否（`restartable = 0`） | VFS 命名空间服务器：卷、句柄、枚举器、书签 | `user/services/vfs/vfs_server.c`（2015） |
| `vfs/`（mem 驱动） | `fs_mem_driver` | `"vfs.fs.mem"`（`fs_mem_driver.c:719`） | 是 | 否 | 内存后端存储驱动，主动向 `vfs` 发起 MOUNT 握手 | `user/services/vfs/fs_mem_driver.c`（767） |
| `vfs/`（blk 驱动） | `fs_virtio_blk_driver` | `"vfs.fs.virtio_blk"`（`fs_virtio_blk_driver.c:1088`） | 是 | 否 | virtio-blk 持久卷 `Disk` 驱动（首次启动格式化后挂载） | `user/services/vfs/fs_virtio_blk_driver.c`（1142） |
| `perm/` | `perm` | `"perm"`（`perm-manager.c:1421`） | 是 | **是** | Powerbox 权限管理器：授权单一事实源 + 角色表 + 规则链 | `user/services/perm/perm-manager.c`（1452） |
| `device_mgr/` | `device_mgr` | `"device_mgr"`（`device_mgr.c:234`） | 是 | **是** | 把内核 PCI 枚举快照暴露给没有 I/O 端口权限的客户端 | `user/services/device_mgr/device_mgr.c`（245） |
| `pkg/` | `pkg` | `"pkg"`（`pkg_manager.c:583`） | 是 | **是** | `.ops` 应用安装/列举/运行/删除，并在 APP_READY 时签发原子能力 | `user/services/pkg/pkg_manager.c`（668） |
| `user/` | `user` | `"user"`（`user/main.c:976`） | 是 | **是** | 账户层：用户名、口令散列、角色、subject 绑定、退出保护代理 | `user/services/user/main.c`（1001） |
| `wm/` | `wm` | `"wm"`（`wm/main.c:376`） | 是 | **是** | 桌面窗口注册表 + 经 term 合成的窗口绘制 + 键盘焦点路由 | `user/services/wm/main.c`（569） |
| `policy/` | `policy` | `"policy"`（`policy/main.c:264`） | 是 | **是** | 三层命令访问架构的中间层：角色 → 命令名 允许/拒绝表 | `user/services/policy/main.c`（322） |
| `gui/` | `gui` | `"gui"`（`gui/main.c:1624`） | 是 | **是** | 像素合成器：每窗口离屏缓冲、z 序、键盘/鼠标事件环 | `user/services/gui/main.c`（1643） |
| `net/` | `net` | `"net"`（`net/main.c:770`） | 是 | 否（`restartable = 0`） | PCnet-Fast III 网卡驱动 + ARP/IP/ICMP/UDP/TCP 协议栈 | `user/services/net/main.c`（779）、`net/proto.c`（729） |
| `shell/` | `shell` | `"shell"`（主端口）；`"shell_test"`（`shell.c:2726`，测试用） | 是（最后一个） | **是** | 交互式命令行：内置 40+ 命令，全部通过 IPC 访问服务 | `user/services/shell/shell.c`（4260） |

### 2.2 演示与自测 blob（不由 manager 启动）

这些目录同样产出内嵌 blob，但**不在** `s_services[]` 中，因此既不受 manager 监督，
也不会自动重启；它们由 shell 的 `exec` / `pkg` / 专用命令拉起。

| 服务目录 | blob / 进程名 | 注册端口 | 由谁拉起 | 职责 | 源码（行数） |
| --- | --- | --- | --- | --- | --- |
| `flaky/` | `flaky` | 无 | manager（`SVC_FLAKY`，唯一在启动阶段被监控到 FAILED 的演示服务） | `Sleep(5)` 后 `return 7` 模拟崩溃，验证重启策略 | `flaky/main.c`（35） |
| `crashpeer/` | `crashpeer` | `"crashpeer"`（`crashpeer/main.c:37`） | init 自检（`init/main.c:639-666`） | 收一次调用后**不回复**直接退出，验证内核唤醒调用方 `ERR_NOENT` | `crashpeer/main.c`（51） |
| `canarytest/` | `canarytest` | 无 | init 自检（`init/main.c:698-702`） | 故意溢出栈缓冲，验证 Ring 3 `__stack_chk_fail` | `canarytest/main.c`（48） |
| `hello/` | `hello` | 无 | shell `exec`（无参数时的默认名） | 最小用户程序：打印 PID、信号自测 | `hello/main.c`（121） |
| `sbox_demo/` | 安装时由 app_id 决定 | 无 | pkg（`PKG_OP_RUN`） | 通过 `PkgReady()` 走沙盒能力签发，再测 `OsSetTime()` 门控 | `sbox_demo/main.c`（71） |
| `runtime_demo/` | `runtime_demo` | 无 | shell `exec` | 运行时特性演示：构造函数、`malloc`、`atexit`、`errno`、信号 | `runtime_demo/main.c`（232） |
| `tui_demo/` | `tui_demo` | 无 | shell `exec` | 通过 `libtui` 演示状态栏/边框/光标 | `tui_demo/main.c`（88） |
| `window_demo/` | `window_demo` | 无 | shell `exec` | v0.4 字符窗最小闭环：直接调 `term` IPC + 键盘焦点 | `window_demo/main.c`（135） |
| `wm_demo/` | `wm_demo` | 无 | shell `exec` | 经 `libwm` 创建 3 个窗口并进入桌面会话 | `wm_demo/main.c`（97） |
| `gui_demo/` | `gui_demo` | 无 | shell `gui` 命令（`shell.c:2457-2464`） | 激活合成器、开 3 个窗口、跑事件回环 | `gui_demo/main.c`（244） |

### 2.3 关于「blob 名」的严格说明

- blob 键名由 `Makefile:295-305` 的 `SVC_BLOB_RULE` 生成：目标文件名去掉目录与
  `.elf` 后缀（`build/user/services/<name>.elf` 到 `<name>_blob.o`），
  符号被 `objcopy --redefine-sym` 重命名为 `<name>_elf_start/_end/_size`。
- 因此 `fs_mem_driver`、`fs_virtio_blk_driver`、`device_mgr` 这类带下划线的名字
  **就是** blob 键名与进程名，而服务目录名与它们并不一致（前两者在 `user/services/vfs/` 下，
  后者在 `user/services/device_mgr/` 下）。
- `init` 与 `hello` 的 blob 被链接进 `kernel.elf`（`Makefile:234`），
  其余服务的 blob 同样全部内嵌（`SVC_NAMES`，`Makefile:206`）。

---

## 3. IPC 传输层与 4096 字节约束

### 3.1 内核提供的原语

| 原语 | 语义 | 出处 |
| --- | --- | --- |
| `IpcPortCreate()` | 创建端口（返回 port id） | `kernel/include/kernel/ipc.h:63` |
| `IpcRegisterPort(name, port)` / 用户态 `PortRegister` | 把端口挂到名字表 | `kernel/include/kernel/ipc.h:151`、`user/lib/libos/syscalls.h:161` |
| `IpcGetPort(name)` / 用户态 `PortGet` | 按名字解析端口，未注册返回 `ERR_NOENT` | `kernel/include/kernel/ipc.h:143` |
| `IpcSend(port, msg, len)` | 单向发送（不等待回复） | `kernel/include/kernel/ipc.h:86` |
| `IpcRecv(port, buf, &len, &tok)` | 阻塞接收；`tok` 非 0 表示这是一次 call | `kernel/include/kernel/ipc.h:97` |
| `IpcRecvFrom(port, buf, &len, &tok, &subject)` | 同上，**额外返回内核填充的发送方 subject** | `kernel/include/kernel/ipc.h:112` |
| `IpcCall(port, req, req_len, resp, &resp_len)` | 同步请求/响应 | `kernel/include/kernel/ipc.h:123` |
| `IpcReply(token, msg, len)` | 按 token 回复某一个挂起调用者 | `kernel/include/kernel/ipc.h:136` |

### 3.2 并发调用者与回复令牌（reply token）

端口不再有「同时只能有一个活跃调用」的限制：每个调用在端口上挂一个
`ipc_pending_msg_t`，接收方拿到的是**不透明回复令牌**（池槽位 + 世代计数），
因此多个客户端可以并发阻塞在同一个端口上（`kernel/ipc/ipc.c:21-31`）。

```text
   caller A --IpcCall--> [ 端口挂起/回复等待表 ] --token tA--> 服务端
   caller B --IpcCall--> [ 端口挂起/回复等待表 ] --token tB--> 服务端
   服务端   --IpcReply(tA)--> 只唤醒 A（世代不匹配的陈旧令牌被拒绝）
```

这条性质被 manager 明确依赖：它的监控线程可以在 shell 正在读串口时调用 `serial`
（`manager.c:51-56`）。

### 3.3 发送方身份由内核填充

`IpcSenderSubject()` 取当前线程所属进程的 `subject_id`，在消息入队／投递时写进
消息记录（`kernel/ipc/ipc.c:210-216`、`kernel/ipc/ipc.c:409`、`kernel/ipc/ipc.c:424`、`kernel/ipc/ipc.c:526`），
`IpcRecvFrom()` 再把它交给接收方（`kernel/ipc/ipc.c:460-462`、`kernel/ipc/ipc.c:495-496`）：

```c
/* ipc.c:213-216 */
static subject_id_t IpcSenderSubject(void) {
    process_t *proc = process_current();
    return proc ? proc->subject_id : 0;
}
```

**身份不可伪造**：请求体里的任何 `u64 subject_id` 字段都只是数据，内核不读取它；
`IpcRecv` 等价于 `IpcRecvFrom(..., NULL)`（`kernel/ipc/ipc.c:502-505`）。

### 3.4 4096 字节上限

内核消息缓冲区是固定大小的（`kernel/include/kernel/types.h:125`）：

```c
#define MAX_MSG_SIZE 4096
```

因此**请求和响应都必须塞进 4096 字节**。各协议的处理方式：

| 协议 | 请求上限 | 响应上限 | 处理手法 |
| --- | --- | --- | --- |
| VFS 客户端 | `vfs_req_write_t` | `vfs_resp_read_t` | 读写载荷统一上限 `VFS_MAX_READ = VFS_MAX_WRITE = 4032`（`vfs.h:131-137`），并有 `_Static_assert` 编译期断言（`vfs.h:555-560`） |
| VFS 驱动 | `DRV_REQ_MAX`（48 字节头 + 4032 字节联合体） | `DRV_RESP_MAX`（4 + 4032） | `DRV_MAX_PAYLOAD = 4032`（`vfs.h:476-484`） |
| perm | 最大者 `perm_req_check_t`（含 `char url[1024]`） | 最大者 `perm_resp_policy_t`（16 + 3840） | `PERM_POLICY_MAX = 3840`（`perm.h:352`）；收发缓冲直接取 `VFS_IPC_MAX`（`perm-manager.c:98-99`） |
| pkg | `pkg_req_install_t` = 4+64+256 = 324 | `pkg_resp_list_t` = 8+8×64 = 520 | 字段上限 `PKG_*_MAX`（`pkg.h:60-65`） |
| user | `user_req_passwd_t` = 4+64+64+32 = 164 | `user_resp_policy_t` = 4+4+64×48 = 3080 | `USER_IPC_MAX 4096`（`user.h:156`） |
| policy | `policy_req_query_t` = 4+4+4+64×32 = 2060 | `policy_resp_dump_t` = 8+64×48 = 3080 | `POLICY_IPC_MAX 4096`（`policy.h:102`） |
| wm | `wm_req_t` = 112 | `wm_resp_t` = 916 | 固定结构，天然远低于上限（`wm_proto.h:48-65`） |
| gui | `gui_req_t` = **4096**（8 字节头 + `data[4088]`） | `gui_resp_t` = **4096**（4 + `data[4092]`） | 恰好占满；`_Static_assert(sizeof(...) <= GUI_IPC_MAX)`（`gui.h:147-148`） |
| net | `net_req_t` = 1540 | `net_resp_t` = 1540 | `data[NET_MTU + 16]`，`NET_MTU = 1514`（`net.h:41`、`net.h:67-80`） |
| device_mgr | `dmgr_req_t` = 8 | `dmgr_resp_t` = 48 | 单个 `pci_device_info_t`（44 字节 + 对齐，`kernel/include/kernel/pci.h:36-47`） |
| serial | `{ u32 op; u32 len; u8 data[len]; }` | `{ i32 ret; u8 data[64]; }` | manager 侧每块 `SERIAL_CHUNK = 32`（`manager.c:116`、`manager.c:197-215`） |
| term | `TERM_MAX_DATA = 256`（`term.c:132`） | 区域快照 = 头 + 2×`TERM_MAX_REGION_CELLS` | `TERM_MAX_REGION_CELLS = 2036`（`term.c:140`） |
| keyboard | `KBD_MAX_DATA = 256`（`keyboard.c:133`） | `{ i32 ret; u8 data[len]; }` | 请求头 8 字节（`KBD_REQ_HDR`，`keyboard.c:177`） |

> 实践规则：**任何要放进 IPC 的载荷都必须按 4032 字节（或更小）分块**，
> 因为 4096 还要扣掉协议头。VFS 的读/写/枚举三处都做了分块；
> `gui` 通过「一条报文只装一个窗口操作」来回避分块。

### 3.5 端口资源上限

| 限制 | 值 | 出处 |
| --- | --- | --- |
| 端口总数 | `MAX_PORTS = 256` | `kernel/include/kernel/types.h:123` |
| 名字注册表槽位 | `PORT_REGISTRY_SIZE = 64` | `kernel/ipc/ipc.c:88` |
| 单个名字长度 | 64 字节（含 NUL） | `kernel/ipc/ipc.c:127`、`kernel/ipc/ipc.c:705-708` |
| 重名注册 | 返回 `ERR_BUSY` | `kernel/ipc/ipc.c:690-695` |
| 挂起消息池 | `PENDING_POOL_SIZE = 128` | `kernel/ipc/ipc.c:87` |

进程退出时 `IpcCleanupProcess(pid)` 会销毁其端口、以 `ERR_NOENT` 唤醒所有等待者，
并清掉它的注册名（`kernel/include/kernel/ipc.h:71-77`）——这是 manager 能干净重启
服务的前提（`manager.c:665-669`）。

---

## 4. 启动拓扑

### 4.1 依赖顺序图

下图严格对应 `user/services/manager/manager.c:491-671` 的 `main()` 调用序列。

```text
内核
 └─ init (PID 1, blob "init")
     │  自检 RunTests() / InitProtocol()        init/main.c:2506-2507
     │
     └─ manager (blob "manager")                init/main.c:2516-2529
         │
         │  ── 阶段 1：日志通道 ─────────────────────────────────────────
         ├─ serial                      manager.c:498   (SVC_SERIAL = 0)
         │   └─ 轮询 PortGet("serial") 最多 2000 tick   manager.c:504-508
         ├─ 串口自测 SerialTestRun()    manager.c:518 / 292-382
         │
         │  ── 阶段 2：显示与网络 ───────────────────────────────────────
         ├─ term                        manager.c:526   (SVC_TERM = 1)
         ├─ keyboard                    manager.c:529   (SVC_KEYBOARD = 2)
         ├─ gui                         manager.c:538   (SVC_GUI = 14)
         ├─ net                         manager.c:540   (SVC_NET = 15)
         │
         │  ── 阶段 3：重启策略演示 ─────────────────────────────────────
         ├─ flaky                       manager.c:548   (SVC_FLAKY = 3)
         ├─ ServiceMonitor(flaky) → 重启 3 次 → FAILED  manager.c:555 / 440-465
         │
         │  ── 阶段 4：权限与存储（顺序有强依赖）────────────────────────
         ├─ vfs                         manager.c:568   (SVC_VFS = 4)
         │   └─ 等 PortGet("vfs")                       manager.c:571-572
         ├─ fs_mem_driver               manager.c:573   (SVC_FS_MEM = 5)
         │   └─ 等 PortGet("vfs.fs.mem") + Sleep(20)    manager.c:576-578
         ├─ perm                        manager.c:591   (SVC_PERM = 7)
         │   └─ 等 PortGet("perm")                      manager.c:594-595
         ├─ fs_virtio_blk_driver        manager.c:607   (SVC_FS_VIRTIO_BLK = 6)
         │   └─ 等 PortGet("vfs.fs.virtio_blk") + Sleep(20)   manager.c:610-612
         ├─ device_mgr                  manager.c:621   (SVC_DEVICE_MGR = 8)
         ├─ pkg                         manager.c:633   (SVC_PKG = 9)
         │
         │  ── 阶段 5：账户、桌面、策略 ─────────────────────────────────
         ├─ user                        manager.c:643   (SVC_USER = 11)
         ├─ wm                          manager.c:651   (SVC_WM = 12)
         ├─ policy                      manager.c:658   (SVC_POLICY = 13)
         │
         │  ── 阶段 6：shell（最后）─────────────────────────────────────
         └─ shell                       manager.c:663   (SVC_SHELL = 10)
             └─ StartServiceMonitors()  manager.c:670
                ├─ 监控线程: perm / pkg / device_mgr / shell / user
                └─ 主线程进入 ThreadYield 空闲循环
```

> 注意：`s_services[]`（`manager.c:132-149`）的**表内顺序**与上面的**实际拉起顺序**
> 并不相同——`SVC_*` 常量（`manager.c:158-173`）只是索引，`main()` 按自己的依赖分析
> 顺序调用 `SpawnService(&s_services[SVC_X], ...)`，其中 term/keyboard/gui/net/user/wm/policy
> 的调用点甚至不落在表顺序上。读代码时以 `main()` 为准。

### 4.2 为什么 serial 最先

三个互相独立的原因，全部来自 `manager.c` 的注释与代码：

1. **manager 自己的日志通道就是 serial**。`ManagerWrite()` / `ManagerPrintf()`
   全部经 `IpcCall(s_serial_port, SERIAL_OP_WRITE)` 输出（`manager.c:66-69`、`manager.c:182-215`），
   端口未解析时 `ManagerPutc()` 静默丢弃。所以必须先拉起 serial 并 `PortGet` 成功，
   否则后续所有启动日志都会消失。
2. **端口解析失败只能退到内核 DebugLog**。`s_serial_port` 轮询 2000 次仍为负时，
   manager 调 `DebugLog("manager: serial port never resolved
")` 后死循环
   （`manager.c:509-513`）——这是「日志订阅者不在，就没有可读输出」的兜底。
3. **串口自测必须独占 RX**。`SerialTestRun()` 要求 host 注入的 `READ_PATH_OK`
   被串口服务自己的 IRQ 线程排空并回环返回（`manager.c:279-291`），
   而此时 shell 尚未启动（shell 会持续轮询同一端口），所以自测被安排在 shell 之前，
   且 term/keyboard 也在自测**之后**才拉起（`manager.c:520-524`）。

### 4.3 为什么 shell 最后

单一写入者（single-writer）规则：

- shell 一旦启动就持续轮询 `serial` 读输入并独占提示符，manager 从此**不再**向串口
  写入（`manager.c:47-49`、`manager.c:637-641`）。
- 因此 `SpawnService(&s_services[SVC_SHELL], 1)` 使用 `quiet = 1`
  **故意不打印** `shell started (PID=..)`：若在 `ProcessCreate()` 之后打印，
  新 shell 可能在打印中途就被调度，造成启动日志交错（`manager.c:637-641`）。
- shell 启动前，manager 必须确保它依赖的端口都已就绪：`vfs`、`vfs.fs.mem`、
  `perm`、`vfs.fs.virtio_blk`（各自带 2000 tick 轮询 + `Sleep(20)`
  让挂载握手完成，`manager.c:567-612`），以及 `user`、`wm`、`policy`
  （shell 启动时就要查询策略表，`manager.c:654-660`）。

### 4.4 两个刻意的顺序选择

| 选择 | 原因（源码注释） |
| --- | --- |
| `perm` 在 `fs_virtio_blk_driver` **之前** | 块设备驱动在无 virtio-blk 设备时会「降级且不注册端口」，manager 的 2000 tick 等待会跑满，从而拖住 init 侧的 P1 测试（其书签预算同为 2000 tick），造成 P1 结果被清零后级联影响后续 GRANT/P2V 测试（`manager.c:580-589`） |
| `pkg` 在 `vfs` 与 `perm` **之后** | pkg 写 `/Volumes/Users/Apps`（`pkg_manager.c:68`），会触发 Powerbox 首次授权；两个依赖必须先在线（`manager.c:625-631`） |

---

## 5. 重启策略

### 5.1 策略参数

| 参数 | 值 | 出处 |
| --- | --- | --- |
| 每个可重启服务的重启上限 | `MAX_RESTARTS = 3` | `manager.c:119` |
| 退出检测 | `ProcessWait(pid, &exit_code)` 阻塞等待最后一个线程退出 | `manager.c:445` |
| 超限动作 | 打印 `manager: <svc> marked FAILED` 并**结束监控线程**（不再重启） | `manager.c:452-455` |
| 重启计数 | `svc->restart_count`（每服务独立，`service_t` 第 3 字段） | `manager.c:125-130`、`manager.c:456` |
| 等待失败 | `ProcessWait` 返回负值时按「已退出」处理并进入重启流程（进程可能已被当作孤儿回收） | `manager.c:446-451` |

### 5.2 监控名单

`StartServiceMonitors()` 只给 5 个服务各起一个监控线程（`manager.c:473`）：

```c
static const int s_restartable[] = {SVC_PERM, SVC_PKG, SVC_DEVICE_MGR, SVC_SHELL, SVC_USER};
```

注意 `service_t.restartable` 字段（`manager.c:132-149`，值为 1 的还包括
`flaky`、`wm`、`policy`、`gui`）与「实际被起监控线程的名单」
**并不完全一致**：

- `flaky` 的 `restartable = 1`，但它的监控由 manager 主线程在启动阶段直接跑完
  （`ServiceMonitor(&s_services[SVC_FLAKY])`，`manager.c:555`），跑到 FAILED 为止；
- `wm` / `policy` / `gui` 的 `restartable = 1`，但**未**列入
  `s_restartable[]`，因此当前迭代它们没有监控线程——这是代码事实，不是设计意图的推断。

### 5.3 不重启的服务与原因

| 服务 | `restartable` | 原因（源码注释：`manager.c:58-64`、`manager.c:133-139`、`manager.c:151-156`） |
| --- | --- | --- |
| `serial` | 0 | 绑定了 IRQ4 到自己的 IRQ 线程，重启需重新绑定 PIC 线并重新初始化 16550；本轮范围外 |
| `term` | 0 | 拥有 framebuffer（`term.c:2089` 的 `fb_map`） |
| `keyboard` | 0 | 拥有 IRQ1 与 PS/2 端口 0x60-0x64 |
| `vfs` | 0 | 拥有命名空间状态（卷、句柄、枚举器、书签）；重启需要 `-ESTALE` 重开语义（路线图 §七.3） |
| `fs_mem_driver` | 0 | 同上：拥有挂载状态 |
| `fs_virtio_blk_driver` | 0 | 同上：拥有卷与（可能）未落盘的数据 |
| `net` | 0 | 拥有自己的 PCI 设备与 10 页 DMA 池 |

注释同时给出了一条重要澄清（`manager.c:151-156`）：**重启在技术上是可行的**
（`ProcessReap()` 会清理死进程的端口与 IRQ），但会打断硬件/命名空间状态，
因此本迭代选择「不重启」而不是「不能重启」。挂起/超时检测同样是未来工作——
当前只有基于 `ProcessWait` 的**退出检测**（`manager.c:58-60`）。

### 5.4 重启后的端口回归

服务重启后必须重新注册同名端口，否则客户端 `PortGet` 会一直失败。
`IpcCleanupProcess()` 在进程回收时清掉旧注册名（`kernel/include/kernel/ipc.h:71-77`），
新的 `PortRegister` 才能成功。init 的 P3 崩溃恢复测试正是断言这条链：
kill pkg 后 manager 的监控线程唤醒，重新 `ProcessCreate`，`PortGet("pkg")` 再次成功
（`user/services/init/main.c:2258-2272`）。

---

## 6. 各服务 IPC 协议

通用约定（所有协议一致，见 `vfs.h:24-27`、`perm.h:43-45`、`pkg.h:42-44`）：

- **扁平结构体直接按本机小端原样拷贝**，不做序列化。
- **`req[0]` 是 opcode**（`u32`）：接收方先读 4 字节判断操作，再按 `msg_len` 决定能否按
  完整结构体解释（各服务在 `msg_len < sizeof(...)` 时回 `ERR_INVAL`）。
- **长度由接收方校验**：所有字段访问都受 `IpcRecv` 实际报告的长度约束
  （如 `device_mgr.c:132-158`、`net/main.c:497-501` 对 `req->len` 与 `msg_len - 8` 的双重检查）。
- **身份从 `IpcRecvFrom` 取**：结构体里的 `subject_id` 字段只允许由可信代理填写（见 §9）。

### 6.1 serial — 端口 `"serial"`

不依赖头文件，opcode 定义在服务与客户端各自的源码里（`serial.c:127-129`、
`manager.c:114-116`）：

| opcode | 名称 | 请求布局 | 响应布局 |
| --- | --- | --- | --- |
| 1 | `SERIAL_OP_WRITE` | `{ u32 op; u32 len; u8 data[len]; }` | `{ i32 ret; }` 或 `{ i32 ret; u8 data[64]; }` |
| 2 | `SERIAL_OP_READ` | `{ u32 op; u32 len; }` | `{ i32 ret; u8 data[64]; }`（非阻塞） |
| 3 | `SERIAL_OP_READ_BLOCK` | `{ u32 op; u32 len; }` | `{ i32 ret; u8 data[64]; }`（阻塞） |

manager 的发送端把 `len` 限制在 `SERIAL_CHUNK = 32` 字节（`manager.c:116`、
`manager.c:197-215`），串口自测的响应缓冲固定 17 个 `u32`（`manager.c:308`）。

### 6.2 term — 端口 `"term"` 与 `"perm.ui"`

opcode 定义在服务端（`term.c:116-130`），`libtui` 侧有一份必须保持一致的镜像
（`user/lib/libtui/tui.h:39-49`）：

| opcode | 名称 | 说明 | payload 上限 |
| --- | --- | --- | --- |
| 1 | `TERM_OP_WRITE` | 在光标处渲染文本 | `TERM_MAX_DATA = 256` |
| 2 | `TERM_OP_CLEAR` | 清屏并复位光标 | — |
| 3 | `TERM_OP_STATUS` | 渲染状态栏 | — |
| 4 | `TERM_OP_BOX` | 画边框 + 标题 | — |
| 5 | `TERM_OP_RENDER_LINE` | 在 `(x, y)` 渲染一行，不动光标 | — |
| 6 / 7 | `TERM_OP_SET_CURSOR` / `TERM_OP_GET_CURSOR` | 光标位置 | — |
| 8 / 9 | `TERM_OP_SNAPSHOT` / `TERM_OP_RESTORE` | 保存/恢复单元区域，`{x,y,w,h}` + `u16 cells[]` | `TERM_MAX_REGION_CELLS = 2036` |
| 10 | `TERM_OP_SCROLLVIEW` | `{i32 delta}` 翻看回滚缓冲 | — |
| 11 | `TERM_OP_REDRAW` | 从 `s_cells` 全屏重绘（`gui` 退出时恢复文本屏） | — |
| 12 | `TERM_OP_GET_SIZE` | 查询 `{cols, rows}` | — |

`perm.ui` 端口只接受 `PERM_OP_UI_SHOW`（`term.c:2012`），请求体是
`perm_req_ui_t`，响应体是 `{ i32 ret; }`（`perm_resp_ui_t`）。
该端口的服务线程**永远停在 `IpcRecv`**，从不阻塞在用户身上，
这样 perm 的 `UI_SHOW` 推送才能同步完成（`term.c:1975-1979`）。

### 6.3 keyboard — 端口 `"keyboard"`

| opcode | 名称 | 语义 | 出处 |
| --- | --- | --- | --- |
| 1 | `KBD_OP_READ` | 非阻塞读键 | `keyboard.c:128` |
| 2 | `KBD_OP_READ_BLOCK` | 阻塞读键（带 park 表） | `keyboard.c:129`、`KBD_PARK_MAX = 4`（`keyboard.c:135`） |
| 3 | `KBD_OP_TAKE_FOCUS` | 取得键盘焦点（只有焦点持有者能读键） | `keyboard.c:130` |
| 4 | `KBD_OP_RELEASE_FOCUS` | 释放焦点 | `keyboard.c:131` |
| 5 | `KBD_OP_MOUSE_READ` | 返回 `{ i32 dx; i32 dy; i32 buttons }` | `keyboard.c:132` |

请求头为 8 字节（`{ u32 op; u32 len; }`，`KBD_REQ_HDR`，`keyboard.c:177`），
载荷上限 `KBD_MAX_DATA = 256`（`keyboard.c:133`）。
焦点门控用调用方 subject 判定（`keyboard.c:628` 的 `IpcRecvFrom`）。

### 6.4 vfs — 端口 `"vfs"`

opcode 见 `vfs.h`，共 20 个（v0.9 新增 `VFS_OP_SYNC`）：

| opcode | 名称 | 请求类型 | 响应类型 |
| --- | --- | --- | --- |
| 1 | `VFS_OP_GET_ITEM` | `vfs_req_get_item_t` | `vfs_resp_get_item_t` |
| 2 | `VFS_OP_CREATE_DIR` | `vfs_req_create_dir_t` | `vfs_resp_create_dir_t` |
| 3 | `VFS_OP_DELETE_ITEM` | `vfs_req_delete_t` | `vfs_resp_delete_t` |
| 4 | `VFS_OP_OPEN_ITEM` | `vfs_req_open_t` | `vfs_resp_open_t` |
| 5 | `VFS_OP_READ` | `vfs_req_read_t` | `vfs_resp_read_t` |
| 6 | `VFS_OP_WRITE` | `vfs_req_write_t` | `vfs_resp_write_t` |
| 7 | `VFS_OP_CLOSE` | `vfs_req_close_t` | `vfs_resp_close_t` |
| 8 / 9 | `VFS_OP_ENUM_BEGIN` / `VFS_OP_ENUM_NEXT` | `vfs_req_enum_begin_t` / `vfs_req_enum_next_t` | `vfs_resp_enum_begin_t` / `vfs_resp_enum_next_t` |
| 10 / 11 / 12 | `VFS_OP_CREATE_BOOKMARK` / `RESOLVE_BOOKMARK` / `REVOKE_BOOKMARK` | `vfs_req_*_bookmark_t` | `vfs_resp_*_bookmark_t` |
| 13 / 14 | `VFS_OP_MOUNT` / `VFS_OP_UNMOUNT`（驱动 → 服务） | `vfs_req_mount_t` / `vfs_req_unmount_t` | `vfs_resp_mount_t` / `vfs_resp_unmount_t` |
| 15 | `VFS_OP_STAT_VOLUME` | `vfs_req_stat_volume_t` | `vfs_resp_stat_volume_t` |
| 16 | `VFS_OP_MOVE` | `vfs_req_move_t` | `vfs_resp_move_t` |
| 17 | `VFS_OP_WHOAMI` | `vfs_req_whoami_t` | `vfs_resp_whoami_t` |
| 18 | `VFS_OP_LIST_VOLUMES` | `vfs_req_list_volumes_t` | `vfs_resp_list_volumes_t` |
| 19 | `VFS_OP_READ_MAP` | `vfs_req_read_map_t` | `vfs_resp_read_map_t` |
| 20 | `VFS_OP_SYNC` | `vfs_req_sync_t` | `vfs_resp_sync_t` |


> **`VFS_OP_SYNC`（v0.9）** —— 刷盘。vfs_server 遍历自己的挂载卷表，向每个卷的驱动端口发 `DRV_OP_SYNC`：成功应答计入 `volumes`，驱动返回 `ERR_INVAL`/`ERR_NOCAP`（没实现）也算"已应答但不支持"，只有其它负值或传输错误才计入 `failures` 并把第一个硬错误写回 `ret`。请求无需任何能力 —— 刷盘只能把**已被接受的写**推向介质，不能修改数据，因此 `sync`、`disk sync`、`power off` 都可以直接调用（`libfs` 的 `FsSync()` 即这一条）。

关键结构体布局：

```c
/* vfs.h:60-63 —— 全局资源定位符（128 位卷 UUID + 卷内稳定 itemID） */
typedef struct { vfs_uuid_t vol; vfs_item_id_t id; } vfs_resource_t;   /* 24 字节 */

/* vfs.h:219-224 —— OPEN：路径只在边界出现一次 */
typedef struct { u32 op; char path[1024]; u32 flags; u32 access; } vfs_req_open_t;  /* 1036 字节 */

/* vfs.h:261-268 —— WRITE：载荷 4032 上限 */
typedef struct { u32 op; vfs_handle_t handle; u64 offset; u32 len; u8 data[4032]; } vfs_req_write_t;

/* vfs.h:328-341 —— 书签 blob（不透明能力载体） */
typedef struct {
    u32 magic; u32 version; u32 payload_len;
    vfs_resource_t resource; vfs_item_id_t parent_id;
    u32 access; u64 subject_id; u64 created_ticks; u64 expiry_ticks;
    u8 mac[16];              /* Phase 2 恒为 0 */
} vfs_bookmark_t;            /* _Static_assert <= VFS_BOOKMARK_MAX(256)，vfs.h:343 */
```

VFS 专属错误码（不与内核 `ERR_*` 冲突，`vfs.h:147-152`）：

| 值 | 名称 | 含义 |
| --- | --- | --- |
| -100 | `VFS_ERR_READONLY` | 只读卷（EROFS） |
| -101 | `VFS_ERR_NOSPC` | 卷满（ENOSPC） |
| -102 | `VFS_ERR_STALE` | 句柄/枚举器失效（ESTALE） |
| -103 | `VFS_ERR_PERM` | 权限拒绝（EPERM） |
| -104 | `VFS_ERR_EXISTS` | 目录已存在（EEXIST） |
| -105 | `VFS_ERR_ACCESS` | 书签未授权（EACCES）——Powerbox 未批准时的返回值 |

驱动侧协议（`vfs.h:468-510`）用「48 字节头 + 4032 字节联合体」压缩所有 DRV 操作：

```c
/* vfs.h:512-525（节选） */
typedef struct {
    u32 op; u32 volume;
    vfs_item_id_t item_id, parent_id;
    u32 from; u32 recursive; u32 len; u64 offset;
    union { char name[256]; u8 data[4032]; } payload;
} drv_req_t;
```

DRV opcode 1-11 是数据面（GETATTR / LOOKUP / READ / WRITE / CREATE_DIR / MKFILE / DELETE /
ENUM / STAT / MOVE / PHYS_RANGE），12-15 是**管理控制面**
（`CTRL_MOUNT` / `CTRL_UNMOUNT` / `CTRL_FORMAT` / `CTRL_FILL`），
后者在驱动侧由 `ATOM_SERVICE_MANAGE` 门控，是唯一允许在卷未挂载时运行的 DRV 操作
（`vfs.h:501-509`）。

### 6.5 perm — 端口 `"perm"`（并向 `"perm.ui"` 推送）

opcode 见 `perm.h:85-100`，共 14 个：

| opcode | 名称 | 方向 | 请求 | 响应 |
| --- | --- | --- | --- | --- |
| 1 | `PERM_OP_QUERY` | UI/管理 → perm | `perm_req_query_t` `{u32 op; u32 query_id;}` | `perm_resp_query_t` |
| 2 | `PERM_OP_ANSWER` | UI/管理 → perm | `perm_req_answer_t` `{u32 op; u32 query_id; i32 allow;}` | `perm_resp_answer_t` |
| 3 | `PERM_OP_CHECK` | vfs → perm | `perm_req_check_t` | `perm_resp_check_t` |
| 4 | `PERM_OP_REVOKE` | shell → perm | `perm_req_revoke_t` | `perm_resp_revoke_t` |
| 5 | `PERM_OP_GRANT` | 测试/管理 → perm | `perm_req_grant_t` | `perm_resp_grant_t` |
| 6 | `PERM_OP_UI_SHOW` | perm → term(`perm.ui`) | `perm_req_ui_t` | `perm_resp_ui_t` |
| 7 | `PERM_OP_ROLE_SET` | 管理 → perm | `perm_req_role_set_t` | `perm_resp_role_set_t` |
| 8 | `PERM_OP_DUMP` | 管理 → perm | `perm_req_dump_t` | `perm_resp_dump_t` |
| 9 | `PERM_OP_CONTEXT` | 预留（P3） | `perm_req_context_t` | `perm_resp_context_t` |
| 10 | `PERM_OP_FREQ` | 预留（P3） | `perm_req_freq_t` | `perm_resp_freq_t` |
| 11 / 12 | `PERM_OP_POLICY_SAVE` / `PERM_OP_POLICY_LOAD` | 预留（P4） | `perm_req_policy_t` | `perm_resp_policy_t` |
| 13 | `PERM_OP_AUDIT` | 预留（P3） | `perm_req_audit_t` | `perm_resp_audit_t` |
| 14 | `PERM_OP_SET_QUIET` | 管理 | `perm_req_set_quiet_t` | `perm_resp_set_quiet_t` |

```c
/* perm.h:128-149 —— CHECK 请求/响应 */
typedef struct {
    u32 op;                    /* = PERM_OP_CHECK */
    vfs_resource_t resource;   /* 24 字节：卷 UUID + itemID */
    u32 access;                /* VFS_ACCESS_* 位 */
    char url[1024];            /* 仅供 UI 展示 */
    u64 subject_id;            /* 由 vfs_server 用 ipc_recv_from 填入 */
    u64 scope_hash;            /* P2 预留，当前 do_check 忽略 */
} perm_req_check_t;

typedef struct {
    i32 ret;          /* 0 = 已授权；VFS_ERR_ACCESS = 拒绝 */
    u32 query_id;     /* ret < 0 时有效 */
    u32 granted;      /* 实际批准的 VFS_ACCESS_* 位掩码（能力化抹位） */
} perm_resp_check_t;

/* perm.h:181-191 —— QUERY 响应（带不可伪造的发起者身份） */
typedef struct {
    i32  ret;
    u32  query_id;
    u32  pid;            /* 内核解析的展示元数据 */
    char name[64];
    char url[1024];
    u32  access;
    u64  subject_id;     /* 内核填充，不可伪造 */
    i32  state;          /* PERM_QUERY_PENDING/ALLOWED/DENIED */
    char label[128];     /* perm 聚合的人类可读描述 */
} perm_resp_query_t;
```

角色与状态常量：

| 常量 | 值 | 出处 |
| --- | --- | --- |
| `PERM_QUERY_PENDING` / `ALLOWED` / `DENIED` | 0 / 1 / 2 | `perm.h:63-65` |
| `PERM_ROLE_OWNER` 到 `PERM_ROLE_AUDITOR` | 0 到 5 | `perm.h:69-78` |
| `PERM_ROLE_DEFAULT` | `PERM_ROLE_STANDARD`(2) | `perm.h:76` |
| `PERM_VERDICT_DENY` / `PERM_VERDICT_ALLOW` | 0 / 1 | `perm.h:81-82` |
| `PERM_MAX_GRANTS` / `QUERIES` / `ROLES` / `RULES` | 64 / 16 / 64 / 96 | `perm-manager.c:82-85` |
| `PERM_BOOTSTRAP_SUBJECT` / `PERM_BOOTSTRAP_ROLE` | 1 / `PERM_ROLE_OWNER` | `perm-manager.c:94-95` |

能力化抹位（capability masking）：授权命中时不返回请求掩码，而是**只返回被覆盖的位**
（`perm-manager.c:805-813`）：

```c
if (g && (g->access & req->access) != 0) {
    resp->ret     = 0;
    resp->granted = g->access & req->access; /* 抹位掩码 */
}
```

调用方（vfs_server）必须用 `granted` 约束后续操作，不得使用请求掩码
（`perm.h:144-148`）。角色链（role chain）路径也做同样的事：只有
`ATOM_DATA_DOCS_READ` / `ATOM_DATA_DOCS_WRITE` 映射回单个读写位
（`perm-manager.c:770-776`）。

### 6.6 pkg — 端口 `"pkg"`

opcode 见 `pkg.h:68-74`，共 5 个（契约标记为 Phase A FROZEN，`pkg.h:39-40`）：

| opcode | 名称 | 请求 | 响应 |
| --- | --- | --- | --- |
| 1 | `PKG_OP_INSTALL` | `pkg_req_install_t` `{u32 op; char name[64]; char perms[256];}` | `pkg_resp_install_t` |
| 2 | `PKG_OP_LIST` | `pkg_req_list_t` `{u32 op;}` | `pkg_resp_list_t` `{i32 ret; u32 count; char apps[8][64];}` |
| 3 | `PKG_OP_RUN` | `pkg_req_run_t` `{u32 op; char app_id[64];}` | `pkg_resp_run_t` `{i32 ret; i32 pid;}` |
| 4 | `PKG_OP_REMOVE` | `pkg_req_remove_t` | `pkg_resp_remove_t` |
| 5 | `PKG_OP_APP_READY` | `pkg_req_app_ready_t` `{u32 op; char app_id[64];}` | `pkg_resp_app_ready_t` |

边界常量（`pkg.h:60-65`）：

| 常量 | 值 | 含义 |
| --- | --- | --- |
| `PKG_NAME_MAX` | 64 | app_id / blob 名（含 NUL） |
| `PKG_PERMS_MAX` | 256 | 逗号分隔的原子名列表 |
| `PKG_MANIFEST_MAX` | 512 | manifest 文本长度 |
| `PKG_MAX_APPS` | 8 | 安装列表容量 |
| `PKG_MAX_ATOMS` | 8 | 单个 manifest 的权限数 |
| `PKG_PENDING_MAX` | 4 | 并发 RUN 握手槽位 |

### 6.7 user — 端口 `"user"`

opcode 见 `user.h:47-66`，共 18 个：

| opcode | 名称 | 请求类型 | 权限要求 |
| --- | --- | --- | --- |
| 1 | `USER_OP_LOGIN` | `user_req_login_t` | —（自身绑定） |
| 2 | `USER_OP_LOGOUT` | `user_req_login_t` | — |
| 3 | `USER_OP_PASSWD` | `user_req_passwd_t` | 改他人需 OWNER/ADMIN |
| 4 | `USER_OP_USERADD` | `user_req_login_t` | OWNER/ADMIN |
| 5 | `USER_OP_USERDEL` | `user_req_login_t` | OWNER/ADMIN（不得删自己/最后一个管理员） |
| 6 | `USER_OP_USERS` | `user_req_login_t` | OWNER/ADMIN |
| 7 | `USER_OP_WHOAMI` | `user_req_login_t` | — |
| 8 | `USER_OP_VERIFY` | `user_req_login_t` | —（退出保护用） |
| 9 | `USER_OP_STOP` | `user_req_stop_t` | OWNER/ADMIN + 目标非关键服务 |
| 10 / 11 | `USER_OP_LOCK` / `USER_OP_UNLOCK` | `user_req_login_t` | OWNER/ADMIN |
| 12 / 13 | `USER_OP_POLICY_SET` / `USER_OP_POLICY_DUMP` | `user_req_policy_t` | OWNER/ADMIN（代理 policy 服务） |
| 14 | `USER_OP_KILL` | `user_req_kill_t` | OWNER/ADMIN（代理内核 kill 门） |
| 15-18 | `USER_OP_DISK_MOUNT` / `DISK_UNMOUNT` / `DISK_FORMAT` / `DISK_FILL` | `user_req_disk_t` | OWNER/ADMIN（代理块设备驱动管理面） |

复用的请求结构体（`user.h:71-84`）：

```c
typedef struct {
    uint32_t op;
    char     name[32];
    char     password[64];
    uint32_t role;      /* USERADD: 目标角色；PASSWD: 未使用 */
} user_req_login_t;     /* 同时用于 LOGIN / USERADD / VERIFY */
```

> 注意 `role` 字段的用途：它只在 USERADD 中作为目标角色使用
> （`user.h:75`）。LOGIN 使用的角色来自账户表本身。

其它布局：

| 结构体 | 大小 | 出处 |
| --- | --- | --- |
| `user_req_passwd_t`（`op` + `old_password[64]` + `new_password[64]` + `name[32]`） | 164 | `user.h:86-91` |
| `user_req_stop_t`（`op` + `svc[32]`） | 36 | `user.h:97-100` |
| `user_resp_stop_t`（`i32 ret` + `detail[64]`） | 68 | `user.h:102-105` |
| `user_req_policy_t`（`op` + `role` + `verdict` + `cmd[32]`） | 44 | `user.h:112-117` |
| `user_resp_policy_t`（`i32 ret` + `count` + `lines[64][48]`） | 3080 | `user.h:119-123` |
| `user_req_kill_t`（`op` + `i32 pid`） | 8 | `user.h:128-131` |
| `user_req_disk_t`（`op` + `volume[64]` + `size`） | 72 | `user.h:143-147` |
| `user_resp_disk_t`（`i32 ret` + `u64 bytes` + `detail[64]`） | 76 | `user.h:149-153` |

账户策略常量：`USER_NAME_MAX 32`、`USER_PW_MAX 64`、`USER_MAX_ACCOUNTS 16`、
`USER_MAX_LOGIN_ATTEMPTS 5`（`user.h:41-44`、`user.h:69`）。
口令散列是 **FNV-1a-64 + 每账户盐**，属完整性校验而非生产级口令存储
（`user.h:26-27`、`user/main.c:32-33` 明示为已知限制）。

### 6.8 policy — 端口 `"policy"`

opcode 见 `policy.h:50-52`：

| opcode | 名称 | 请求 | 响应 |
| --- | --- | --- | --- |
| 1 | `POLICY_OP_QUERY` | `policy_req_query_t`（`op` + `role` + `count` + `cmds[64][32]`，2060 字节） | `policy_resp_query_t`（`i32 ret` + `count` + `verdicts[64]`，72 字节） |
| 2 | `POLICY_OP_SET` | `policy_req_set_t`（`op` + `role` + `cmd[32]` + `verdict`） | `policy_resp_set_t` |
| 3 | `POLICY_OP_DUMP` | `policy_req_dump_t` | `policy_resp_dump_t`（`ret` + `count` + `lines[64][48]`） |

判定常量（`policy.h:55-59`）：`POLICY_UNSET = 0`（**不在表中即默认允许**）、
`POLICY_ALLOW = 1`、`POLICY_DENY = 2`。

设计定位（`policy.h:18-33`）：三层命令访问架构的中间层——
能力（内核，硬限制，不可伪造）→ 策略库（本服务，按角色判命令名）→ shell 覆盖
（启动时查询并过滤命令表，若策略服务不可用则退到硬编码救援列表）。
该服务明确**不**评估能力，也**不**使用环境变量做任何授权判断。

### 6.9 wm — 端口 `"wm"`

协议唯一定义在共享头 `user/lib/libwm/wm_proto.h`（服务端 `user/services/wm/wm.h` 只是
`#include` 这个共享头的薄壳，`wm.h:35`）：

| opcode | 名称 | 语义 |
| --- | --- | --- |
| 1 | `WM_OP_CREATE` | 建窗，返回 `win_id` |
| 2 | `WM_OP_DESTROY` | 销毁（仅属主/管理员） |
| 3 | `WM_OP_LIST` | 列举注册表，返回 `count` + `lines` |
| 4 | `WM_OP_FOCUS` | 设置键盘焦点 |
| 5 | `WM_OP_MOVE` | 移动（仅属主/管理员） |
| 6 | `WM_OP_WRITE` | 写一行内容（仅属主/管理员） |
| 7 / 8 | `WM_OP_ACTIVATE` / `WM_OP_DEACTIVATE` | 开始/结束桌面会话 |
| 9 | `WM_OP_GET_STATE` | 查询 `{active, focus, n}` |

```c
/* wm_proto.h:48-65 */
typedef struct {
    uint32_t op;
    char     title[32];
    uint32_t x, y, w, h;
    uint32_t win_id;
    uint32_t mx, my;
    uint32_t row;
    char     text[44];
} wm_req_t;      /* 112 字节 */

typedef struct {
    int32_t  ret;
    uint32_t win_id;
    uint32_t count;
    uint32_t active;
    uint32_t focus;
    char     lines[16][56];   /* WM_MAX_WINDOWS × (WM_TITLE_MAX + 24) */
} wm_resp_t;     /* 916 字节 */
```

边界：`WM_MAX_WINDOWS 16`、`WM_TITLE_MAX 32`、`WM_CONTENT_ROWS 8`、`WM_CONTENT_COLS 44`
（`wm_proto.h:32-35`）。窗口变更操作（DESTROY/MOVE/WRITE）按窗口属主 subject 门控
（`user/services/wm/main.c:15-30`）。

### 6.10 gui — 端口 `"gui"`

opcode 见 `gui.h:48-60`；报文是「**统一信封 + 操作载荷**」两段式
（`gui.h:24-27`）：

```text
req  = { u32 op; u32 len; u8 data[len] }      gui_req_t  = 4096 字节
resp = { i32 ret; u8 data[] }                 gui_resp_t = 4096 字节
```

| opcode | 名称 | 载荷（放进 `req.data`） | 响应 |
| --- | --- | --- | --- |
| 1 | `GUI_OP_CREATE` | `gui_req_create_t` `{char title[64]; i32 w; i32 h;}` | `ret` = 新窗口 id（`gui/main.c:765`） |
| 2 | `GUI_OP_DESTROY` | `gui_req_id_t` | `ret` |
| 3 | `GUI_OP_MOVE` | `gui_req_move_t` `{id; x; y}` | `ret` |
| 4 | `GUI_OP_FOCUS` | `gui_req_id_t` | `ret` |
| 5 | `GUI_OP_FILL` | `gui_req_fill_t` `{id; x; y; w; h; u32 color}` | `ret` |
| 6 | `GUI_OP_TEXT` | `gui_req_text_t` `{id; x; y; u32 fg; u32 bg; char text[256]}` | `ret` |
| 7 | `GUI_OP_POLL` | 无 | `gui_resp_poll_t`（1032 字节） |
| 8 | `GUI_OP_POINTER` | 无 | `i32 [x, y, buttons]` 写入 `resp.data`（`gui/main.c:1102-1113`） |
| 9 / 10 | `GUI_OP_ACTIVATE` / `GUI_OP_DEACTIVATE` | 无 | `ret` |
| 11 | `GUI_OP_RESIZE` | `gui_req_resize_t` `{id; w; h}` | `ret` |

事件结构（`gui.h:118-132`）：

```c
typedef struct {
    u32 type;   /* GUI_EV_KEY/MOUSEMOVE/BUTTON/WHEEL/CLOSE */
    u32 code;   /* KEY: 字符/控制码；BUTTON: 1 左 2 右 3 中；WHEEL: 带符号增量 */
    i32 x, y;
    i32 win;    /* 目标窗口：KEY 为焦点窗，鼠标为命中窗（0 = 无） */
    u64 owner;  /* 窗口属主 subject；0 = 广播（事件隔离用） */
} gui_event_t;  /* 32 字节 */

typedef struct { i32 ret; u32 count; gui_event_t events[32]; } gui_resp_poll_t;
```

边界常量（`gui.h:40-45`）：`GUI_MAX_WINDOWS 8`、`GUI_MAX_TITLE 64`、
`GUI_TITLE_H 16`、`GUI_BORDER 1`、`GUI_IPC_MAX 4096`、`GUI_MAX_EVENTS 32`。

### 6.11 net — 端口 `"net"`

opcode 见 `net.h:44-63`，共 16 个，分三层：

| 层 | opcode | 名称 | 说明 |
| --- | --- | --- | --- |
| 以太网 | 1 | `NET_OP_GET_MAC` | 返回 `mac[6]`（`resp.data` 6 字节，`resp.len = 6`） |
| | 2 | `NET_OP_SEND` | `{len; data[]}` 返回 `ret` |
| | 3 | `NET_OP_RECV` | 非阻塞，无包返回 `-6`（`ERR_AGAIN`，`net/main.c:515`） |
| | 4 | `NET_OP_STATS` | 返回 `{rx; tx; err}` |
| L3/L4 | 5 | `NET_OP_SET_IP` | 静态地址 `{ip[4]; gw[4]}` |
| | 6 | `NET_OP_IP_SEND` | 原始 IP 发送 |
| | 7 | `NET_OP_PING` | ICMP echo + 等待回复 |
| | 8 / 11 | `NET_OP_UDP_BIND` / `NET_OP_UDP_UNBIND` | 绑定/解绑本地 UDP 端口（16-65535） |
| | 9 | `NET_OP_UDP_SENDTO` | `{ip[4]; sport; dport; data}` |
| | 10 | `NET_OP_UDP_RECV` | 返回 `{srcip[4]; sport; dport; data}` |
| TCP | 12 | `NET_OP_TCP_LISTEN` | `{port}`，监听**一条**连接 |
| | 13 | `NET_OP_TCP_ACCEPT` | 阻塞，返回 `{peer[4]; peerport}` |
| | 14 / 15 | `NET_OP_TCP_SEND` / `NET_OP_TCP_RECV` | 已建立连接上的收发（RECV 阻塞） |
| | 16 | `NET_OP_TCP_CLOSE` | 关闭连接 |

```c
/* net.h:67-77 */
typedef struct { u32 op; u32 len; u8 data[NET_MTU + 16]; } net_req_t;   /* 1540 */
typedef struct { i32 ret; u32 len; u8 data[NET_MTU + 16]; } net_resp_t; /* 1540 */
```

协议栈常量（`net.h:82-97`）：`ETH_TYPE_IPV4 0x0800`、`ETH_TYPE_ARP 0x0806`、
`IP_PROTO_ICMP 1` / `IP_PROTO_TCP 6` / `IP_PROTO_UDP 17`，`IP_HDR_LEN 20`、
`UDP_HDR_LEN 8`、`TCP_HDR_LEN 20`，TCP 标志位 `TCP_FIN/SYN/RST/PSH/ACK`。
协议栈入口在 `user/services/net/proto.h`：`ProtoInit` / `ProtoRx` /
`ProtoIpSend` / `ProtoPing` / `ProtoUdpBind|Unbind|Sendto|Recv` /
`ProtoTcpListen|Accept|Send|Recv|Close`。
默认静态地址在 `net/main.c:755-760` 配置为 `10.0.2.15`，网关 `10.0.2.2`。

### 6.12 device_mgr — 端口 `"device_mgr"`

opcode 是**从 0 开始**的（与其它服务的「从 1 开始」不同，`device_mgr.c:84-85`）：

| opcode | 名称 | 请求 | 响应 |
| --- | --- | --- | --- |
| 0 | `DMGR_OP_GET_COUNT` | `dmgr_req_t`（8 字节，`index` 未用） | `ret` = PCI 设备数 |
| 1 | `DMGR_OP_GET_DEVICE` | `dmgr_req_t` 用 `index` | `ret = 0` 且填 `dev`，否则负错误码且 `dev` 清零 |

```c
/* device_mgr.c:91-99 */
typedef struct { u32 op; i32 index; } dmgr_req_t;                /* 8 字节  */
typedef struct { i32 ret; pci_device_info_t dev; } dmgr_resp_t;  /* 48 字节 */
```

`pci_device_info_t` 是内核/用户共享 ABI（`kernel/include/kernel/pci.h:36-47`）：
`bus/dev/func`（各 `uint32_t`）、`vendor_id/device_id/class_code`（`uint16_t`）、
`prog_if/revision_id`（`uint8_t`）、`bar[6]`（`uint32_t`）、`irq_line`（`uint8_t`）。
`index` 非法或消息长度不足时返回 `ERR_INVAL`（`device_mgr.c:139-156`、`device_mgr.c:177-180`）。

### 6.13 manager — 端口 `"manager"`（v0.9 控制面）

服务监管者此前只有"拉起服务 + 监控退出"的能力，没有对外接口。v0.9 给它加了一个控制端口，协议定义在 `user/services/manager/manager.h`：

| op | 请求 | 应答 |
| --- | --- | --- |
| `SVC_OP_LIST` (1) | `{ op }` | `entries[0..count-1]`：全部服务 |
| `SVC_OP_STATUS` (2) | `{ op; name }` | `entries[0]`：该服务状态 |
| `SVC_OP_START` (3) | `{ op; name }` | 重新 `BlobGet` + `ProcessCreate` 的结果 |
| `SVC_OP_STOP` (4) | `{ op; name }` | `Kill(pid, SIGKILL)`，并置 `stop_requested` 抑制自动重启 |
| `SVC_OP_RESTART` (5) | `{ op; name }` | 停止 + 重新拉起（`Sleep(2)` 等待回收后重建） |

`svc_entry_t` 每条记录：`name`、`pid`（-1 = 未启动）、`alive`（内核进程表里是否还在）、`monitored`（是否有监控线程）、`restarts`、`running`。

**门控**：控制循环检查**调用方**是否持 `ATOM_SERVICE_MANAGE`（`CapHasAtom`，`kernel/cap/cap.c`）。shell 不持该原子，因此它必须经 `user` 服务管理员代理；代理先校验人类是 OWNER/ADMIN，再转发 —— 与 `disk` / `kill` / `policy` 完全同一条信任边界。`STOP` 额外拒绝 `manager` / `init`（`SvcDispatch()`），shell 侧也拒绝停止 `shell` / `user` 自身。

**重启抑制的时序**：`STOP` 必须**先**置 `stop_requested` 再 `Kill`，因为监控线程此时可能正阻塞在 `ProcessWait(pid)` 上；线程醒来后看到该标志就打印 "stopped by admin (no restart)" 并结束监控循环，不再重启。若不这样做，管理员每停一次服务都会被自动重启一次。

控制端口在 manager 启动序列的**第 0 步**创建并注册，因此任何时刻都可解析；端口号在启动日志末尾统一打印（线程自身启动时 serial 服务尚未就绪，那时打印会被丢弃）。

### 6.14 无 IPC 协议但仍以 blob 形式存在的服务

| blob | 说明 |
| --- | --- |
| `flaky` | 只 `Sleep(5)` 后 `return 7`（`flaky/main.c:32-34`） |
| `canarytest` | 故意溢出栈（`canarytest/main.c:44-46`） |
| `crashpeer` | 注册 `crashpeer` 端口，收一次调用后不回复直接退出（`crashpeer/main.c:32-50`） |
| `hello` / `runtime_demo` / `tui_demo` / `window_demo` / `wm_demo` / `gui_demo` / `sbox_demo` | 纯客户端，不注册任何端口 |

---

### 6.15 perm 的 v1.0 扩展（P3/P4 落地）

§6.5 记录的是 P0–P2 的协议。v1.0 在**不新增 opcode 之外**的前提下把 P3/P4 做成生效语义，
新增 `PERM_OP_CTX_QUERY = 15`，并给既有 op 增加字段：

| op | 新增内容 |
| --- | --- |
| `PERM_OP_CHECK`(3) | 响应新增 `flags`：`PERM_DEC_GRANT_BEAT` / `ROLE_CHAIN` / `DEFAULT_DENY` / `BACKGROUND` / `QUARANTINED` / `SCOPE_MISMATCH` / `EXPIRED`。调用者可以据此区分"被授权"和"为什么没被授权"，测试也用它断言 |
| `PERM_OP_ANSWER`(2) | 请求新增 `ttl_ticks`（0=永久，>0=限时授权）与 `scope_hash`；**回答不会放大作用域**（未给 scope 时继承询问自身的 scope） |
| `PERM_OP_GRANT`(5) | 请求新增 `expiry_ticks` / `scope_hash` / `source`；响应回填真正生效的截止时间与 scope |
| `PERM_OP_FREQ`(10) | 请求新增 `clear_quarantine`；响应新增 `denies` / `quarantined` / `quarantine_ticks` |
| `PERM_OP_CONTEXT`(9) | 请求新增 `list`（只读查询）；响应改为 `{ret, count, entries[16]}`，每条含 subject/foreground/quarantined |
| `PERM_OP_CTX_QUERY`(15) | **新增**：只读地读取前台/后台表，返回同样的 `perm_resp_context_t` |
| `PERM_OP_AUDIT`(13) | 请求新增 `subject_id` / `verdict_filter` / `since_tick` / `max_entries`；条目新增 `event`（`PERM_EV_*`） |
| `PERM_OP_POLICY_SAVE`(11) | 请求新增 `include_expired`（默认只导出仍然有效的授权） |
| `PERM_OP_POLICY_LOAD`(12) | 快照格式升级到 v2（携带 expiry/scope/source），**兼容读取 v1**；仍是全有或全无 + 热更新 |

**审计事件的 verdict 语义**：`perm_audit_ent_t.verdict` 直接用规则表的枚举 ——
`PERM_VERDICT_ALLOW = 1` / `PERM_VERDICT_DENY = 0`（**不是**独立的 0=granted 约定）。
读日志或写工具时按枚举判断，别按数字直觉判断。

### 6.16 vfs 的 v1.0 扩展（对象模型补完）

| opcode | 名称 | 请求 → 响应 | 说明 |
| --- | --- | --- | --- |
| 21 | `VFS_OP_STAT_HANDLE` | `vfs_req_stat_handle_t` → `vfs_resp_stat_handle_t` | 按句柄取元数据；每次调用重跑判权；失效句柄 → `VFS_ERR_STALE`；枚举器 token → `ERR_INVAL` |
| 22 | `VFS_OP_TRUNCATE` | `vfs_req_truncate_t` → `vfs_resp_truncate_t` | 按句柄改长度；缩小释放尾部、扩大零填充；只读句柄 → `VFS_ERR_PERM` |
| 23 | `VFS_OP_REFRESH_BOOKMARK` | `vfs_req_refresh_bookmark_t` → `vfs_resp_refresh_bookmark_t` | 续期书签（0=永久）；已过期但授权仍在的书签也允许续期，否则 `VFS_ERR_ACCESS` |

同时生效的语义变化：**过期的书签在 RESOLVE 时返回 `VFS_ERR_STALE`**（此前 `expiry_ticks` 从未被检查）、
`MOVE` 拒绝移入自身子树（`ERR_INVAL`）、服务端统一做名称校验、删除后相关句柄/枚举器一律 `VFS_ERR_STALE`。
详见 [vfs_design.md](vfs_design.md) §13。

## 7. 关键流程时序

### 7.1 Powerbox 授权（vfs → perm → term perm.ui → 用户 y/n → 能力签发）

```text
 客户端          vfs_server            perm-manager           term("perm.ui")        用户
   |                 |                      |                       |                  |
   |--CREATE_BOOKMARK->|                    |                       |                  |
   |  (VFS_OP 10)     | IpcRecvFrom 取       |                       |                  |
   |                  | 调用方 subject       |                       |                  |
   |                  |--PERM_OP_CHECK(3)-->|                       |                  |
   |                  | {resource,access,    | 1) 授权表命中?        |                  |
   |                  |  url,subject_id}     |    (grant&req)!=0     |                  |
   |                  |                      |    -> ret=0, granted=抹位掩码            |
   |                  |                      | 2) 角色规则链 (role,atom) 首个匹配       |
   |                  |                      |    ALLOW -> ret=0, granted=GrantedForAtom|
   |                  |                      |    DENY  -> VFS_ERR_ACCESS（不弹面板）   |
   |                  |                      | 3) 默认拒绝 -> 建/复用 PENDING 查询       |
   |                  |<--VFS_ERR_ACCESS-----|    perm-manager.c:830-847               |
   |                  |   + query_id         |--PERM_OP_UI_SHOW(6)-->|                  |
   |<-(-105 EACCES)---|                      | {query_id,subject,pid, | 渲染面板        |
   |                  |                      |  name,url,access,      | 抢键盘焦点      |
   |                  |                      |  state,label[128]}     | (线程 B)        |
   |                  |                      |                       |                  |
   |                  |                      |<--PERM_OP_ANSWER(2)----|<--按 y ----------|
   |                  |                      | {query_id, allow=1}    |  IpcSend 而非    |
   |                  |                      |                       |  IpcCall（防死锁）|
   |                  |                      | (a)grant_upsert(subject,resource,access) |
   |                  |                      | (b)DecisionEncode -> CapGrantToSubject() |
   |                  |                      |    把 ALLOW 编码成内核原子能力            |
   |                  |                      |    perm-manager.c:882-898 / 572-582      |
   |                  |                      |--UI_SHOW(ALLOWED)----->| 重绘结果并保持   |
   |                  |                      |                       | 若干 tick 后恢复 |
   |--CREATE_BOOKMARK->| (重试)               |                       |                  |
   |                  |--PERM_OP_CHECK------>| 授权表命中 -> ret=0   |                  |
   |<--书签 blob-------|  vfs_bookmark_t      |                       |                  |
```

| 步骤 | 事实 | 出处 |
| --- | --- | --- |
| subject 来源 | vfs_server 用 `IpcRecvFrom` 取真实调用方 subject 填进 CHECK 请求 | `vfs_server.c:1996-2001`、`vfs_server.c:1511` |
| CHECK 门控 | `PERM_OP_CHECK` 要求调用者持有 `ATOM_SERVICE_MANAGE`，否则 `ERR_DENIED`（防止把 perm 当授权预言机） | `perm-manager.c:793-800` |
| ANSWER 门控 | 同样要求 `ATOM_SERVICE_MANAGE`，沙盒应用无法自批自己的查询 | `perm-manager.c:863-874` |
| 决策顺序 | 授权表命中 → 角色链（override-first）→ 默认拒绝 + Powerbox | `perm-manager.c:805-847` |
| 查询复用 | 同 `(subject, resource)` 的 PENDING 查询被复用，不重复弹面板 | `perm-manager.c:611-623` |
| 显示名解析 | 新查询用 `ProcInfoBySubject()` 取 pid/name；主体已死则退化为 `"subject <id>"` | `perm-manager.c:635-651` |
| 能力签发 | `DecisionEncode()` 调 `CapGrantToSubject(subject, atom, RIGHT_ALL, 0, 0)` | `perm-manager.c:572-582` |
| access→atom 映射 | WRITE → `ATOM_DATA_DOCS_WRITE`；READ → `ATOM_DATA_DOCS_READ`；其余 → `ATOM_NONE` | `perm-manager.c:551-557` |
| 面板线程模型 | `perm.ui` 服务线程永远停在 `IpcRecv`；键盘线程轮询 `s_ui_await` 并在拿到 y/n 后抢/还焦点 | `term.c:1975-1983`、`term.c:1933-1972` |
| 应答用 `IpcSend` | perm 在 `DoAnswer` 内同步回推 `UI_SHOW`，term 若用 `IpcCall` 会死锁 | `term.c:1909-1926` |
| 静默模式 | `PERM_OP_SET_QUIET` 抑制 UI_SHOW，但查询照常创建、ANSWER/QUERY 照常工作 | `perm.h:102-107`、`perm-manager.c:166-171` |
| 授权前错误码 | 未授权一律 `VFS_ERR_ACCESS(-105)`，客户端看到 `-EACCES` | `perm.h:124-125`、`vfs.h:152` |

### 7.2 pkg 安装 .ops 并签发原子能力

```text
 shell             pkg-manager                    vfs            perm              app
   |                    |                          |              |                 |
   |--PKG_OP_INSTALL(1)->|                          |              |                 |
   |  {name, perms}     | BlobGet(name) 取 ELF      |              |                 |
   |                    | 组装 .ops(app_id+manifest)|              |                 |
   |                    |--FsCreateDir("/Volumes/Users/Apps")----->|                 |
   |                    |<--(-105 或 0)-------------|  首次写 Users 卷触发 Powerbox   |
   |                    |--FsWrite(.../app.ops)---->|              |                 |
   |<--ret--------------|  pkg_manager.c:388-410    |              |                 |
   |                    |                          |              |                 |
   |--PKG_OP_RUN(3)---->| FsOpenItem 读回并解析 manifest           |                 |
   |  {app_id}          | 记 s_pending[pid] = manifest atoms       |                 |
   |                    | ProcessCreate(app_id, payload ELF) --------------------->|
   |<--{pid}------------|  pkg.h:110-127            |              |                 |
   |                    |                          |              |                 |
   |                    |<---------------------PKG_OP_APP_READY(5)------------------|
   |                    | IpcRecvFrom -> caller subject（不可伪造） |                 |
   |                    | ProcInfoBySubject(subject) -> {pid,name} |                 |
   |                    | 自报 app_id 必须等于内核给的 name        |                 |
   |                    | 必须匹配 s_pending 中的 (pid, app_id)    |                 |
   |                    | CapGrantToSubject(subject, atom, ...) 逐个签发           |
   |                    |-----------reply 0--------------------------------------->|
```

| 事实 | 出处 |
| --- | --- |
| 安装目录 | `PKG_APPS_DIR = "/Volumes/Users/Apps"`，文件为 `<app_id>/app.ops` | `pkg_manager.c:68`、`pkg_manager.c:388-400` |
| 未知权限（含被禁用的管理原子）返回 `ERR_INVAL` | `pkg.h:82-83` |
| RUN 只登记不签发 | 「app 在运行前无法接收原子」 | `pkg.h:113-116` |
| APP_READY 三重校验 | subject → `ProcInfoBySubject` → 名称一致 → 匹配 pending 记录 | `pkg_manager.c:530-566` |
| 签发门控 | `CapGrantToSubject` 内核侧要求调用者持有 `ATOM_SERVICE_MANAGE` | `kernel/syscall/syscall.c:382-389` |
| 失败语义 | 非 0 一律视为「未获得任何权限」 | `pkg.h:149-152` |
| 验证样例 | `sbox_demo` 装两次（带 `sys.set_time` 与不带），只报告 `OsSetTime()` 的真实返回值；自授权 `CapCreateAtom()` 必须 `ERR_NOCAP` | `sbox_demo/main.c:24-29`、`sbox_demo/main.c:54-60` |

### 7.3 login 绑定 subject 到账户并 ROLE_SET

```text
 shell                     user 服务                     perm-manager
   |                          |                              |
   |--USER_OP_LOGIN(1)------->|                              |
   |  {name, password}        | acct_find(name) -> 账户       |
   |                          | disabled? -> ERR_DENIED       |
   |                          | 校验口令（FNV-1a-64 + 盐）     |
   |                          | 失败: fail_count++;           |
   |                          |   >=5 -> 自动锁定             |
   |                          | 成功: fail_count = 0          |
   |                          | 绑定 s_binds[caller] = acct   |
   |                          |  (caller = IpcRecvFrom 的     |
   |                          |   内核 subject，不可伪造)      |
   |                          |--PERM_OP_ROLE_SET(7)-------->|  要求调用者 OWNER/ADMIN
   |                          |  {subject_id, role}          |  角色立即对后续检查生效
   |                          |<--{ret, role}----------------|  （不重写既有授权）
   |                          | 若 OWNER/ADMIN:               |
   |                          |  CapGrantToSubject(caller,    |
   |                          |    ATOM_SYS_SHUTDOWN, ...)    |
   |                          | 否则 CapRevokeByAtom(...)     |
   |<--{ret, role, name}------|                              |
   |  CmdFilterLoad() 重新拉取该角色的命令策略                  |
```

| 事实 | 出处 |
| --- | --- |
| 调用方 subject 从 `IpcRecvFrom` 取 | `user/main.c:987`；`user.h:30-32` 明示「绝不从请求字节取身份」 |
| 失败计数与锁定 | `USER_MAX_LOGIN_ATTEMPTS = 5` | `user.h:69`、`user/main.c:212-225` |
| 角色同步 | `PermRoleSet()` → `PERM_OP_ROLE_SET` | `user/main.c:171-186`、`user/main.c:253-258` |
| 关机能力联动 | OWNER/ADMIN 登录授予 `ATOM_SYS_SHUTDOWN`，否则撤销 | `user/main.c:260-268` |
| 角色热更新语义 | 立即影响后续检查，但**不重写授权**（grant 优先于角色默认） | `perm.h:234-239` |
| 首次启动引导 | 无账户时创建默认 `admin/admin`，角色 OWNER，并打印到串口日志 | `user/main.c:20-23` |
| shell 侧后续动作 | 登录后立即 `CmdFilterLoad()` 重新加载策略 | `shell.c:3933-3935` |

### 7.4 stop 退出保护（TUI 确认 → whoami → 掩码口令 → VERIFY → STOP）

```text
  用户           shell                 libtui/term        user 服务            内核
   |               |                       |                 |                  |
   |--stop pkg---->| CmdStop(argv[1])      |                 |                  |
   |               |--TuiConfirm(20,14,60,...)->| 画确认框  |                  |
   |<--输入 y/n----|                       |                 |                  |
   |               |  n -> "stop: cancelled" 直接返回        |                  |
   |               |--USER_OP_WHOAMI(7)------------------->| 查 subject 绑定   |
   |               |<--{name, role}-----------------------| 未登录 -> 报错    |
   |               |--TuiInputLine(..., mask=1)->| 掩码输入 |                  |
   |<--输入口令----|                       |                 |                  |
   |               |--USER_OP_VERIFY(8)------------------>| 按 name+password  |
   |               |  {name, password}     |                 | 校验（不改绑定）  |
   |               |<--{ret}------------------------------|                  |
   |               |  失败 -> "wrong password" 返回        |                  |
   |               |--USER_OP_STOP(9)-------------------->| (1) caller 绑定账户|
   |               |  {svc}                |                 |     必须 OWNER/ADMIN
   |               |                       |                 | (2) SvcIsCritical(svc)? 拒绝
   |               |                       |                 | (3) ProcessList 找同名
   |               |                       |                 |     且非 ZOMBIE/FINISHED
   |               |                       |                 |--Kill(pid,SIGKILL)->|
   |               |<--{ret, detail}----------------------|  （user 持有原子）  |
   |<--"stop: 'pkg' (PID n) stopped"---------|             |                  |
```

| 事实 | 出处 |
| --- | --- |
| 三步流程顺序 | `shell.c:4184-4189` 注释 + `shell.c:4191-4259` 实现 |
| 确认对话框参数 | `TuiConfirm(20, 14, 60, "Confirm Stop", msg, ...)` | `shell.c:4199-4201` |
| 掩码输入 | `TuiInputLine(5, 31, "Admin password: ", pw, sizeof(pw), 1)`，末参数 `mask=1` | `shell.c:4227` |
| VERIFY 用 WHOAMI 得到的账户名 | `strncpy(vq.name, who.name, ...)` | `shell.c:4230-4234` |
| 关键服务名单（STOP） | `serial, term, keyboard, vfs, fs_mem_driver, fs_virtio_blk_driver, perm, manager, user` | `user/main.c:574-577` |
| 关键服务名单（KILL） | 同上再加 `policy` | `user/main.c:749-751` |
| kill 由 user 执行的原因 | shell 端不具备通过内核 kill 门所需的 `ATOM_SERVICE_MANAGE` 判定链 | `user/main.c:29-32`、`user.h:125-127` |
| 目标状态过滤 | 跳过 `state == 3`(ZOMBIE) / `4`(FINISHED) | `user/main.c:619-620`（状态码定义见 `kernel/include/kernel/proc_info.h:30`） |
| 掩码回显实现 | `ReadLineImpl(buf, maxlen, mask)`，`mask=1` 时回显 `*` | `shell.c:979-991`、`shell.c:1349-1351` |

### 7.5 GUI 窗口创建与事件轮询

```text
 client(gui_demo)      gui 服务(server 线程)     gui 服务(input 线程)      keyboard
   |                      |                          |                     |
   |--GUI_OP_ACTIVATE(9)->| s_active=1, 指针居中,     |                     |
   |                      | 抢键盘焦点, 全屏重绘      |                     |
   |<--ret----------------|  gui/main.c:1116-1123    |                     |
   |                      |                          |                     |
   |--GUI_OP_CREATE(1)--->| 校验尺寸 <= fb - 边框 - 标题栏                 |
   |  {title[64],w,h}     | 取空槽（GUI_MAX_WINDOWS=8）                    |
   |                      | malloc(w*h*4) 离屏 32bpp 缓冲                  |
   |                      | owner = caller（不可伪造 subject）             |
   |                      | 级联自动摆放（400 次尝试避让）                  |
   |                      | GuiComposite() 重合成 -> 标脏区                |
   |<--ret = 窗口 id------|  gui/main.c:659-770      |                     |
   |                      |                          |                     |
   |--GUI_OP_FILL/TEXT--->| 属主校验（owner==caller 或 owner==0）          |
   |                      | 画进离屏缓冲 -> 重合成                    |    |
   |                      |                          |--KBD_OP_READ(1)---->|
   |                      |                          |--KBD_OP_MOUSE_READ(5)->|
   |                      |                          | 命中测试 -> 焦点切换    |
   |                      |                          | 写事件环（带 owner）    |
   |--GUI_OP_POLL(7)----->| 只消费 owner==caller 或 owner==0 的事件,      |
   |<--{count,events[32]}--| 其余压回环中（事件隔离）|                     |
   |                      |  gui/main.c:1070-1100    |                     |
   |                      |                          |                     |
   |--GUI_OP_DEACTIVATE(10)->| TERM_OP_REDRAW 恢复文本屏 + 释放焦点         |
```

| 事实 | 出处 |
| --- | --- |
| 合成器激活前完全空闲 | 「不写 fb、不占焦点，直到客户端调用 `GUI_OP_ACTIVATE`」 | `gui/main.c:28-30` |
| 尺寸校验 | `max_w = fb.w - 2*GUI_BORDER`，`max_h = fb.h - 2*GUI_BORDER - GUI_TITLE_H`，最小 16 | `gui/main.c:674-681` |
| 窗口属主 | `slot->owner = caller`（来自 `IpcRecvFrom`） | `gui/main.c:713`、`gui/main.c:1191` |
| 离屏缓冲 | 32bpp，`pitch = w * 4` | `gui/main.c:700-759` |
| 自动摆位 | 400 次尝试避开已有窗口，越界则接受重叠而非失败 | `gui/main.c:719-753` |
| 属主门控 | `!w || (w->owner != 0 && w->owner != caller)` → 拒绝 | `gui/main.c:785`、`gui/main.c:831`、`gui/main.c:885` |
| 事件隔离 | POLL 只取本客户端事件并压缩环，保留他人事件 | `gui/main.c:1077-1095` |
| 事件类型与字段含义 | KEY 跟随焦点窗，鼠标跟随命中窗，`owner = 0` 表示广播 | `gui.h:106-126` |
| 输入线程用到的 opcode | `KBD_OP_READ = 1`、`KBD_OP_MOUSE_READ = 5` | `gui/main.c:72-73`、`gui/main.c:1264`、`gui/main.c:1283` |
| 指针查询 | `GUI_OP_POINTER` 返回 `i32 [x, y, buttons]` 到 `resp.data` | `gui/main.c:1102-1113` |

### 7.6 TCP 连接建立

`net` 服务的 TCP 是**单连接**模型（`net.h:57`、`net/proto.h:60-65`）。
shell 的 `net tcp <port>` 走服务端（监听）角色，`net tcp <ip> <port> <msg>` 走客户端角色。

```text
  对端主机              net 服务(协议栈 proto.c)          shell / 客户端
     |                        |                                |
     |                        |<--NET_OP_TCP_LISTEN(12)--{port}--|
     |                        |   ProtoTcpListen(port)         |
     |                        |                                |
     |--SYN------------------>| 收 SYN -> 回 SYN+ACK           |
     |<--SYN+ACK--------------|                                |
     |--ACK------------------>| 进入 ESTABLISHED               |
     |                        |--(accept 返回)---------------->|
     |                        |<--NET_OP_TCP_ACCEPT(13)---------|  阻塞
     |                        | 返回 {peer[4], peerport}        |
     |                        |                                |
     |--数据----------------->| 入队                            |
     |                        |<--NET_OP_TCP_RECV(15)-----------|  阻塞
     |                        | 返回 {data}（附 peer_port）      |
     |<--数据-----------------|<--NET_OP_TCP_SEND(14)--{data}----|
     |                        |                                |
     |                        |<--NET_OP_TCP_CLOSE(16)----------|
     |<--FIN------------------| 关闭已建立连接                   |
```

| 事实 | 出处 |
| --- | --- |
| TCP 相关 opcode 12-16 | `net.h:57-63` |
| `ProtoTcpAccept` 阻塞约 6 秒、`ProtoTcpRecv` 阻塞 | `net/proto.h:62-64` |
| 上层调用与收包同一循环 | `NetServerLoop()` 先 `NetServiceRx()` + `ProtoRx()`，再 `IpcRecv` | `net/main.c:458-479` |
| `NET_OP_RECV` 无包语义 | 返回 `-6`（`ERR_AGAIN`），非阻塞 | `net/main.c:505-516` |
| Rx 采用轮询而非中断 | QEMU pcnet INTx 经电平触发 8259 会风暴，故禁用 INTx | `net/main.c:23-25`、`net/main.c:450-457` |
| shell 命令 | `net mac | arp | ping <ip> | tcp <ip> <port> <msg> | recv | stats` | `shell.c:2702`、`shell.c:2649-2699` |

---

## 8. 客户端库与服务端口的对应关系

| 库目录 | 面向的端口（`PortGet` 字面串） | 解析时机 | 主要 API | 出处 |
| --- | --- | --- | --- | --- |
| `libos/` | **无端口**：直接系统调用封装 | — | `IpcCall` / `IpcRecv` / `PortGet` / `PortRegister` / `ThreadCreate` / `MutexCreate` / `BlobGet` / `ProcessCreate` / `ProcessWait` / `CapGrantToSubject` / `PciGetCount` / `FbGetInfo` / `fb_map` / `GetSubject` / `ProcInfoBySubject` | `user/lib/libos/syscalls.h`（425 行）；另有 `elf_parse.c`、`spinlock.h` |
| `libc/` | **无端口**：标准 C 库；`printf` 走内核 `SYS_DEBUG_LOG` | — | string / stdio / stdlib / math / time / setjmp / threads / wchar / utf8 / ctype | `user/lib/libc/` |
| `libipc/` | 端口名由调用者传入（薄封装） | 每次 `IpcConnect` | `IpcConnect(name)` → `PortGet(name)`；`IpcRequest(port, req, resp)` → `IpcCall` | `user/lib/libipc/ipc.c:51-63` |
| `libfs/` | `"vfs"` | 首次使用时解析并缓存（`s_vfs_port`） | `FsGetItem` / `FsCreateDir` / `FsDeleteItem` / `FsOpenItem` / `FsRead` / `FsWrite` / `FsClose` / `FsEnumBegin` / `FsEnumNext` / `FsEnumEnd` / `FsStatVolume` / `FsWhoami` / `FsListVolumes` / `FsCreateBookmark` / `FsResolveBookmark` / `FsRevokeBookmark` / `FsMoveItem` / `FsReadMap` | `user/lib/libfs/fs.c:82-89`、`user/lib/libfs/fs.h:30-120` |
| `libpkg/` | `"pkg"` | 首次使用时解析并缓存（`s_pkg_port`） | `PkgInstall` / `PkgList` / `PkgRun` / `PkgRemove` / `PkgReady` | `user/lib/libpkg/pkg.c:50-56`、`user/lib/libpkg/pkg.h:34-49` |
| `libtui/` | `"term"` + `"keyboard"` | term 端口在 `TuiPortGet()`；键盘端口在 `TuiKbdGet()`（惰性） | `TuiWrite` / `TuiWriteStr` / `TuiClear` / `TuiStatus` / `TuiRenderBox` / `TuiRenderLineAt` / `TuiSetCursor` / `TuiGetCursor` / `TuiSnapshot` / `TuiRestore` / `TuiGetSize` / `TuiInputLine` / `TuiConfirm` / `TuiMenu` | `user/lib/libtui/tui.c:389`、`user/lib/libtui/tui.h:41-49` |
| `libwm/` | `"wm"` | `WmPortGet()` 惰性解析并缓存（`s_wm_port`） | `WmCreate` / `WmDestroy` / `WmList` / `WmFocus` / `WmMove` / `WmWrite` / `WmActivate` / `WmDeactivate` / `WmGetState` | `user/lib/libwm/wm.c:24-32`、`user/lib/libwm/wm.h:33-90`、`user/lib/libwm/wm_proto.h:29` |
| `libgui/` | **无端口**：本地像素绘制 + 直接映射 framebuffer | — | `GuiFbOpen` / `GuiFbClose` / `GuiPixel` / `GuiFill` / `GuiHline` / `GuiVline` / `GuiRect` / `GuiText` / `GuiTextWidth` / `GuiBlit` | `user/lib/libgui/gui.h:53-84`；映射门控见 `user/lib/libgui/gui.h:18-22`（与 term 相同需 `ATOM_SERVICE_MANAGE`） |
| `libime/` | **无端口**：纯查表库 | — | `ImeLookup(pinyin, chars_out)`、`ImePrefix(pinyin)` | `user/lib/libime/ime.h:38-57` |

补充说明：

- `libfs` / `libpkg` / `libwm` 三个库都用「首次解析 + 缓存」模式，端口号一旦缓存就不再重解析；
  服务重启后旧句柄会失效——这是当前实现的已知行为（`fs.c:82-89`、`pkg.c:50-56`、`wm.c:24-32`）。
- `libgui` **不是** `gui` 服务的客户端库：它直接映射系统 framebuffer 做像素绘制；
  `gui` 服务的客户端协议由包含 `user/services/gui/gui.h` 的程序自行封装
  （例如 `gui_demo/main.c:35-44` 的 `GuiCall()`）。
- `libime` 只提供拼音到候选字的查表，候选限制在 `font_cjk.h` 覆盖的码点内，
  保证 term/gui 都能渲染（`ime.h:17-22`）。
- 终端区域快照/恢复上限是 `TUI_MAX_REGION_CELLS = 2036` 个单元，
  恰好由 4096 报文决定：8 字节头 + 16（x,y,w,h）+ 2×2036（`tui.h:51-53`、`term.c:140`）。

---

## 9. 服务间身份约定

### 9.1 规则

> **所有需要知道「谁在请求」的服务，必须用 `IpcRecvFrom` 取内核填充的 sender subject，
> 绝不允许信任请求体里的身份字段。**

理由与出处：

1. sender subject 由内核在消息入队/投递时写入（`kernel/ipc/ipc.c:213-216`、`:409`、`:424`、`:526`），
   用户态无法伪造：发送方只提供消息缓冲区内容，不参与填充该字段。
2. `perm.h` 对此有明确注释：`perm_req_check_t.subject_id` 是
   「由 vfs_server 用 `ipc_recv_from` 取得后填入，**绝不接受 app 自报值**（不可伪造）」
   （`perm.h:133-135`）。
3. `user.h` 同样声明：「调用方身份总是来自 `IpcRecvFrom`（内核填充的 subject），
   绝不来自请求字节」（`user.h:30-32`）。

### 9.2 可信代理（trusted proxy）模式

当「知道身份的进程」和「有权限的进程」不是同一个时，OpSys 用**可信代理**转交身份：

```text
   app / shell (有身份, 无原子)          可信代理 (有原子)             权威服务
   ------------------------------        ----------------             ----------
   FsOpenItem(...)                       vfs_server                   perm
        |                                  | IpcRecvFrom -> subject     |
        |--VFS_OP_OPEN_ITEM--------------->|                            |
        |                                  |--PERM_OP_CHECK{subject,..}->|
        |                                  |  (subject 由 vfs 填，         |
        |                                  |   不采用 app 自报值)         |
        |<--(-105 / handle)----------------|                            |
```

| 代理 | 它持有 | 它代谁做事 | 出处 |
| --- | --- | --- | --- |
| `vfs_server` | `ATOM_SERVICE_MANAGE`、`SYS_SHM_*` | 用调用方真实 subject 向 perm 发 CHECK；`VFS_OP_WHOAMI` 把 `SYS_GET_SUBJECT` 代理给沙盒客户端（沙盒不直接碰内核） | `vfs_server.c:1996-2001`、`vfs.h:411-413` |
| `user` 服务 | `ATOM_SERVICE_MANAGE` | 代理 `USER_OP_KILL`（内核 SYS_KILL）、`POLICY_SET/DUMP`（policy 服务）、`DISK_MOUNT/UNMOUNT/FORMAT/FILL`（块设备驱动管理面，该面由驱动侧 `CapHasAtom(ATOM_SERVICE_MANAGE)` 门控） | `user.h:107-111`、`:125-127`、`:138-142`；`user/main.c:658-735` |
| `perm-manager` | `ATOM_SERVICE_MANAGE` | 唯一的原子能力签发者（`DecisionEncode` → `CapGrantToSubject`） | `perm-manager.c:559-582` |
| `pkg-manager` | `ATOM_SERVICE_MANAGE` | 在 APP_READY 时把 manifest 原子签进应用的内核能力表 | `pkg.h:147-152`、`pkg_manager.c:530-566` |
| `term`（`perm.ui` 线程） | `ATOM_SERVICE_MANAGE` | 代表用户回答 Powerbox 查询 | `perm-manager.c:856-862` |
| `fs_virtio_blk_driver` | `ATOM_SERVICE_MANAGE` | 用 `CapHasAtom` 门控自己的挂载/卸载/格式化/填充控制面 | `vfs.h:501-509` |

### 9.3 当前未使用 IpcRecvFrom 的服务（代码事实）

为保持事实准确，下表列出**主循环用 `IpcRecv` 而非 `IpcRecvFrom`** 的服务。
它们不使用调用方身份做授权判断，因此当前不构成漏洞；若未来要给它们加基于身份的判定，
必须先改成 `IpcRecvFrom`。

| 服务 | 主循环 | 说明 |
| --- | --- | --- |
| `serial` | `IpcRecv`（`serial.c:440`） | 无授权判定 |
| `term`（`term` 端口） | `IpcRecv`（`term.c:1569`） | 显示输出；`perm.ui` 端口同样用 `IpcRecv`（`term.c:2001`），因为 perm 是唯一推送方 |
| `net` | `IpcRecv`（`net/main.c:474`） | 无授权判定（DMA 池创建由 `ATOM_SERVICE_MANAGE` 门控） |
| `device_mgr` | `IpcRecv`（`device_mgr.c:172`） | 只读 PCI 快照 |
| `fs_mem_driver` | `IpcRecv`（`fs_mem_driver.c:754`） | 数据面驱动，挂载由 vfs 侧校验 |
| `crashpeer` | `IpcRecv`（`crashpeer/main.c:46`） | 测试夹具 |
| `flaky` / `canarytest` / 各 demo | 不接收 | 纯客户端 |

使用 `IpcRecvFrom` 的服务（正确形态）：`vfs`（`vfs_server.c:2001`）、
`perm`（`perm-manager.c:1438`）、`pkg`（`pkg_manager.c:596`）、
`user`（`user/main.c:987`）、`wm`（`wm/main.c:389`）、
`policy`（`policy/main.c:278`）、`gui`（`gui/main.c:1191`）、
`keyboard`（`keyboard.c:628`）、
`fs_virtio_blk_driver`（`fs_virtio_blk_driver.c:1129`，用于门控 `DRV_OP_CTRL_*` 管理面）。

### 9.4 内核侧的身份相关 syscall

| syscall | 作用 | 门控 | 出处 |
| --- | --- | --- | --- |
| `SYS_GET_SUBJECT` | 返回调用者自己的 subject | 无 | `kernel/syscall/syscall.c:979-987` |
| `SYS_PROC_INFO_BY_SUBJECT` | subject 到 `{pid, name, uuid_hi, uuid_lo}` | 无（只读） | `kernel/syscall/syscall.c:1095-1096`、`kernel/include/kernel/proc_info.h:48-53` |
| `SYS_CAP_GRANT_TO_SUBJECT` | 签发原子能力到目标主体的能力表 | `ATOM_SERVICE_MANAGE` | `kernel/syscall/syscall.c:370-389` |
| `SYS_CAP_REVOKE_BY_ATOM` | 按 (subject, atom, scope) 全局撤销 | `ATOM_SERVICE_MANAGE` | `kernel/syscall/syscall.c:342-355` |
| `SYS_CAP_HAS_ATOM` | 查询某主体是否持有某原子 | `ATOM_SERVICE_MANAGE` | `kernel/syscall/syscall.c:403-425` |
| `SYS_PROCESS_LIST` | 列出进程快照（`proc_info_t[64]`） | 无 | `kernel/include/kernel/proc_info.h:28-35` |

`proc_ident_t`（内核签发身份记录）固定布局：`i32 pid; char name[64]; u64 uuid_hi; u64 uuid_lo;`
（`kernel/include/kernel/proc_info.h:48-53`）；系统/内核进程的 subject 为 0，
其 UUID 为 `(0, 0)`（`kernel/include/kernel/proc_info.h:44-46`）。
`cap_lookup(subject, atom, scope)` 形态的门控由 `CapLookupByAtom()` 完成——
纯内核能力表扫描，零 IPC（`kernel/include/kernel/cap.h:210-238`）。

---

## 10. 客户端使用服务的惯例（模板）

以 `libfs` 为范本，所有客户端遵循同一套模式（`user/lib/libfs/fs.c:82-108`）：

```c
static int s_vfs_port = -1;

static int FsPort(void) {                 /* 惰性解析 + 缓存 */
    if (s_vfs_port < 0) {
        s_vfs_port = PortGet("vfs");
        if (s_vfs_port < 0)
            return s_vfs_port;            /* 传播 ERR_NOENT */
    }
    return s_vfs_port;
}

int FsGetItem(const char *url, vfs_item_info_t *out_item) {
    if (!url || !out_item) return ERR_INVAL;
    int port = FsPort();
    if (port < 0) return port;

    vfs_req_get_item_t *req = (vfs_req_get_item_t *)s_req;
    memset(req, 0, sizeof(*req));                    /* 1. 清零 */
    req->op = VFS_OP_GET_ITEM;                       /* 2. req[0] = opcode */
    strncpy(req->path, url, sizeof(req->path) - 1);  /* 3. 有界拷贝 */

    vfs_resp_get_item_t *resp     = (vfs_resp_get_item_t *)s_resp;
    int                  resp_len = (int)sizeof(*resp);   /* 4. 进：缓冲容量 */
    int r = IpcCall(port, req, (int)sizeof(*req), resp, &resp_len);
    if (r < 0)          return r;          /* 5. 传输错误 */
    if (resp->ret < 0)  return resp->ret;  /* 6. 服务错误 */
    *out_item = resp->item;
    return 0;
}
```

服务端对应模板（以 `device_mgr` 为例，`device_mgr.c:168-184`）：

```c
static void DmgrServerLoop(int port) {
    for (;;) {
        int msg_len = (int)sizeof(s_req);
        int token   = 0;
        int ret     = IpcRecv(port, s_req, &msg_len, &token);
        if (ret < 0) { printf("device_mgr: ipc_recv failed (%d)\n", ret); ThreadExit(1); }
        if (msg_len < (int)sizeof(u32)) {      /* 无 opcode */
            DmgrReply(token, ERR_INVAL, NULL);
            continue;
        }
        u32 op = *(u32 *)s_req;
        DmgrHandleRequest(token, op, msg_len); /* 每个 handler 再看 msg_len 是否够长 */
    }
}
```

需要身份时把 `IpcRecv` 换成 `IpcRecvFrom` 并加一个出参（`vfs_server.c:2001`）：

```c
u64 caller_subject = 0;
ret = IpcRecvFrom(port, s_req, &msg_len, &token, &caller_subject);
```

---

## 11. 已知限制与未来工作

以下全部来自源码注释，不是推测：

| 限制 | 说明 | 出处 |
| --- | --- | --- |
| 无挂起/超时检测 | 退出检测靠阻塞 `ProcessWait`；挂起检测需要内核进程状态查询，属未来工作 | `manager.c:58-60` |
| serial 重启未实现 | 需重绑 PIC 线并重新初始化 16550 | `manager.c:60-64` |
| `wm`/`policy`/`gui` 有 `restartable=1` 但未入监控名单 | `s_restartable[]` 只有 5 项 | `manager.c:132-149` 与 `manager.c:473` |
| 口令散列非生产级 | FNV-1a-64 + 盐，仅完整性校验（树内无密码学库） | `user.h:26-27` |
| ACL 字段仅兼容层 | `vfs_item_info_t` 的 `posix_mode`/`uid`/`gid` 只在 `/System/usr` 兼容层使用，策略留在 user 层 | `vfs.h:84-86` |
| 书签无签名 | `vfs_bookmark_t.mac[16]` 在 Phase 2 恒为 0，Phase 3 才引入 HMAC | `vfs.h:319-322`、`vfs.h:340` |
| `scope_hash` 未生效 | CHECK 请求里保留该字段，但 `do_check` 当前忽略 | `perm.h:136-138` |
| 预留 opcode | `PERM_OP_CONTEXT` / `FREQ` / `POLICY_SAVE` / `POLICY_LOAD` / `AUDIT` 属 P3/P4 预留接口，当前只维护最小状态 | `perm.h:298-305`、`:317-323`、`:340-348`、`:366-372` |
| 客户端端口缓存不重解析 | `libfs` / `libpkg` / `libwm` 缓存端口号，服务重启后需重新解析 | `fs.c:82-89`、`pkg.c:50-56`、`wm.c:24-32` |
| TCP 单连接 | 协议栈一次只维护一条已建立连接 | `net.h:57` |
| net Rx 为轮询 | QEMU pcnet INTx 会风暴电平触发的 8259，故禁用 INTx | `net/main.c:23-25`、`net/main.c:450-457` |
| PCI 为启动快照 | 设备管理器不重扫总线 | `device_mgr.c:61-62` |
| pkg 容量上限 | 单 manifest 最多 8 个原子、最多 8 个已安装应用、4 个并发 RUN 握手 | `pkg.h:63-65` |
| perm 表容量上限 | 授权 64 条、查询 16 条、角色 64 条、规则 96 条 | `perm-manager.c:82-85` |

---

## 12. 附：快速索引

| 想做的事 | 看哪里 |
| --- | --- |
| 查某个服务注册了什么端口 | `user/services/<dir>/<file>.c` 里搜 `PortRegister(` |
| 查某服务的 opcode | 对应 `*.h` 的 `enum { *_OP_* }` 或 `#define *_OP_*` |
| 查 manager 的启动顺序 | `user/services/manager/manager.c` 的 `main()`（491-671 行） |
| 查重启策略 | `manager.c:119`（`MAX_RESTARTS`）、`manager.c:440-465`（`ServiceMonitor`）、`manager.c:472-480`（监控名单） |
| 查内核 IPC 上限与令牌语义 | `kernel/include/kernel/types.h:125`、`kernel/ipc/ipc.c:20-72` |
| 查发送方身份如何取得 | `kernel/ipc/ipc.c:210-216`、`kernel/ipc/ipc.c:438-505` |
| 查管理原子播种名单 | `kernel/syscall/process_desc.c:311-317` |
| 查权限判定顺序 | `user/services/perm/perm-manager.c:778-851` |
| 查被保护的系统关键服务名单 | `user/services/user/main.c:574-577`、`user/services/user/main.c:749-751` |

> 返回 [文档索引](README.md)

