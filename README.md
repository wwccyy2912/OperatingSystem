# OpSys

**x86_64 微内核操作系统 —— 机制在内核，策略在用户态**

[![Arch](https://img.shields.io/badge/arch-x86__64-blue)](#三系统架构)
[![Boot](https://img.shields.io/badge/boot-Multiboot2%20%2B%20GRUB2-orange)](#32-启动流程)
[![Kernel](https://img.shields.io/badge/Ring%200-13.5k%20LOC-lightgrey)](#33-内核模块)
[![Services](https://img.shields.io/badge/Ring%203-25%20services-green)](#34-用户态服务)
[![License](https://img.shields.io/badge/license-GPLv3%20%2F%20LGPLv3%20%2F%20CC%20BY%204.0-red)](#十一许可证)

OpSys 是一个从零实现的 64 位微内核操作系统。内核只保留**调度、内存管理、IPC、能力校验**四类最小机制；**全部设备驱动与系统服务**——串口、键盘、PS/2 鼠标、virtio-blk、PCnet 网卡、终端、文件系统、窗口管理器、像素合成器、权限引擎、用户账户、包管理——都以独立进程运行在 Ring 3，通过同步 IPC 消息传递协作。

> **当前状态**：开发线 `v0.8-dev`（内核 v0.7.1 之后叠加 GUI / 网络 / 国际化三大方向）。
> 启动自检 **8 个套件全绿**才允许进入登录界面，任一失败立即 fail-fast 停机并打印醒目横幅。

> **本项目完全由 AI 生成**，人负责方向决策与验收。详见[特别鸣谢](#十二特别鸣谢)。

---

## 目录

- [一、项目概览](#一项目概览)
- [二、功能特性](#二功能特性)
- [三、系统架构](#三系统架构)
- [四、快速开始](#四快速开始)
- [五、Shell 命令速查](#五shell-命令速查)
- [六、项目结构](#六项目结构)
- [七、测试与验证](#七测试与验证)
- [八、开发指南（摘要）](#八开发指南摘要)
- [九、文档索引](#九文档索引)
- [十、版本沿革与路线图](#十版本沿革与路线图)
- [十一、许可证](#十一许可证)
- [十二、特别鸣谢](#十二特别鸣谢)

---

## 一、项目概览

### 1.1 这是什么

OpSys 不是 Linux 的裁剪版，也不是教学玩具内核：它有完整的**用户态服务生态**、一套自研的**基于属性的动态权限模型**、**对象句柄式文件系统**、以及**文本 TUI + 像素 GUI 双界面层**。它可以在 QEMU 上完成「启动 → 自检 → 登录 → 跑应用 → 读写持久化磁盘 → 关机」的完整闭环。

| 维度 | 事实 |
| --- | --- |
| 架构 | x86_64（Long Mode），Multiboot2 + GRUB2 引导 |
| 内核 | Ring 0，13.5k 行 `.c/.S/.asm`（含头文件约 17.1k 行） |
| 用户态 | Ring 3，31.4k 行 `.c/.S` + 6.2k 行头文件，27 个用户态程序（18 个常驻服务/监管进程 + 演示与测试程序）、9 个客户端库 |
| 构建 | GNU Make + GCC + NASM，产物为可引导 ISO（`make iso`） |
| 运行 | QEMU（`make run`），串口日志 + VGA/线性帧缓冲双输出通道 |
| 许可 | Kernel/服务核 GPLv3；libc/Runtime/扩展服务 LGPLv3；文档 CC BY 4.0 |

### 1.2 设计哲学：四条不可回退的红线

这四条决定了这个项目"长什么样"，任何改动都必须遵守（详见 [docs/microkernel_audit.md](docs/microkernel_audit.md)、[docs/kernel_roadmap.md](docs/kernel_roadmap.md)）：

1. **微内核纪律** —— 调度器、PMM/VMM、IPC 通道、能力表、IRQ 转发留在 Ring 0；**任何设备驱动与服务都在 Ring 3**。新增功能默认先问"能不能放用户态"。
2. **身份内核化** —— 权限身份是内核在 IPC 交付时填充的 `subject_id`（u64，不可伪造），彻底取代 UID/GID 与客户端自报的 `app_id_hash`。请求体里的身份字段一律不可信。
3. **决策下沉，零 IPC** —— 敏感系统调用的授权判定是**纯内核能力表查找**（`cap_lookup`），不在 syscall 路径上回调用户态策略服务。策略服务只负责签发能力，不参与每次判定。
4. **无 Root** —— 没有超级用户。权限 = 角色（Role）+ 原子权限（Atom）+ 上下文（Context）+ 策略（Policy）四层合成，外加用户显式授权（Powerbox）。

### 1.3 一句话理解运行时

> 内核不认识"文件"，只认识**能力句柄**和**IPC 端口**。
> 想读文件？先向 `vfs` 端口发消息；`vfs` 再问 `perm` 端口"这个 subject 能读吗"；`perm` 可能弹一个 TUI 窗口问用户；用户答 y，`perm` 调 `cap_grant_to_subject` 把**能力**写进你的能力表；然后 `vfs` 才把数据交给你。

---

## 二、功能特性

### 2.1 Ring 0 微内核

- **物理内存管理（PMM）**：位图 + next-fit 分配游标，页级分配/释放。
- **虚拟内存管理（VMM）**：4 级页表、按需建表、W^X 段权限、用户/内核地址空间分离。
- **调度器**：CFS 风格优先级调度，就绪队列 + 按绝对唤醒 tick 排序的睡眠链表（每 tick 唤醒摊销 O(1)）；PIT 100 Hz 时钟基准。
- **线程与进程**：`MAX_THREADS = 2048`；线程上下文切换（`context_switch.S`）、`ThreadSetCtx` 允许用户态信号蹦床改写被中断线程的保存现场。
- **IPC**：端口（`MAX_PORTS = 256`）+ 4 KiB 消息（`MAX_MSG_SIZE = 4096`）；`send/recv/call/reply` 四原语，`ipc_call` 为同步请求-应答；每个端口带**应答等待链**，支持多客户端并发调用同一服务。
- **能力系统**：每进程能力表（`MAX_CAPS = 1024`，按需分配），条目携带 `type / rights / obj_id / atom_id / subject / expiry_ticks / quota / scope_hash`，支持**惰性过期**与**按 atom+scope 批量吊销**。
- **中断与同步**：IDT（含 #DF/NMI 的 IST 栈）、PIC/PIT、IRQ → 用户态线程的**通知（notification）转发**、内核互斥锁 Fast-Path。
- **安全加固**：栈保护金丝雀（`-fstack-protector-strong` + `-mstack-protector-guard=global`）、W^X 段布局、ASLR（栈/堆/金丝雀）、用户态栈守卫页、ELF 段重叠校验。
- **精简图形原语**：仅保留 framebuffer 的 `FB_GET_INFO` / `FB_MAP`（把物理帧缓冲映射给用户态），**绘制完全在 Ring 3**。

### 2.2 Ring 3 全量服务

常驻进程共 18 个：`init` + `manager` + 16 个由 `manager` 按依赖顺序拉起并监控的服务。另有 9 个演示/测试程序（`hello`、`sbox_demo`、`runtime_demo`、`tui_demo`、`window_demo`、`wm_demo`、`gui_demo`、`crashpeer`、`canarytest`）以 blob 形式随镜像分发，用 `exec` 启动。

| 分类 | 服务 |
| --- | --- |
| 引导与监管 | `init`（PID 1，含启动自检）、`manager`（服务监管 + 崩溃自动重启） |
| 驱动 | `serial`、`keyboard`（PS/2 键盘 + 鼠标 + 焦点所有权）、`net`（PCnet-Fast III）、`device_mgr`（PCI 枚举） |
| 存储 | `vfs`、`fs_mem_driver`（内存卷）、`fs_virtio_blk_driver`（持久化磁盘卷） |
| 系统 | `perm`（权限引擎 + Powerbox）、`user`（账户/登录/退出保护）、`policy`（命令策略 DB）、`pkg`（.ops 包管理与沙盒授权） |
| 界面 | `term`（文本终端 + Powerbox 面板）、`wm`（v0.4 文本窗口管理器）、`gui`（像素合成器） |
| 应用与演示 | `shell`、`hello`、`sbox_demo`、`runtime_demo`、`tui_demo`、`window_demo`、`wm_demo`、`gui_demo` |
| 测试桩 | `flaky`（崩溃/重启验证）、`crashpeer`、`canarytest`（栈溢出自检） |

### 2.3 权限与身份

- **五层权限模型**：身份层 → 角色层 → 原子权限层 → 上下文层 → 策略层。原子权限（Atom）是封闭集合，共 23 个（如 `ATOM_SYS_SHUTDOWN`、`ATOM_DATA_DOCS_READ`、`ATOM_PKG_INSTALL`、`ATOM_SERVICE_MANAGE`）。
- **上下文与频率策略（v1.0 生效）**：主体分前台/后台，**后台主体的默认拒绝不会弹出询问面板**；滚动窗口内拒绝过多会进入**隔离期**（一律拒绝且不弹窗），可手动解除。
- **限时/限作用域授权**：授权记录带 TTL 与 scope，Powerbox 可以发"临时授权"；过期授权惰性回收并留审计。
- **审计与策略快照**：十余类事件（判定、询问、裁决、签发、撤销、角色、上下文、隔离、过期、快照）进环形审计；角色与授权可导出为快照文件并热加载。
- **能力生命周期**：`CapCreateAtom`（带过期/quota/scope）→ `CapConsume`（扣配额，惰性过期）→ `CapRevokeByAtom`（跨全部进程按 atom+scope 批量吊销）；`CapGrantToSubject` 是权限引擎的"决策编码为能力"路径。
- **Powerbox 授权流**：默认拒绝 → `vfs` 询问 `perm` → `perm` 推送到 `term` 的 `perm.ui` 端口渲染询问面板 → 用户 `y/n` → 签发或拒绝。**被拒绝的阻塞命令会返回 `-105 (EACCES)`，需重新执行**。
- **角色与账户**：OWNER / ADMIN / STANDARD / CHILD / GUEST / AUDITOR 六种角色；`user` 服务负责登录、改密、建号、锁定（5 次失败自动锁定）；登录会把内核签发的 subject 绑定到账户，并同步角色到权限引擎（`ROLE_SET`，仅管理面可调用）。
- **退出保护**：`stop <svc>` 需要 TUI 确认 → 当前账户 whoami → 掩码密码 → `VERIFY` → `STOP`，且 `user` 服务会二次校验 OWNER/ADMIN 并拒绝关闭关键服务。
- **沙盒应用**：`.ops` 包 = 4 KiB 头 + manifest 文本 + x86_64 ELF；应用权限由 manifest 声明，`pkg-manager` 按声明签发原子能力，**应用无法自授**。

### 2.4 VFS：对象句柄 + 安全作用域书签

- 抛弃 POSIX 的"一切皆 fd"，采用**面向对象资源句柄**：`Volume` / `Item` / `FileHandle` / `Enumerator`。
- **Security-Scoped Bookmark**：路径永不离开服务端，客户端持有不透明书签 blob；文件被移动/改名后书签依然有效（`parent_id` 追踪）。
- **真实磁盘持久化**：`virtio-blk` 用户态驱动 + 内存卷（RAM）+ 磁盘卷（`disk.img`）双支持，跨重启数据保留。
- **FSKit-lite 驱动协议**：驱动进程主动向 `vfs` 发起 `MOUNT` 握手注册（A1 协议），文件系统实现可插拔。
- **零拷贝读路径**：`ShmCreate` / `ShmMap` 把物理页池只读映射给客户端，大文件读取免逐块拷贝。
- **全操作能力门控**：`open/read/write/move/create_dir/delete/enum` 全部走授权，且支持**能力抹位**（申请 `READ|EXEC` 只会拿到 `READ` 句柄；角色链按**逐位求值**，`READ|WRITE` 会同时拿到两个权限位）。
- **对象模型补完（v1.0）**：按句柄取元数据（`fstat by handle`）、按句柄截断（缩小释放、扩大零填充）、书签**过期强制生效**与续期、移动环检测、保留名/超长名校验、删除后句柄与枚举器统一返回 `VFS_ERR_STALE`、内存卷配额。

### 2.5 双界面层：TUI 与像素 GUI

**TUI（文本层）**

- VGA 文本模式与线性 RGB 帧缓冲双模式；状态栏、边框盒子、任意位置文本渲染、光标控制。
- 交互组件库：掩码输入行、确认框、菜单；配合 `TERM_OP_SNAPSHOT/RESTORE` 实现**非破坏性弹框**（关闭后底层屏幕原样恢复）。
- ANSI 转义序列 + 颜色 cell + 滚动历史（`scroll` 命令翻页）。

**wm（文本窗口管理器，v0.4）**

- 独立服务：窗口注册表（create/destroy/list/focus/move/write，owner subject 门控）+ 经 `term` 渲染的合成器 + 键盘焦点路由。
- `libwm` 客户端库 + `wm_demo` 三窗口桌面（Terminal / Files / Settings）。

**gui（像素合成器，开发线）**

- 每窗口独立像素缓冲 + **脏区合成**（刷新率优化）+ Z 序与焦点 + 标题栏拖动 + 关闭/最大化按钮 + 最小化任务栏。
- 事件按窗口 **owner 隔离**（`gui_event_t.owner`），键盘输入跟随焦点窗口，支持鼠标移动/按键/滚轮与隐式 grab。
- `libgui` 像素绘制库（像素/矩形/直线/边框/blit/文本），支持 32bpp ARGB 与 24bpp BGR 两种帧缓冲格式。
- `gui_demo` 桌面演示：Keys / Canvas / Info 三窗口。

### 2.6 网络

- **PCnet-Fast III (AM79C973)** 用户态驱动（QEMU 兼容性最好的 legacy 网卡），DMA 描述符环收发，PCI 配置空间通过 `SYS_PCI_CFG_READ/WRITE` 访问（受 `CAP_TYPE_PCI_DEV` 门控）。
- **原子门控（v1.0）**：`net` 服务按能力放行——出站操作需 `ATOM_NET_CONNECT`，绑定/监听/接收需 `ATOM_NET_BIND`（OWNER/ADMIN 登录时由 `user` 服务签发），诊断类查询不设门槛。
- **开箱即用的网卡**：`make run` / `make debug` 自 v0.9 起默认附带 PCnet（`-netdev user,id=n0 -device pcnet,netdev=n0`），
  因此 `ip` / `netstat` / `net ping` / `http` 不需要额外参数。若你手工拼 QEMU 命令行，记得自己加上这两个参数，
  否则 `net` 服务会打印 "NIC start FAILED" 后退出（不影响其它功能）。协议与 QEMU 配置细节见 [docs/net_design.md](docs/net_design.md) §七。
- **协议栈**：ARP（缓存表）→ IPv4（校验和）→ ICMP（ping）→ UDP（端口绑定/收发）→ TCP（单连接状态机）。
- `net` 服务的 IPC 协议覆盖 MAC 查询、原始帧收发、静态 IP 配置、ping、UDP、TCP；Shell 提供 `net` 命令族。

### 2.7 国际化

- 全链路 UTF-8：**输入（拼音 IME）→ 行编辑（按码位）→ 渲染（8×16 ASCII + 16×16 CJK 双字模）→ 存储**。
- libc 提供 `utf8.c` / `wchar.c` / `wctype.c` / `uchar.h` / `locale.h` 等宽字符与多字节转换支持。
- `ime` 命令或 `Ctrl+Space` 切换拼音输入法。

### 2.8 自有 C Runtime 与 libc

- **零系统调用 malloc 快路径**（无竞争自旋锁，不触发 syscall）、大小分桶、**就地 realloc 扩展**（吸收后续空闲块，避免 O(n²) 拷贝）。
- **ASLR 堆随机化**（内核在 `[0x70000000, 0x78000000)` 内以 64 KB 粒度随机堆基址，`SYS_GET_HEAP_BASE` 下发）+ 守卫页保护。
- `.init_array` 全局构造 / `.fini_array` 析构 / `atexit` 链。
- **POSIX 风格信号**：`signal` / `kill` / `sigreturn`，语义在 Ring 3（`user/runtime/signal_user.c` 的 dispatcher + handler 表）。
- libc 覆盖 stdio / stdlib / string / ctype / inttypes / time / math / threads / wchar / wctype / complex / fenv / stdatomic / setjmp / tgmath 等头文件族。

### 2.9 系统工具链（v0.9）

一套覆盖"看、改、验、管"的命令行工具，全部经服务端口访问资源，shell 自身不持有设备能力：

| 类别 | 命令 |
| --- | --- |
| 文件系统 | `df`（容量/使用率）、`du`（递归占用）、`cp`（文件/目录树复制）、`touch`、`head`、`hexdump`、`tree`、`wc` |
| 磁盘维护 | `disk list\|info\|sync\|check\|read\|mount\|unmount\|format\|fill` —— 卷详情、刷盘、**只读一致性扫描**、原始扇区转储 |
| 开关机 | `power status\|sync\|off\|reboot\|halt`（+ `poweroff`/`halt`/`restart` 别名）—— **先刷盘再断电**；`halt` 由新增的 `SYS_HALT` 实现（停 CPU、不复位） |
| 服务监管 | `svc list\|status\|start\|stop\|restart` —— 经 `manager` 控制端口，停止时抑制自动重启 |
| 网络 | `ip`（查看/配置地址）、`netstat`、`udp`（bind/send/recv）、`dns`（A 记录）、`http`（HTTP/1.0 GET，走 TCP 主动连接） |

维护类命令统一走**管理员代理链**：shell → `user` 服务（校验 OWNER/ADMIN）→ 目标服务/驱动（再次以原子或能力门控），因此命令行永远拿不到它不该有的权限。

### 2.10 性能与可观测性

- **SYSCALL/SYSRET 快速路径**（v0.7 Track 1）取代 `int 0x80`。
- 用户态服务 ELF 以 **blob** 形式嵌入内核镜像，进程创建 = `BlobGet` + `ProcessCreate`，无需文件系统依赖即可启动（真正的 FS 服务随后才拉起）。
- 启动画面 + 启动自检摘要；串口输出（`printf` 调试通道）与终端输出（`term` 端口）**严格分离**。
- 干净构建 0 新警告（`-Wall -Wextra`），`clang-format` 可校验（`make format-check`）。

---

## 三、系统架构

### 3.1 分层视图

```
┌──────────────────────────────────────────────────────────────────────────┐
│ 应用层      .ops 沙盒应用（manifest 声明权限）· 演示程序 · 文件管理器 fm │
├──────────────────────────────────────────────────────────────────────────┤
│ 服务层      18 个常驻 Ring 3 进程，各自独立地址空间与能力表               │
│  init · manager · serial · keyboard · term · vfs · fs_mem_driver         │
│  fs_virtio_blk_driver · perm · device_mgr · pkg · user · policy          │
│  wm · gui · net · shell · hello · 各 demo / 测试桩                       │
├──────────────────────────────────────────────────────────────────────────┤
│ 客户端库    libipc · libos · libfs · libpkg · libtui · libwm · libgui    │
│             libime ·（libc 标准库）                                      │
├──────────────────────────────────────────────────────────────────────────┤
│ C Runtime   crt0 · malloc · errno · exit/atexit · signal_user            │
├──────────────────────────────────────────────────────────────────────────┤
│ 微内核      Ring 0：Sched · PMM/VMM · IPC · Capability · IRQ · Mutex     │
│             （+ Framebuffer 映射、PCI 枚举、blob 表、RTC/PRNG）          │
└──────────────────────────────────────────────────────────────────────────┘
```

**依赖方向单向向下**：服务只通过 `libos` 的系统调用包装与 `libipc` 的 `IpcCall` 触达内核；服务之间只通过端口名寻址（`PortRegister` / `PortGet`），不共享内存、不直接调用彼此的函数。

### 3.2 启动流程

```
GRUB2 (Multiboot2, gfxmode=1024x768x32, gfxpayload=keep, 加载 unicode.pf2)
  │
  └─ boot.asm         校验 Multiboot2 → 建立分页 → 加载 64 位 GDT → Long Mode
       │
       └─ KernelMain(mboot_info_phys, kernel_phys_start)
            ├─ Stage 1   SerialInit            早期串口控制台
            ├─ Stage 1b  RngInit + 金丝雀随机化（ASLR）
            ├─ Stage 2   解析 Multiboot2（内存映射、帧缓冲信息）
            ├─ Stage 3   GdtInit / IdtInit     GDT、IDT、异常处理
            ├─ Stage 4   PmmInit / VmmInit     物理页 + 4 级页表
            ├─ Stage 5   SchedInit / ThreadInit
            ├─ Stage 6   CapInit / IpcInit / MutexInit
            ├─ Stage 7   SyscallInit           SYSCALL/SYSRET 入口
            ├─ Stage 8   ProcessInit → 创建 PID 1（最小能力集）
            ├─ Stage 9   BlobGet("init") → ElfBootLoad → 映射用户栈（ASLR）
            ├─ Stage 10  TSS.RSP0（syscall 内核栈）+ #DF/NMI IST 栈
            └─ Stage 11  关 PIT、屏蔽全部 IRQ → IRETQ 进入 Ring 3
                 │
                 └─ init (PID 1)
                      ├─ RunTests()              经典 syscall 套件（含 bench/stress）
                      ├─ InitProtocol()          注册 "init" 端口
                      ├─ BlobGet("manager") → ProcessCreate
                      ├─ P1 权限引擎套件  → P2 门控 → P2 VFS 授权
                      ├─ KBD 焦点 → P3 崩溃恢复 → P4 资源耗尽 → P5 零拷贝
                      ├─ 任一套件失败 → BootSelftestFail 停机
                      └─ ALL SELFTESTS PASSED → 进入 idle
                           │
                           └─ manager（用户态监管者，重启策略 MAX_RESTARTS=3）
                                serial → term → keyboard → flaky(重启策略演示)
                                → vfs → fs_mem_driver → fs_virtio_blk_driver
                                → device_mgr → perm → pkg → user → wm → policy
                                → gui → net → shell（最后，此后由 shell 接管提示符）
```

### 3.3 内核模块

| 模块 | 路径 | 职责 |
| --- | --- | --- |
| 平台相关 | `kernel/arch/x86_64/` | `boot.asm`、`context_switch.S`、`syscall_entry.S`、GDT/IDT、串口、PIT/IO、RTC、PRNG、栈金丝雀、virtio-blk（legacy 块设备） |
| 内存 | `kernel/mm/` | `pmm.c`（物理页）、`vmm.c`（页表映射）、`vspace.c`（虚拟区间分配）、`elf_boot.c`、`shm.c`（零拷贝页池）、`rbtree.c` |
| 调度 | `kernel/sched/` | `sched.c`（就绪队列 + 睡眠链）、`thread.c`（TCB）、`thread_ctx.c` |
| 进程 | `kernel/process/` | `process.c`（进程表/回收）、`signal.c`（信号投递与 sigframe） |
| IPC | `kernel/ipc/` | `ipc.c`（端口、call/reply、应答等待链）、`irq.c`（IRQ 转发）、`notify.c`、`mutex.c` |
| 能力 | `kernel/cap/cap.c` | 能力表、`cap_lookup`（rights/atom/expiry/quota/scope 校验）、按 atom 批量吊销 |
| 系统调用 | `kernel/syscall/` | `syscall.c`（分发表 + 全部 handler）、`process_desc.c`（进程镜像描述符）、`pci.c`（PCI 枚举与配置空间） |
| 其他 | `kernel/blob/`、`kernel/gfx/`、`kernel/panic.c`、`kernel/kernel_main.c` | 嵌入 ELF blob 表、framebuffer 映射、panic 与栈回溯 |

### 3.4 用户态服务

| 服务 | 端口名 | 职责 | 可自动重启 |
| --- | --- | --- | --- |
| `init` | `init` | PID 1：启动自检 8 套件、拉起 manager | — |
| `manager` | — | 服务监管：按序拉起、`ProcessWait` 监控、崩溃重启（≤3 次） | — |
| `serial` | `serial` | COM1 16550 驱动，服务全系统的调试输出 | ✗（持有 IRQ4） |
| `term` | `term`、`perm.ui` | 帧缓冲文本终端、TUI 组件、Powerbox 面板、启动画面 | ✗（持有帧缓冲） |
| `keyboard` | `keyboard` | PS/2 键盘 + 鼠标驱动、IRQ1、焦点所有权（TAKE/RELEASE_FOCUS） | ✗（持有 IRQ1） |
| `vfs` | `vfs` | 卷/节点/句柄/枚举器对象模型、书签、挂载、授权转发 | ✗（持有命名空间） |
| `fs_mem_driver` | — | 内存卷驱动（MOUNT 握手挂载到 vfs） | ✗ |
| `fs_virtio_blk_driver` | — | virtio-blk 磁盘卷驱动（持久化） | ✗ |
| `perm` | `perm` | 权限引擎：角色/规则/授权/吊销/Powerbox 查询 | ✓ |
| `device_mgr` | `device_mgr` | PCI 设备枚举与查询 | ✓ |
| `pkg` | `pkg` | `.ops` 安装/列出/运行/卸载，按 manifest 签发原子能力 | ✓ |
| `user` | `user` | 账户、登录/登出、改密、锁定、退出保护校验 | ✓ |
| `policy` | `policy` | 命令策略 DB（角色 × 命令 allow/deny），运行时热更新 | ✗（表中标记可重启，但未进监控名单） |
| `wm` | `wm` | v0.4 文本窗口管理器（注册表 + 合成 + 焦点路由） | ✗（同上） |
| `gui` | `gui` | 像素合成器（窗口缓冲、脏区合成、事件队列） | ✗（同上） |
| `net` | `net` | PCnet 网卡驱动 + ARP/IPv4/ICMP/UDP/TCP 协议栈 | ✗（持有 PCI 设备） |
| `shell` | — | 命令行前端：REPL、行编辑、历史、补全、IME、TUI 工具 | ✓ |

> **"可自动重启"这一列的实际含义**：`s_services[]` 中的 `restartable` 标记与实际监控名单并不完全一致——
> 引导结束后真正启动监控线程（`ProcessWait` + 崩溃重启 ≤3 次）的只有 **`perm` / `pkg` / `device_mgr` / `shell` / `user`** 五个
> （`user/services/manager/manager.c` 的 `StartServiceMonitors()`，`s_restartable[]`）；
> `policy`/`wm`/`gui` 虽在服务表里标了可重启，但尚未注册监控线程；`flaky` 的重启演示发生在**引导阶段**（跑完即 `FAILED`，不再重启）。
> 该差异已在 [docs/developer_guide.md](docs/developer_guide.md) 中如实记录。

> 完整 IPC 协议（opcode 枚举、结构体布局、时序图）见 [docs/service_reference.md](docs/service_reference.md)。

### 3.5 IPC 模型

```
客户端                          内核                        服务端
  │                              │                            │
  ├─ IpcCall(port, req, len) ────┤ 投递到 port 的等待队列      │
  │   （阻塞，挂入应答等待链）    ├──────────────────────────► │ IpcRecvFrom()
  │                              │                            │ ├─ 取 sender subject（内核填充）
  │                              │                            │ ├─ 业务处理（可能再 IpcCall 别的服务）
  │  ◄───────────────────────────┤ ◄──── IpcReply(token) ─────┤
  │  返回 resp + 长度            │   唤醒对应 token 的调用者   │
```

- **同步语义**：`IpcCall` 一次完成"发送请求 + 阻塞等应答"；应答通过 `token` 精确路由回调用者，因此**同一端口可并发服务多个客户端**。
- **消息上限 4096 字节**：所有协议结构体都用 `_Static_assert` 守住这个上限；超限返回 `ERR_INVAL`（P4 自检会验证这一点）。
- **能力不随消息传递**：当前版本能力通过 `CapGrantToSubject` 由权限引擎**定向签发**到目标进程的能力表，而不是随 IPC 消息捎带。

### 3.6 身份与权限流

```
   进程创建                IPC 交付                授权判定              能力签发
┌────────────┐      ┌──────────────────┐    ┌────────────────┐   ┌──────────────────┐
│ 内核分配   │      │ 内核在 recv 时   │    │ perm 引擎：    │   │ CapGrantToSubject│
│ subject_id │─────►│ 填充 sender      │───►│ 角色+规则+     │──►│ 把 atom 能力写进 │
│ （不可伪造）│      │ subject（不从    │    │ 上下文+策略    │   │ 目标进程能力表   │
└────────────┘      │ 请求体读取）     │    │ → 允许/拒绝/   │   └──────────────────┘
                    └──────────────────┘    │   询问用户     │            │
                                            └────────────────┘            ▼
   之后每次敏感操作：内核 cap_lookup() 纯本地查表 ──► 命中即放行（零 IPC）  ┌──────────────┐
                                                                    │ ATOM_DATA_*  │
                                                                    │ expiry/quota │
                                                                    │ scope_hash   │
                                                                    └──────────────┘
```

### 3.7 内存布局（用户进程视角）

| 区域 | 范围 | 说明 |
| --- | --- | --- |
| 程序镜像 | `0x00400000` 起 | `scripts/user.ld` 链接地址；`.text` R+X、`.rodata` R、`.data/.bss` R+W（严格 W^X） |
| 用户堆 | `[0x70000000, 0x78000000)` 内随机 64KB 对齐基址 | 256 MB 上限；`SYS_GET_HEAP_BASE` 下发，malloc 惰性取用 |
| 线程栈 | `[0x90000000, 0x100000000)` | 每个地址空间一个 `ASLR_STACK_BLOCK` 对齐块，线程 TID 决定块内页偏移，互不冲突 |
| init 引导栈 | `[0x04000000, 0x10000000)` | 仅 PID 1 的初始栈（随机页对齐），与线程栈区隔离 |
| PIE 保留区 | `[0x40000000, 0x70000000)` | 为将来 `ET_DYN` 全量 PIE 预留（当前全部 blob 为 `ET_EXEC`） |

---

## 四、快速开始

### 4.1 环境依赖

| 工具 | 用途 | Debian/Ubuntu 安装 |
| --- | --- | --- |
| GCC（C11） | 编译内核与用户态 C | `sudo apt install gcc` |
| NASM | 汇编 `.asm` / `.S` | `sudo apt install nasm` |
| ld.lld 或 ld | 链接内核与用户 ELF | `sudo apt install lld` 或 `binutils` |
| GRUB2 工具 | 生成可引导 ISO | `sudo apt install grub2-common grub-pc-bin xorriso` |
| QEMU x86_64 | 运行与自动化测试 | `sudo apt install qemu-system-x86` |
| Python 3 | 自动化测试脚本 | `sudo apt install python3` |
| GDB（可选） | 内核调试 | `sudo apt install gdb` |
| clang-format（可选） | 代码风格校验 | `sudo apt install clang-format` |

> 详细安装与排错见 [docs/getting_started.md](docs/getting_started.md)。

### 4.2 构建

```bash
git clone <repo-url> OpSys && cd OpSys

make iso           # 全量构建：内核 + 全部用户服务 + GRUB ISO（推荐入口）
make kernel.elf    # 只构建内核 ELF（含嵌入的服务 blob）
make init_user     # 只构建用户态 init ELF
make help          # 列出全部构建目标
make format        # clang-format 统一风格
make format-check  # 校验风格（CI 用）
make clean         # 清除 build/
```

产物：

| 路径 | 内容 |
| --- | --- |
| `kernel.elf` | 内核 ELF（已内嵌全部服务 blob） |
| `build/user/services/*.elf` | 27 个用户态程序各自的 ELF |
| `build/<svc>_blob.o` | 由 ELF 转换的二进制 blob 目标文件 |
| `build/opsos.iso` | GRUB 引导的可启动 ISO |

> 也提供脚本封装：`scripts/build.sh [target]`（带 `-v` 详细输出、`-j N` 并行度）。

### 4.3 运行

```bash
# 1) 首次运行前准备持久化磁盘（8 MB，已在 .gitignore 中）
qemu-img create disk.img 8M

# 2) 运行
make run                 # 构建 ISO 并启动 QEMU（串口直连 stdio）
make debug               # 启动 QEMU + GDB stub（-s -S，端口 1234）
scripts/build.sh iso     # 构建脚本封装（彩色输出、-j N 并行度）
```

> **想留存串口日志**：直接重定向 `make run` 的输出即可，例如
> `make run 2>&1 | tee build/serial.log`（QEMU 是 `-serial stdio`，日志随终端输出一起出来）。
> 注意 `scripts/run.sh --serial` 的实现在已有 `-serial mon:stdio` 的基础上**又追加了一条 `-serial file:...`**，
> 那会绑定到 **COM2**，而系统只往 COM1 输出，因此该文件会保持为空——这是脚本的一个已知缺陷，不要依赖它取证。

> **用 GDB 调试**：`make debug` 后在另一终端执行 `gdb kernel.elf -ex 'target remote :1234'`。
> 断点请打 `KernelMain`（`kernel/kernel_main.c:89`）——`scripts/run.sh --debug` 打印的 `break kernel_main` 是过时提示（函数名已统一为 PascalCase）。
> 注意 Makefile 未加 `-g`，需要源码级单步请自行追加调试信息。

`make run` 实际执行的 QEMU 命令：

```bash
qemu-system-x86_64 \
  -cdrom build/opsos.iso \        # 从 ISO 引导（GRUB → kernel.elf）
  -m 256M \                        # 256 MB 物理内存
  -serial stdio \                  # 串口直连标准输入输出（服务 printf 通道）
  -d int,cpu_reset,guest_errors \  # 记录中断/复位/客户机错误
  -drive file=disk.img,if=none,id=vd,cache=writethrough \
  -device virtio-blk-pci,drive=vd,disable-modern=on   # legacy virtio-blk 持久化卷
```

> 无图形环境（SSH/CI）下同样可用：`term` 服务通过 `SYS_FB_MAP` 拿到 GRUB 提供的**线性帧缓冲**，屏幕内容可用 QEMU monitor 的 `screendump` 截图后用 `tools/vga_decode.py` 解码为文本。

### 4.4 首次启动会看到什么

1. **GRUB 菜单** —— 默认 10 秒超时，选中 `OpSys`（Multiboot2 加载 `kernel.elf`）；另有 `Diagnostics: videoinfo` 条目用于排查显示问题。
2. **串口上的内核启动日志** —— 从 `OpSys kernel starting...` 到 `Transitioning to ring 3...`，逐阶段打印初始化结果与 ASLR 信息。
3. **init 启动自检** —— 8 个套件顺序执行，每项打印 `PASS`，最后输出 `init: ALL SELFTESTS PASSED`。**任一失败会打印 `!!! SELFTEST FAILURE ...` 并停机**。
4. **服务拉起日志** —— manager 依次启动各服务并打印 `MANAGER_OK` 一类锚点。
5. **启动画面 + 登录** —— 默认账户 `admin` / `admin`（角色 OWNER）。登录后提示符形如 `opsys:/$`。
6. **可以开始操作**：

```text
opsys:/$ ps                 # 查看全部进程
opsys:/$ ls /Volumes        # 列出卷（System RAM 卷 / Disk 磁盘卷）
opsys:/$ tee /Volumes/Disk/hello.txt hi   # 首次写磁盘会弹出 Powerbox 权限面板
opsys:/$ perm_answer 1 y    # 授权（或被拒绝后重试该命令）
opsys:/$ gui                # 进入像素桌面（gui_demo）
```

---

## 五、Shell 命令速查

登录后 shell 共注册 **61 条内置命令**（含 `bm_create`/`bm`、`perm_answer`/`perm`、`userlock`/`user_lock` 等别名）。完整参考（参数、示例、权限要求）见 [docs/shell_reference.md](docs/shell_reference.md)。

| 分类 | 命令 |
| --- | --- |
| 会话 | `help` `clear` `exit` `reboot` `shutdown` `uptime` `pid` `free` |
| 进程 | `ps` `kill <pid> [signum]` `stop <svc>` `threads` `mutex` `exec [blob]` |
| 文件 | `ls` `cat` `stat` `tee` `mkdir` `rm` `mv` `cd` `pwd` `fm`（TUI 文件管理器）`fallocate` |
| 权限 | `perm_answer` `perm_query` `perm_revoke` `bm_create` `bm_resolve` `bm_revoke` `cap` |
| 账户 | `login` `logout` `whoami` `passwd` `useradd` `userdel` `user_lock` `user_unlock` `users` |
| 策略/环境 | `policy_set` `policy_dump` `export` `unset` `env` |
| 设备/界面 | `disk <list|info|sync|check|read|mount|unmount|format|fill>` `net <mac|arp|ping|tcp|recv|stats>` `gui` `mouse` `ime` `scroll` |
| 包管理 | `pkg <install|list|run|remove>` |
| 文件工具 | `df` `du` `cp` `touch` `head` `hexdump` `tree` `wc` |
| 电源/服务 | `power <status|sync|off|reboot|halt>` `poweroff` `halt` `restart` `svc <list|status|start|stop|restart>` |
| 网络工具 | `ip [show|mac\|set <addr> [gw]]` `netstat` `udp <bind|send|recv>` `dns <name>` `http <ip> <port> [path]` |

交互能力：命令历史（↑/↓，可编辑）、Tab 补全、UTF-8 行编辑、拼音输入法（`Ctrl+Space`）、cwd 感知提示符、`fm` 全屏文件管理器。

---

## 六、项目结构

```
OpSys/
├── boot/grub.cfg                 # GRUB2 引导配置（Multiboot2、gfxmode、串口）
├── kernel/                       # Ring 0 微内核（GPLv3）
│   ├── arch/x86_64/              # 引导、GDT/IDT、上下文切换、syscall 入口、
│   │                             # 串口、RTC、PRNG、栈金丝雀、legacy virtio-blk
│   ├── mm/                       # PMM · VMM · vspace · ELF 装载 · 共享页池 · rbtree
│   ├── sched/                    # 调度器 · 线程 · 线程上下文
│   ├── process/                  # 进程生命周期 · 信号投递
│   ├── ipc/                      # IPC 通道 · IRQ 转发 · 通知 · 互斥锁
│   ├── cap/                      # 能力系统（rights/atom/expiry/quota/scope）
│   ├── syscall/                  # 系统调用分发 · 进程镜像描述符 · PCI
│   ├── blob/                     # 服务 ELF 的二进制 blob 表
│   ├── gfx/                      # Framebuffer 描述与映射（不做绘制）
│   ├── include/kernel/           # 内核头文件（含 syscall_numbers.h 单一事实源）
│   ├── kernel_main.c · panic.c
│   └── LICENSE                   # GPLv3
├── user/                         # Ring 3 用户态
│   ├── runtime/                  # C Runtime（LGPLv3）：crt0 · malloc · errno ·
│   │                             # exit/atexit · signal_user · stack_chk
│   ├── lib/                      # libc（LGPLv3）+ SDK 客户端库（GPLv3）
│   │   ├── libc/    libos/    libipc/   libfs/  libpkg/
│   │   ├── libtui/  libwm/    libgui/   libime/
│   │   └── font_cjk.h            # 16×16 CJK 字模表
│   └── services/                 # 27 个用户态程序（核 GPLv3 / 扩展 LGPLv3）
│       ├── init/       manager/    serial/    keyboard/  term/
│       ├── vfs/        perm/       device_mgr/ pkg/      user/
│       ├── policy/     wm/         gui/       net/       shell/
│       ├── hello/      sbox_demo/  runtime_demo/ tui_demo/
│       ├── window_demo/ wm_demo/   gui_demo/
│       └── flaky/      crashpeer/  canarytest/
├── docs/                         # 设计文档（CC BY 4.0）
├── scripts/                      # 构建、运行、打包与自动化验收脚本
├── tools/vga_decode.py           # VGA screendump PPM → 文本解码
├── Makefile                      # 统一构建系统
├── disk.img                      # virtio-blk 持久化卷（gitignore）
└── README.md
```

### 6.1 用户态库职责

| 库 | 端口/依赖 | 职责 |
| --- | --- | --- |
| `libc` | 无 | C 标准库子集（stdio/stdlib/string/math/time/wchar/…），`printf` 走串口调试通道 |
| `libos` | 内核 | `syscalls.h` 系统调用包装、`elf_parse`（用户态 ELF 解析）、自旋锁 |
| `libipc` | — | `IpcCall` 等 IPC 原语的客户端封装 |
| `libfs` | `vfs` | VFS 客户端：卷/目录/文件/书签/枚举/零拷贝读 |
| `libpkg` | `pkg` | 包管理客户端（安装/列出/运行/卸载） |
| `libtui` | `term` | 文本 UI：写入、清屏、状态栏、边框盒、定点渲染、光标、快照/恢复、输入组件 |
| `libwm` | `wm` | 文本窗口管理客户端 |
| `libgui` | 帧缓冲 / `gui` | 像素绘制：点/矩形/线/blit/文本（ASCII+CJK） |
| `libime` | 无 | 拼音输入法引擎（码表 + 候选选择） |

### 6.2 脚本与工具

| 文件 | 用途 |
| --- | --- |
| `scripts/build.sh` / `scripts/run.sh` | 构建/运行封装（颜色输出、`--debug`；注意 `run.sh --serial` 的日志文件实际为空，见 §4.3） |
| `scripts/smoke_test.py` | 三轮冒烟测试（R1 基线 / R2 盲区 / R3 压力，`--drive` 启用磁盘持久化） |
| `scripts/accept.py` | Powerbox 书签授权 + 文件移动存活 + 撤销的 10 步验收 |
| `scripts/verify_wm.py` / `verify_window_demo.py` | 文本窗口管理器 / 最小窗口闭环验证 |
| `scripts/verify_users.py` | 账户与退出保护验证（登录/建号/角色门控/二次校验） |
| `scripts/verify_demos.py` | hello / runtime_demo / tui_demo 演示验证 |
| `scripts/ops_pack.py` | `.ops` 包打包与校验 |
| `tools/vga_decode.py` | VGA screendump（PPM）→ 文本解码（8×16 字模、9×20 网格） |
| `scripts/rename_to_pascal.py` | 一次性函数命名统一工具（历史工具，保留备查） |

---

## 七、测试与验证

### 7.1 启动自检（Ring 3 端到端，fail-fast）

`init` 在启动时执行 **8 个套件**，全部通过才继续拉起服务：

| 套件 | 内容 |
| --- | --- |
| 经典 syscall | 端口/能力/内存映射/线程/时间/信号/FPU 切换/IPC 错误路径/对端死亡唤醒/栈金丝雀/堆守卫 + 3 个基准（10 万次 `get_time`、100 万次 yield 往返）+ 2 个压力（1000 线程、10 万次 IPC 往返） |
| P1 权限引擎 | subject 身份、OWNER 自动放行、ROLE_SET 热重载与降权、默认拒绝不弹窗、Powerbox 授权、撤销回退、GRANT 覆盖角色、非管理面拒绝、dump、跨进程签发 |
| P2 syscall 门控 | `set_time` 未授权 → `ERR_NOCAP` / 授权后成功、`reboot` 未授权、`notify` 越权、`debug_getchar` 越权 |
| P2 VFS 授权 | OPEN 未授权拒绝、**能力抹位**、上下文/频率/策略往返、5 个 op 门控 + 枚举生命周期 |
| KBD 焦点 | TAKE_FOCUS / RELEASE_FOCUS 所有权往返（非 owner 释放返回 `ERR_NOCAP`） |
| P3 崩溃恢复 | `kill pkg` → manager 自动重启 → 端口恢复 |
| P4 资源耗尽 | 超过 `MAX_MSG_SIZE` 的 IPC 报文 → `ERR_INVAL`；线程表耗尽 → `ERR_NOMEM` 且可恢复 |
| P5 零拷贝读 | READ_MAP 映射的池数据与分块读逐字节一致（头 + 尾校验） |

### 7.2 宿主机自动化

```bash
python3 scripts/smoke_test.py            # R1 基线 + R2 盲区 + R3 压力
python3 scripts/smoke_test.py --drive    # 追加 virtio-blk 持久化（需 disk.img）
python3 scripts/accept.py                # Powerbox 书签全流程验收
python3 scripts/verify_wm.py             # 窗口管理器
python3 scripts/verify_window_demo.py    # 最小窗口闭环
python3 scripts/verify_users.py          # 账户与退出保护
python3 scripts/verify_demos.py          # 各演示程序
python3 scripts/ops_pack.py pack <elf> <manifest> <out.ops>   # 打包 .ops
```

这些脚本以 **QEMU 双通道观测模型**工作：

- **串口通道** —— 只有服务 `printf` 的调试输出（回归锚点、manager 启动日志）。
- **VGA 通道** —— shell 的提示符/回显/命令输出只在线性帧缓冲上，需要 `screendump` 截图后用 `tools/vga_decode.py` 解码。
- **键盘注入** —— 通过 QEMU monitor 的 `sendkey` 注入 PS/2 扫描码；Powerbox 面板会抢占键盘焦点，因此脚本采用"输入命令 → 等待面板 → `y` → 重新输入命令"的循环。

详细方法与新增测试模板见 [docs/testing_guide.md](docs/testing_guide.md)；历次测试结果见 [docs/test_report.md](docs/test_report.md)。

---

## 八、开发指南（摘要）

> 完整手册（含可复制的最小骨架代码）见 [docs/developer_guide.md](docs/developer_guide.md)。

### 8.1 代码风格

- 缩进 4 空格，K&R 括号，列宽 ≤ 100，UTF-8 + LF（配置见 [.clang-format](.clang-format)）。
- 命名：类型 `_t` 后缀；全局变量 `g_` 前缀，静态变量 `s_` 前缀；宏全大写；**函数名 PascalCase**（如 `PmmAllocPage`、`IpcCall`），libc 保留标准名。
- 文件头统一带 SPDX 许可标识，并在头部注释里写"结构（Structure）/ 工作原理（How it works）/ 目的（Purpose）/ 注意事项（Caveats）"四段式说明。
- 错误处理：返回负错误码，结果通过指针参数输出。

### 8.2 改动前必读的三条纪律

1. **微内核纪律**：新增功能先判断能否放 Ring 3；Ring 0 只接受"机制"。
2. **新增服务共需改五处**：Makefile 的 `USER_C`、`USER_SVC_ENTRY_OBJ`、`SVC_NAMES`、`SVC_LINK_RULE` 四处，
    **外加 `kernel/blob/blob.c` 的 blob 注册表**（`BLOB_REG` 与 `extern` 声明，条目数受 `BLOB_MAX_ENTRIES = 28` 限制，漏改会在开机时 panic）；
    随后在 manager 的 `s_services[]` 登记并决定是否进 `s_restartable[]`。
3. **新增系统调用**：在 `syscall_numbers.h` **追加编号（不重排）** → 写 handler 并注册分发表 → 加能力/atom 门控 → 在 `libos/syscalls.h` 加包装 → 在 init 自检中加测试。

### 8.3 提交前验证流程

```bash
make build/user/services/<svc>/<file>.c.o   # 1) 单文件快速编译验证
make iso                                    # 2) 全量构建（0 新警告）
make run                                    # 3) 启动自检 8 套件全过
make format-check                           # 4) 风格校验
```

禁止格式化的文件：`user/services/vfs/fs_mem_driver.c`（冻结）、`user/services/term/font.h` 与 `kernel/include/kernel/panic_font.h`（生成的字模数据）。

---

## 九、文档索引

**从这里开始** → [docs/README.md](docs/README.md)（完整导航与阅读路线）

| 文档 | 内容 |
| --- | --- |
| [docs/getting_started.md](docs/getting_started.md) | 快速上手：依赖、构建、运行、调试、排错 |
| [docs/architecture.md](docs/architecture.md) | 系统架构总览：分层、启动、IPC、内存、能力 |
| [docs/syscall_reference.md](docs/syscall_reference.md) | 系统调用参考手册（编号、参数、门控、错误码） |
| [docs/service_reference.md](docs/service_reference.md) | 服务与 IPC 协议参考（端口、opcode、结构体、时序） |
| [docs/shell_reference.md](docs/shell_reference.md) | Shell 命令参考 |
| [docs/requirements.md](docs/requirements.md) | 需求规格说明书 |
| [docs/permission_model.md](docs/permission_model.md) | 基于属性的动态权限模型（设计 + 落地记录） |
| [docs/permission_reference.md](docs/permission_reference.md) | 权限引擎参考手册（原子、判定、协议、审计、策略、CLI） |
| [docs/vfs_design.md](docs/vfs_design.md) | VFS 对象模型、书签、驱动协议、分阶段实施 |
| [docs/tui_design.md](docs/tui_design.md) | TUI 设计 + v0.4 窗口管理器 |
| [docs/gui_design.md](docs/gui_design.md) | 像素 GUI 与合成器设计 |
| [docs/net_design.md](docs/net_design.md) | 网络子系统设计（PCnet + 协议栈） |
| [docs/i18n_design.md](docs/i18n_design.md) | UTF-8 / CJK / 输入法国际化设计 |
| [docs/runtime_design.md](docs/runtime_design.md) · [runtime_quick_ref.md](docs/runtime_quick_ref.md) | C Runtime 设计文档与快速参考 |
| [docs/ops_format.md](docs/ops_format.md) | `.ops` 应用包格式与沙盒授权契约 |
| [docs/kernel_roadmap.md](docs/kernel_roadmap.md) | 内核开发方向决策与 Ring 0/3 归属定案 |
| [docs/microkernel_audit.md](docs/microkernel_audit.md) | 微内核化审计报告 |
| [docs/developer_guide.md](docs/developer_guide.md) | 开发者指南（风格、任务手册、骨架代码） |
| [docs/testing_guide.md](docs/testing_guide.md) | 测试与验证指南（自检套件 + 自动化脚本矩阵） |
| [docs/test_report.md](docs/test_report.md) | 历轮测试报告 |
| [docs/faq.md](docs/faq.md) | 常见问题 FAQ |
| [docs/CHANGELOG.md](docs/CHANGELOG.md) | 版本沿革（按提交整理的开发日志） |

---

## 十、版本沿革与路线图

| 阶段 | 内容 | 状态 |
| --- | --- | --- |
| v0.1 | 内核骨架 + init + 串口服务 + IPC | ✅ |
| v0.2 | 能力系统 + 权限模型 P0/P1/P2 地基 | ✅ |
| VFS Phase 0–2 | 对象模型 + 内存卷 + virtio-blk + 书签 + Powerbox | ✅ |
| v0.3 | 包管理器 + `.ops` 沙盒应用 | ✅ |
| v0.4 | 窗口管理器（wm 服务 + libwm + wm_demo 桌面） | ✅ |
| v0.5 | 用户账户/退出保护 + 环境变量 + 命令策略三层架构 | ✅ |
| v0.6.x | 架构优化：性能、安全（W^X/栈保护）、UX、可读性、去冗余 | ✅ |
| v0.7.x | SYSCALL 快速路径、DMA 超时重试、金丝雀自检、终端滚动历史、磁盘工具 | ✅ |
| — | Shell 历史/Tab 补全 + TUI 组件库 + fm 文件管理器 | ✅ |
| v0.8-dev | 像素 GUI 合成器 + PS/2 鼠标 + 网络协议栈 + UTF-8/CJK/IME | ✅ |
| v0.9-dev | 磁盘/开关机/网络工具族 + `SYS_HALT` + manager 控制端口 + libc/Runtime 补全 | 🚧 工作区 |
| v1.0 | 稳定版：多核（SMP）、性能优化、文档与测试体系完备 | 📋 规划 |

详细方向决策与性能预算见 [docs/kernel_roadmap.md](docs/kernel_roadmap.md)、[docs/requirements.md](docs/requirements.md)；逐提交的开发日志见 [docs/CHANGELOG.md](docs/CHANGELOG.md)。

### 已知限制（诚实清单）

- 单核（无 SMP）：`ThreadSetAffinity` 接口已就绪，多核调度未实现。
- 权限模型只覆盖到"服务/应用 → 资源"这一层；没有 POSIX 兼容层、没有 `fork`/`execve`、没有 mmap 文件映射。
- 网络仅支持单连接 TCP（已有主动连接），无重传定时器/拥塞控制/DHCP；`dns` 是 shell 里的最小 A 记录实现。
- `ATOM_NET_BIND`/`ATOM_NET_CONNECT` 已定义但**尚未接入 `net` 服务**，网络 opcode 目前无原子门控。
- `disk info/check/read` 只覆盖 virtio-blk 的 `Disk` 卷；内存卷会明确报告"没有控制面"。
- `power off` 只做「刷盘 → 断电」，不会逐个停止服务。
- 密码存储是 FNV-1a-64 + 加盐的完整性校验，**不是**生产级口令哈希（代码中已明确标注）。
- 挂起/超时检测、部分服务的在线重启（serial/term/keyboard/vfs）仍是未来工作。
- 所有用户程序目前为 `ET_EXEC`（固定 `0x400000`），全量 PIE 已预留地址区间但未启用。

---

## 十一、许可证

本项目采用**多组件许可证模型**，各组件的许可证独立适用：

| 组件 | 许可证 | LICENSE 位置 | 适用范围 |
| --- | --- | --- | --- |
| Kernel | [GPLv3](https://www.gnu.org/licenses/gpl-3.0.html) | [kernel/LICENSE](kernel/LICENSE) | `kernel/` 全部代码 |
| System Core | [GPLv3](https://www.gnu.org/licenses/gpl-3.0.html) | [user/services/LICENSE](user/services/LICENSE) | init · manager · perm · vfs |
| System Extension | [LGPLv3](https://www.gnu.org/licenses/lgpl-3.0.html) | [user/services/LICENSE](user/services/LICENSE) | serial · keyboard · term · device_mgr |
| libc | [LGPLv3](https://www.gnu.org/licenses/lgpl-3.0.html) | [user/lib/LICENSE](user/lib/LICENSE) | `user/lib/libc/` |
| SDK（libos/libipc/libfs/libtui/libwm/libgui/libime/libpkg） | [GPLv3](https://www.gnu.org/licenses/gpl-3.0.html) | [user/lib/LICENSE](user/lib/LICENSE) | `user/lib/` 其余子目录 |
| Runtime | [LGPLv3](https://www.gnu.org/licenses/lgpl-3.0.html) | [user/runtime/LICENSE](user/runtime/LICENSE) | `user/runtime/` 全部运行时 |
| Documentation | [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/) | [docs/LICENSE](docs/LICENSE) | `docs/` 全部文档 |
| Applications | 由作者自行决定 | — | `hello/` · `sbox_demo/` · 各 demo |

> **许可证逻辑**：核心系统组件（内核 + system core + SDK）采用 GPLv3，确保修改回馈社区；扩展组件与 libc/runtime 采用 LGPLv3，允许闭源应用链接；应用层由作者自行选择。

> **已知不一致（如实记录）**：截至当前提交，`kernel/` 与 `user/` 下全部 177 个源文件的 SPDX 文件头**统一写的是 `GPL-3.0-or-later`**，
> 尚未按上表为 libc / runtime / 扩展服务改写成 `LGPL-3.0-or-later`。也就是说：**目录级 LICENSE 与 SPDX 头目前并不一致**，
> 以 LICENSE 文件为意图、以 SPDX 头为现状。修正此项需要一次独立的许可整理提交。

---

## 十二、特别鸣谢

本项目从架构设计到代码实现**完全由 AI 参与完成**，人类负责方向决策、验收与迭代反馈。

### 工具

1. **OpenCode** —— 配合 Big Pickle 模型完成了起步设计与大部分架构。
2. **Trae CN** —— 配合其内置大模型完成代码格式整理与部分缺陷修复。
3. **DeepSeek Harness** —— 配合 DeepSeek 模型承担了操作系统生产环境大部分代码功能的添加与优化。

### AI 大模型

1. **OpenCode Big Pickle**
2. **DeepSeek v4 Flash**
3. **GLM 5.2**
