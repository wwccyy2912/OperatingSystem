# OpSys 常见问题（FAQ）

> 适用版本：OpSys v0.8-dev（git HEAD 105805d）　|　最后更新：2026-09-19
>
> 本文按「概念设计 / 构建环境 / 运行使用 / 故障排查」四类整理 26 个高频问题；每个回答先给结论，再给代码或文档出处（形如 `kernel/ipc/ipc.c:120-160`），并尽量给出可直接复制的命令。

| 分类 | 问题数 | 适合谁读 | 章节 |
| --- | --- | --- | --- |
| A 概念与设计 | 8 | 想理解「为什么不那样做」的读者 | [A](#a-概念与设计为什么不这样做) |
| B 构建与环境 | 6 | 第一次在本机构建 ISO 的人 | [B](#b-构建与环境) |
| C 运行与使用 | 7 | 已经在跑 shell、想用起来的人 | [C](#c-运行与使用) |
| D 故障排查 | 5 | 启动失败 / 卡死 / 看不见输出的人 | [D](#d-故障排查) |

> 安装步骤（克隆、装依赖、make iso、make run）不在这里重复，见 [README.md](../README.md) 的「安装指南」与「使用方法」；更细的启动脚本参数见 `scripts/run.sh --help`。

---

## A. 概念与设计（为什么不这样做）

### Q: 为什么是微内核？内核里到底留了什么？

**结论**：OpSys 是「功能型微内核（pragmatic microkernel）」——保留性能关键与安全关键机制，能安全外移的全在 Ring 3。`docs/microkernel_audit.md` 对每个子系统逐项定案，结论是「无必须迁出的内核模块」，唯一剩下的迁移候选 virtio-blk 也被判定为迁出风险大于收益。

**依据（子系统逐项结论，`docs/microkernel_audit.md:10-23`）**：

| 子系统 | 位置 | 结论 | 理由（引自审计文档） |
| --- | --- | --- | --- |
| 进程/线程/调度 | `kernel/sched`、`kernel/process` | 留内核 | 调度与上下文切换是性能与安全核心 |
| 物理/虚拟内存 | `kernel/mm` | 留内核 | 页表操作需特权级；内存是全局资源 |
| 能力系统 | `kernel/cap` | 留内核 | 权限检查不可信用户态 |
| IPC/信号/互斥 | `kernel/ipc` | 留内核 | 内核同步原语（无 futex 类共享内存同步） |
| syscall 层 | `kernel/syscall` | 留内核 | 系统调用边界本身 |
| GDT/IDT/中断 | `kernel/arch/x86_64` | 留内核 | 特权指令；IDT 只能内核写 |
| 帧缓冲 | `kernel/gfx` | 留内核（仅映射/查询） | 渲染全在用户态 term/gui |
| PCI 配置 | `kernel/syscall/pci.c` | 留内核 | 配置空间访问需 I/O 特权 |
| blob 镜像管理 | `kernel/blob` | 留内核 | 与进程创建耦合 |
| panic | `kernel/panic.c` | 留内核 | 崩溃处理必须在内核 |
| virtio-blk | `kernel/arch/x86_64/virtio_blk.c` | 迁移候选② → 保持现状 | DMA/中断/页缓冲都在内核（`docs/microkernel_audit.md:26-34`） |

内核目录与 Makefile 实际编译清单一致：`kernel/{arch/x86_64, mm, sched, ipc, cap, syscall, blob, gfx, process, include/kernel}`（`Makefile:60-96`）。用户态则承载全部驱动与服务（serial/keyboard/term/vfs/fs_mem_driver/fs_virtio_blk_driver/perm/device_mgr/pkg/user/wm/policy/gui/net），由 manager 逐个 `SYS_PROCESS_CREATE` 拉起（`user/services/manager/manager.c:132-149`）。

审计文档同时核对了性能关键路径，确认「留内核」是对的（`docs/microkernel_audit.md:55-64`）：syscall 分发是 O(1) 表查 + 能力检查，线程切换无用户态往返，IPC 调用-应答无中间层，磁盘 DMA 直连。

### Q: 为什么废弃 UID/GID 和 Root，改用 subject_id + 角色 + 原子权限？

**结论**：因为 UID/GID 太粗、且身份在旧设计里是「客户端自报的哈希」，可以随意伪造；`docs/permission_model.md` 把「身份层内核化」定为不可妥协的红线，并用「角色 + 原子权限 + 上下文 + 策略」四层取代 Root。

**依据**：

- 旧模型的两个致命问题（`docs/permission_model.md:29-39`）：IPC 不向接收方暴露发送者身份（`sender_tid` 仅内部唤醒用），且身份是客户端自报的 `app_id_hash`（`SHELL_APP_HASH 0x5E11E5`，无人验证）——任何进程都能冒充任意身份。
- 结论与替代物（`docs/permission_model.md:150-178`）：`subject_id_t = u64`，内核全局唯一、永不重用；`sys_ipc_recv` 增加 `sender_subject` 出参，由内核在入队/交付时填充，用户态不可改。代码实现为 `SYS_GET_SUBJECT(55)` / `SYS_IPC_RECV_FROM(56)`（`kernel/include/kernel/syscall_numbers.h:135-136`），使用点例如 `user/services/keyboard/keyboard.c:628`（焦点所有权就建立在这个不可伪造 subject 上）。
- Root 的消除（`docs/permission_model.md:133-138`）：无全局 Root、无 `sudo`；系统维护走恢复模式，提权必须由 perm-engine 弹窗请求 Owner 确认。角色是 `Owner/Admin/Standard/Child/Guest/Auditor`（`docs/permission_model.md:82-89`），实现在 `user/services/perm/perm-manager.c:1335-1382` 的 `SeedRules()`；原子权限编译成整数枚举 `atom_id_t`（`kernel/include/kernel/atom.h`）。
- 内核侧只留「查表」这一半：`SYS_SET_TIME` 在产生任何副作用之前做 `CapLookupByAtom(..., ATOM_SYS_SET_TIME, 0)`，查不到就 `ERR_NOCAP`（`kernel/syscall/syscall.c:903-921`）。
- 遗留物核对：`kernel/include/kernel/types.h:57-61` 仍保留 `uid_t`/`gid_t`/`UID_ROOT` 三个 typedef，但全仓库已无任何使用点（`grep -rn 'uid_t\|UID_ROOT' kernel user` 只命中这三行）；`cred_t` 路线已废弃（`docs/permission_model.md:36`），`kernel/include/kernel/` 下也不再存在 `cred.h`。

### Q: 为什么 VFS 不用 fd 而用对象句柄 + 安全作用域书签？

**结论**：fd 是「整数下标 + 隐式全局路径空间」，OpSys 走面向对象资源模型（Volume / Item / FileHandle / Enumerator），且**路径字符串只在客户端请求边界出现一次**，服务端内部一律用 `(volume_id, item_id)`；持久授权用不透明书签承载，路径不出服务端。

**依据**：

- 设计目标与对象模型：`docs/vfs_design.md:13-17`（摒弃 POSIX「一切皆 fd」）、`docs/vfs_design.md:59-63`、`docs/vfs_design.md:69-107`（`vfs_uuid_t` / `vfs_item_id_t` / `vfs_resource_t` / `vfs_item_info_t`）。
- FileHandle 的「能力语义」由谁实现：`docs/vfs_design.md:625-644` 的**决策 1** 明确「FileHandle 完全由 vfs_server 用户态管理（内核零改动）」，理由是 `kernel/cap.h` 的 `CAP_TYPE_*` 全部对应内核对象，内核不理解 VFS 语义对象；权限强制点本就在 vfs_server。
- 书签格式与「跨移动仍有效」：`docs/vfs_design.md:346-374`（`vfs_bookmark_t` 含 `resource`/`parent_id`/`access`/`subject_id`/`created_ticks`/`expiry_ticks`，`sizeof = 96`），实现在 `user/services/vfs/vfs.h`（类型定义）与 `user/services/vfs/vfs_server.c`。
- 每次操作重新校验（无授权缓存）：`docs/vfs_design.md:376-384`；代码入口 `PermCheck()`（`user/services/vfs/vfs_server.c:299-353`）。
- 代价与对策：内核 IPC 消息上限 4096 字节（`docs/vfs_design.md:28` 引 `kernel/include/kernel/types.h` 的 `MAX_MSG_SIZE`），所以读操作分块（`VFS_MAX_READ ≈ 4032`），目录枚举固定 8 项/批（`docs/vfs_design.md:437-444`）；`VFS_MAX_DEPTH = 8` 层路径解析。
- 顺带一提：VFS 头文件保留 `posix_mode`/`uid`/`gid` 三个字段，注释写明「/System/usr 兼容层 only」（`user/services/vfs/vfs.h:84-86`），两个驱动都恒写 `posix_mode = 0`（`user/services/vfs/fs_mem_driver.c:497`、`user/services/vfs/fs_virtio_blk_driver.c:680`）——兼容层本身尚未实现（见 Q7）。

### Q: 为什么内核不做策略查询（决策下沉、零 IPC）？

**结论**：因为「syscall 处理器同步 IPC 询问用户态 perm-engine」是微内核的死锁红线。正确形态是：**授予路径异步**（perm-engine 决定后把结论编码进能力），**使用路径同步**（内核纯查表，零 IPC）。

**依据**（`docs/permission_model.md:183-213`）：

1. syscall 处理器常在持有自旋锁/内核状态时运行，阻塞等用户态回复会导致互等；
2. perm-engine 自己也是用户态进程，它执行时必然再调内核服务（内存映射、IPC），形成环；
3. 即便无锁，每次敏感 syscall 一次 IPC 往返的延迟也不可接受。

代码事实：`kernel/include/kernel/syscall_numbers.h:148-152` 的注释直接写着「pure kernel cap-table lookup (决策下沉 — ZERO IPC to user space)」；`sys_set_time` 与 `sys_reboot`/`sys_shutdown` 都是先查表后动作（`kernel/syscall/syscall.c:903-921`、`kernel/syscall/syscall.c:1356-1366`）。查表原语是 `CapLookupByAtom()`（`kernel/cap/cap.c:407`），能力条目带 `expiry_ticks`/`quota` 做惰性过期（`docs/permission_model.md:238-243`）。

验收标准（文档原文）：`sys_set_time` 未授权返回 `EPERM` 且耗时 < 1µs 级，内核代码中不存在任何「syscall → 用户态服务 → 回内核」的调用链（`docs/permission_model.md:212-213`）。

### Q: 为什么信号语义放在 Ring 3（`user/runtime/signal_user.c`）？

**结论**：信号是**低频异常流**，移出内核不损性能，却能把「handler 表 / SIG_IGN / SIG_DFL / 默认动作」这些策略逻辑从 TCB 里彻底拿走。内核只保留「投递机制」。

**依据**：

- 决策记录：`docs/kernel_roadmap.md:110`（POSIX 信号 → Ring 3，前置原语 `SYS_THREAD_SET_CTX`）、`docs/kernel_roadmap.md:186`（D4 定案）；执行记录见 `docs/kernel_roadmap.md:138`「内核 signal.c 仅留投递机制；`sigrestore.S` 删除；`SYS_SIGNAL` 改注册 dispatcher」。
- 内核侧只剩三件事（`kernel/process/signal.c:29-44`）：SIGKILL 直接 `force_exit`；其它信号置 pending 位；在 checkpoint（`signal_check_syscall()` 与 `SignalCheckInterrupt()`）把被中断的用户上下文快照成 `sigframe_t`，改写 RIP/RDI/RSP 跳进 Ring 3 dispatcher；`SYS_SIGRETURN` 把 sigframe 拷回当前 syscall 帧。
- 用户侧实现（`user/runtime/signal_user.c:17-30`）：`s_handlers[NSIG]` 表在用户内存，内核完全不知道；`Signal()` 是纯用户态换表（不产生 syscall）；默认动作表在同一文件 `user/runtime/signal_user.c:70-80`；dispatcher 入口 `__sig_dispatcher()` 在 `:95-111`。
- 一个已知约束：单次投递、不排队（`kernel/process/signal.c:65-71`）；`sigframe_t` 的字段必须与 `kernel/include/kernel/signal.h` 保持同步（`user/runtime/signal_user.c:52-61`）。
- 相关 syscall 编号：`SYS_SIGNAL(48)/SYS_KILL(49)/SYS_SIGRETURN(50)`、`SYS_THREAD_SET_CTX(54)`（`kernel/include/kernel/syscall_numbers.h:115-118,130`）。

### Q: 为什么有 TUI 和 GUI 两套界面层，wm 和 gui 有什么区别？

**结论**：它们不是「两代界面」，而是**两种像素所有权模型**：TUI/wm 是**字符单元（cell）**模型，渲染统一交给 term；gui 是**像素合成**模型，gui 服务直接持有 framebuffer 并为每个窗口维护离屏缓冲。wm 从不映射 framebuffer，gui 则必须持有它。

| 维度 | term（TUI 底座） | wm（v0.4 桌面） | gui（v0.7.1 像素合成器） |
| --- | --- | --- | --- |
| 渲染面 | 8x16 字模 + 9x20 网格，VGA 文本 / Linear RGB 双模式 | 经 term 渲染，焦点窗口最后画（置顶 + `*` 标记） | 直接写 framebuffer（`libgui`），每窗口离屏缓冲 + z-order |
| 是否映射 fb | 是（内核 `SYS_FB_GET_INFO/MAP`，门控 `ATOM_SERVICE_MANAGE`） | 否，纯 IPC 客户端 | 是 |
| 客户端 API | `TERM_OP_*`（libtui 封装） | `wm_create/destroy/list/focus/move/write/...`（libwm） | `GUI_OP_*` FILL/TEXT/POLL/ACTIVATE（libgui） |
| 输入 | 键盘焦点持有者 | `TAKE_FOCUS`，1-9 聚焦 / hjkl 移动 / q 退出 | `TAKE_FOCUS`，鼠标命中测试 + 键盘事件环形缓冲 |
| 空闲态 | 常驻 | 常驻但 idle，直到客户端 ACTIVATE | 常驻但 idle，无 fb 写、无焦点 |

**依据**：`docs/tui_design.md:12-18`（TUI 能力清单）与 `:26-65`（分层：应用 → libtui → `TERM_OP_*` → term → fb）；`user/services/wm/main.c:18-40`（「owns the DESKTOP…renders every window through the term service…never maps the framebuffer itself」）；`user/services/gui/main.c:18-36`（「Owns the display framebuffer (mapped via libgui, gated on `ATOM_SERVICE_MANAGE`)…composes an off-screen window buffer per window」）；framebuffer 门控见 `docs/ops_format.md:136`。

为什么要两套？因为两者的「正确性」定义不同：窗口管理器需要**文本界面的可读性**（shell/Powerbox/文件管理器都在 cell 网格里，Powerbox 面板甚至要能覆盖并原样恢复底层屏幕，见 `docs/tui_design.md:51-53`），而像素 UI 需要**任意图形与鼠标**（`user/services/gui_demo/main.c:18-25`：Keys 回显、Canvas 点击画方块、Info 显示指针位置）。gui 激活时接管键盘焦点（`user/services/gui/main.c:1130-1137`），退出时释放焦点并让 term 重画文本屏（`user/services/gui/main.c:1145-1172`）。

### Q: 为什么不支持 POSIX 全兼容 / fork-exec / mmap 文件？

**结论**：这三种语义在本仓库中**根本没有实现**（不是被开关关掉），设计上用另一套等价机制替代：进程创建走「用户态解析 ELF + 描述符式加载」，执行入口是「新建进程」而非替换自身映像，文件共享走共享页池而非 mmap 文件。POSIX 兼容层只在文档里规划为收敛在 `/System/usr`。

**依据**：

- syscall 全表（`kernel/include/kernel/syscall_numbers.h:26-191`）里没有 `fork` / `execve` / `mmap`；`grep -rn '\bfork\b\|execve' kernel user` 无任何命中。
- 进程创建是描述符式：调用者先用 `user/lib/libos/elf_parse.c` 解析 ELF，把 `proc_image_desc_t` + `proc_seg_desc_t[]` 交给内核；内核只做校验与映射（`kernel/syscall/process_desc.c:15-43`、`:96-113`）。这条路线是 roadmap P1 的产物（`docs/kernel_roadmap.md:136`）。
- `exec` 的真实语义：`exec [blob_name]` 或 `exec <vfs-path>` —— shell 读出整个 ELF 后 `ProcessCreate`（`user/services/shell/shell.c:2338-2345`），父进程不被替换，也不继承地址空间。
- 文件内容的「零拷贝读」用共享页池：`SYS_SHM_CREATE(67)` / `SYS_SHM_MAP(68)` + `VFS_OP_READ_MAP`（`kernel/include/kernel/syscall_numbers.h:177-183`、`docs/kernel_roadmap.md:126`）。mmap 的页错误按需读 / 写回 / `msync` 语义不在其中。
- POSIX 兼容层的现状：`docs/vfs_design.md:620-621` 规划「收敛在 `/System/usr`，仅 CLI 工具经符号链接 + 权限映射访问」，但 `/System/usr` 在 VFS 代码中不存在（`grep -rn 'System/usr' user/services/vfs` 无命中），`posix_mode` 字段被恒置 0（见 Q3）。用户态 C 库本身也只是标准库子集（`README.md:139`）。

### Q: 为什么函数名是 PascalCase 而不是 Linux 风格？

**结论**：这是 2026-09 的一次全仓统一重命名（内核 ~317 个函数、用户态 968 个函数），目的单一：自定义 API 与实现统一微软风格 PascalCase，而 libc/C11 标准名、汇编符号、syscall handler 名按保留集不动。Linux 风格的 `模块_动词_名词` 已废弃（`README.md:346-348` 的贡献规范尚未同步更新，属于过期文档）。

**依据**：

- 提交记录：`4877fcd`（kernel：「函数名统一微软 PascalCase（kernel/ 约 317 个函数）…保留小写：C 入口/库函数、汇编定义符号…、syscall handler 与 `sc_##fn` 适配器（与 token-pasting 宏强耦合）」）；`aa2b97b`（user：「函数重命名（user/ 全目录，968 个函数）」，例 `IpcCall/PortGet/ThreadCreate/FsOpenItem/GuiText/TuiMenu/ImeLookup/ShellMain/TermWrite`）。
- 工具与保留集：`scripts/rename_to_pascal.py:8-16`（安全性说明）、`:17-77`（`RESERVED`：全部 libc/POSIX 名、C 关键字、汇编定义符号如 `context_switch`/`isr_handler`/`syscall_dispatch`）、`:78-86`（`is_reserved()`：`__` 前缀、首字母大写、`sys_`/`sc_sys_` 前缀一律跳过）。
- 残留的 snake_case 是**脚本覆盖不全**，不是双规范：`kernel/cap/cap.c:131` `cap_table_create()`、`:392` `cap_lookup()`、`:468` `cap_get_table()` 都是返回指针的定义，未匹配到脚本的函数定义正则。新代码请统一 PascalCase，且不要顺手改汇编符号或 `sys_*` handler（会与 `sc_##fn` token-pasting 宏脱节）。

---

## B. 构建与环境

### Q: 缺 nasm/gcc/grub2-mkrescue/qemu 怎么办？各发行版包名？

**结论**：先 `command -v` 逐个确认再用包管理器补齐；注意 **GRUB 工具在 Fedora 叫 `grub2-mkrescue`、在 Debian/Ubuntu 叫 `grub-mkrescue`**，而 `Makefile:322` 硬编码了前者。

构建实际用到的工具（`Makefile:12-18`）：`nasm`（AS）、`gcc`（CC）、`objcopy`（binutils）、`ld.lld` 或 `ld`（LD，优先 lld）。运行/校验还需要 `qemu-system-x86_64`、`qemu-img`（造 disk.img）、`python3`（`scripts/*.py`）、`gdb`（可选）。

| 工具 | Fedora / RHEL | Debian / Ubuntu | Arch | openSUSE |
| --- | --- | --- | --- | --- |
| nasm | `nasm` | `nasm` | `nasm` | `nasm` |
| gcc / binutils | `gcc`、`binutils` | `gcc`、`binutils` | `gcc`、`binutils` | `gcc`、`binutils` |
| lld（可选，优先使用） | `lld` | `lld` | `lld` | `lld` |
| GRUB 工具 + 字体 | `grub2-tools`（提供 `/usr/share/grub/unicode.pf2`，本机已实测 `rpm -qf` 命中）、`grub2-tools-extra` | `grub-common`、`grub-pc-bin`（EFI 再加 `grub-efi-amd64-bin`） | `grub` | `grub2` |
| ISO 生成 | `xorriso` | `xorriso` | `xorriso` | `xorriso` |
| QEMU / 磁盘工具 | `qemu-system-x86`、`qemu-img` | `qemu-system-x86`、`qemu-utils` | `qemu-system-x86`、`qemu-img` | `qemu-x86`、`qemu-tools` |
| 脚本/调试 | `python3`、`gdb` | `python3`、`gdb` | `python`、`gdb` | `python3`、`gdb` |

**只装了 `grub-mkrescue` 怎么办**（Debian/Ubuntu 常见）：

```bash
command -v grub2-mkrescue || sudo ln -s "$(command -v grub-mkrescue)" /usr/local/bin/grub2-mkrescue
```

`scripts/run.sh` 在启动前只检查 QEMU 是否存在（`scripts/run.sh:70-72`），ISO 缺失时自动 `make iso`（`scripts/run.sh:74-77`），所以更推荐用 `scripts/run.sh` 而不是裸 `make run`。

### Q: 为什么必须用 `-mstack-protector-guard=global`？

**结论**：因为这是**无 TLS 的 freestanding 内核**。GCC 默认把 stack canary 放在 `%fs:0x28`（TLS 槽），而内核从未设置 FS base——那样读到的会是物理地址 `0x28`（实模式 IVT 区），不是自己的金丝雀；`=global` 让 canary 变成一个内核自有全局变量。

**依据**：

- 编译器开关与理由写在 Makefile 注释里（`Makefile:24-31`），内核与用户态两条 flag 集都带这个开关（`Makefile:33`、`Makefile:46`）。
- 内核侧实现：`__stack_chk_guard` 是非零初始化全局（`kernel/arch/x86_64/stack_chk.c:49-50`，注释说明它位于 `.data`，由 GRUB 从 ELF 加载，因此**第一次内核 C 调用起就有效**）；`StackChkRandomize()` 在 `RngInit()` 之后用启动 PRNG 替换哨兵值，并把低字节清零以拦住按字节的字符串溢出（`kernel/arch/x86_64/stack_chk.c:58-63`）；失配时 `__stack_chk_fail()` 直接 `panic("STACK SMASHING DETECTED... ")`（`:70-77`）。
- 用户态侧同理，但种子来自进程内熵（ticks/heap base/PID/栈地址，`user/runtime/stack_chk.c:51-60`），并在 `_init()` 里、任何构造函数之前完成。
- 自测证据：`canarytest` 服务故意溢出 8 字节栈缓冲，用于验证 Ring 3 保护器确实触发（`user/services/canarytest/main.c:18-27`），串口锚点 `STACK SMASHING DETECTED: user stack canary mismatch`（`scripts/smoke_test.py:351`）——看到这行是**测试预期**，不是故障。

### Q: 为什么 `disk.img` 不在版本库？没有它还能启动吗？

**结论**：`disk.img` 是 virtio-blk 的持久化载体（唯一持久面），已被 `.gitignore` 排除，必须本机创建。**没有它就 `make run` 不起来**——QEMU 会直接报错退出（不是内核问题）；若只是不挂载该设备，系统仍能启动，只是没有 `Disk` 卷。

**依据**：

- `.gitignore:25` 忽略 `disk.img`；README 给出创建命令 `qemu-img create disk.img 8M`（`README.md:241-246`）。
- `make run` 的 QEMU 命令行固定带 `-drive file=disk.img,if=none,id=vd,cache=writethrough`（`Makefile:327-333`，`debug` 目标同样，`:338-345`）。实测缺失文件时的报错：

```text
qemu-system-x86_64: -drive file=disk.img,if=none,id=vd,cache=writethrough:
Could not open 'disk.img': No such file or directory
```

- 若设备存在但驱动初始化失败，用户态驱动会「降级存活」而不是拖垮启动：`VbdkDegrade()` 打印原因后空转（`user/services/vfs/fs_virtio_blk_driver.c:1017-1023`），触发点包括找不到 0x1AF4/0x1001 设备、拿不到 `CAP_TYPE_PCI_DEV`、几何参数异常（`:1028-1065`）。首次启动时超级块 magic 缺失会自动格式化（`:1072-1075`）。
- 重建一个全新 8 MiB 镜像等价于「清盘」：`rm -f disk.img && qemu-img create disk.img 8M`，下次启动自动 `VbdkFormat()`；在系统内也可用 `disk format Disk`（shell 要求手输 `YES` 确认，见 `user/services/shell/shell.c:1929-1940`）。
- 持久化验证方法（脚本已实现）：写一个标记文件后经 QEMU monitor `system_reset` 硬复位，再读回（`scripts/smoke_test.py:606-612`，需 `--drive`）。

### Q: `grub2-mkrescue` 报缺 `unicode.pf2` 怎么办？`loadfont` 有什么作用？

**结论**：`loadfont` 是 gfxterm 的前提——**没有字体，gfxterm 起不来，GRUB 静默回落到文本控制台，`gfxpayload=keep` 也就不会生效**。缺字体时装上带字体的 GRUB 包即可（Fedora `grub2-tools`，Debian/Ubuntu `grub-common`）；注意 Makefile 把 stderr 丢进了 `/dev/null`，所以必须手工重跑命令才能看到真实报错。

**依据**：

- 引导配置：`insmod all_video` / `gfxterm` / `serial`，`loadfont /boot/grub/fonts/unicode.pf2`，`set gfxmode=1024x768x32`、`set gfxpayload=keep`，`terminal_output serial gfxterm`（`boot/grub.cfg:4-18`）；第 9-10 行的注释原文就是「gfxterm cannot initialize without a loaded font; without it GRUB silently falls back to the text console and gfxpayload never applies」。
- ISO 打包规则：`grub2-mkrescue --modules="all_video gfxterm gfxterm_background font" --locales="" -o build/opsos.iso build/isodir 2>/dev/null`（`Makefile:322-323`）；ISO 里只放了 `kernel.elf` 与 `grub.cfg`（`Makefile:318-321`），字体是 `grub2-mkrescue` 自动从主机复制进去的——本机实测生成物内确有 `boot/grub/fonts/unicode.pf2`（2394108 字节），主机来源是 Fedora 的 `grub2-tools` 包。

排查与修复：

```bash
# 1) 手工重跑，看到被 2>/dev/null 吞掉的报错
mkdir -p build/isodir/boot/grub
cp kernel.elf build/isodir/boot/kernel.elf
cp boot/grub.cfg build/isodir/boot/grub/grub.cfg
grub2-mkrescue --modules="all_video gfxterm gfxterm_background font" --locales="" \
    -o build/opsos.iso build/isodir
# 2) 确认字体在位
ls -l /usr/share/grub/unicode.pf2
```

如果 `gfxterm` 确实没起来（VGA 变成文本控制台），term 会退回 VGA 文本模式（`0xB8000`），此时 `tools/vga_decode.py` 的 9x20 网格解码结果会是乱码——这本身就是「gfxpayload 没生效」的判据。GRUB 菜单里的 `Diagnostics: videoinfo` 条目可以只用来看图形模式（`boot/grub.cfg:26-29`）。

### Q: 用户态为什么不能 `include kernel/types.h`？

**结论**：因为 `kernel/types.h` 用 **enum** 定义 `OK`/`ERR_*`，而用户态的 `user/lib/libos/syscalls.h` 用同名 **#define 宏**定义它们；同名宏会把枚举声明体展开成非法表达式（例如 `(-3) = -3,`），编译直接失败。这不是路径问题（`Makefile:52` 确实把 `-I kernel/include` 加进了用户态），而是符号冲突。

**依据**：

- 枚举定义：`kernel/include/kernel/types.h:64-75`（`typedef enum { OK = 0, ERR_NOMEM = -1, ... } error_t;`）；宏定义：`user/lib/libos/syscalls.h:39-45`（`#define OK 0`、`#define ERR_NOCAP (-3)` …，注释写明「must match kernel/types.h」）。
- 多处源码注释点名了这个坑：`user/services/manager/manager.c:107-108`、`user/services/serial/serial.c:101-102`、`user/services/term/term.c:99-100`、`user/services/device_mgr/device_mgr.c:71-72`、`user/services/vfs/vfs.h:29-31`。
- 正确做法是「只用共享 ABI 头 + 本地 typedef」：共享头只有 `kernel/syscall_numbers.h`、`kernel/atom.h`、`kernel/pci.h`、`kernel/blk.h`、`kernel/proc_image.h`（见 `user/lib/libos/syscalls.h:28-37`、`user/lib/libos/elf_parse.h:36`），固定宽度类型在本地重新 typedef：

```c
/* 摘自 user/services/manager/manager.c:109-111 */
typedef uint8_t  u8;
typedef uint32_t u32;
typedef int32_t  i32;
```

不要靠调整 include 顺序去绕过：那只是把宏/枚举重定义暂时藏起来，后续任何一次头文件顺序变化都会再炸。

### Q: 构建产物在哪、如何只重编一个文件？

**结论**：`build/` 完全镜像源码树（`build/kernel/mm/pmm.c.o`、`build/user/services/shell/shell.c.o`），可执行产物是根目录的 `kernel.elf`、`build/user/services/*.elf`（每个服务一个 ELF）、`build/<svc>_blob.o`（嵌入内核的 blob）与 `build/opsos.iso`。只改一个 `.c` 时用单文件目标验证编译，再用 `make iso` 重链。

```bash
make build/kernel/mm/pmm.c.o            # 内核单文件（Makefile:238-249 的一般规则）
make build/user/services/shell/shell.c.o # 用户态单文件（Makefile:308-315）
make iso                                # 重链 kernel.elf + 全部服务 ELF + blob + ISO
make clean                              # 只删 build/，不动根目录的 kernel.elf
```

**依据**：目标文件命名与分层规则见 `Makefile:154-166`；服务 ELF 与 blob 规则见 `Makefile:253-305`（每个服务一条 `SVC_LINK_RULE`，blob 用 `ld -r -b binary` + `objcopy --redefine-sym` 生成 `<svc>_elf_start/_end/_size`）；ISO 规则见 `Makefile:317-323`；`clean` 只执行 `rm -rf build`（`Makefile:348-349`）。

增量构建依赖自动生成的头文件依赖：`-MD -MP`（`Makefile:38`、`:54`）配合末尾的 `-include $(KERNEL_C_OBJ:.o=.d) $(USER_OBJ:.o=.d)`（`Makefile:374`），所以改了头文件也会触发正确的重编。

**注意**：内核与用户态都用 `-mcmodel=large`（`Makefile:34`、`:47`），而 `Makefile:246` 的注释仍写着 `-mcmodel=kernel`——以 flag 行为准，注释是旧文案。

---

## C. 运行与使用

### Q: 默认账号密码是什么？忘了密码怎么办？

**结论**：首次启动会自举一个默认账户 `admin/admin`（角色 OWNER）。**账户表只在内存里，没有任何持久化**，所以「忘记密码」的标准解法就是重启系统，account 表会重新 seed 回 `admin/admin`。

**依据**：

- 自举逻辑与提示语：`user/services/user/main.c:959-969`（`AcctCount() == 0` 时创建 `admin`，密码 `"admin"`，角色 `PERM_ROLE_OWNER`，并打印 `user: DEFAULT admin/admin created - CHANGE THE PASSWORD (passwd)`）。
- 登录流程与角色同步：登录把内核签发的 `caller` subject 绑定到账户，并把角色写进 perm-engine（`user/services/user/main.c:192-258`）；OWNER/ADMIN 登录还会额外授予 `ATOM_SYS_SHUTDOWN`（`:260-268`），登出撤销（`:297-299`）。
- 登录命令支持两种输入：`login admin admin`（明文参数）或只输入 `login` 后按提示输入（密码走掩码输入，`user/services/shell/shell.c:3891-3936`）。
- 锁定策略：连续 `USER_MAX_LOGIN_ATTEMPTS`（=5，`user/services/user/user.h:69`）次密码错误会把账户 `disabled = 1` 自动锁定（`user/services/user/main.c:212-225`），锁定后登录直接 `ERR_DENIED`（`:207-211`）。管理员可用 `user_unlock <name>` 解锁，但**不能锁自己、不能锁最后一个 admin**（`:488-528`）；`users` 列表里被锁账户带 `L` 标记（`:551-559`）。
- 无持久化是已知设计阶段：内存卷「重启即失」（`docs/permission_model.md:313-314`），账户表就是 `static user_acct_t s_accts[16]`（`user/services/user/main.c:93-94`），持久化属于 P4 议题（`docs/permission_model.md:419`）。
- 改密码用 `passwd [name]`（`user/services/shell/shell.c:3811`）；密码散列是 FNV-1a-64 + 每账户盐，文档明确为演示级而非生产级存储（`user/services/user/user.h:25-28`）。

### Q: 为什么 shell 输出不在串口上？如何同时看两边？

**结论**：shell（以及所有 `term` 渲染的 TUI 内容）**只画在 framebuffer 上**，串口只承载「服务 libc printf → 内核 `SYS_DEBUG_LOG` → COM1」这条日志通道。要同时看两边，就用「串口写文件 + monitor `screendump` + `tools/vga_decode.py` 把 PPM 解回文本」的组合。

**依据**：

- term 里的串口镜像代码被条件编译关掉：`#ifdef TERM_DEBUG_SERIAL_MIRROR`（`user/services/term/term.c:1038-1057`，注释写着「Disabled by default to avoid racing the user-space serial service」），而 Makefile 从未定义该宏（`Makefile:32-54`）。
- 观测模型在验收记录里有正式描述：「shell 提示符/命令回显/错误只渲染到 VGA linear framebuffer；serial 永不显示 shell 输出」（`docs/ops_format.md:201-208`）。
- 用户态 `printf` 走 `DebugLog()`（`user/lib/libc/stdio.c:24-32`、`:472`），最终落到内核串口；shell 则是把输出 `IpcCall` 给 `term` 端口（`user/services/shell/shell.c:3755-3756`）。

可复制的双通道命令（与 `scripts/smoke_test.py:653-661` 一致）：

```bash
qemu-system-x86_64 -cdrom build/opsos.iso -m 256M \
  -vnc 127.0.0.1:0 \
  -serial file:build/serial.log \
  -monitor unix:/tmp/opsys-mon.sock,server=on,wait=off &
sleep 40
# 经 monitor 抓屏（PPM），再解码成 113x38 文本网格
printf 'screendump /tmp/opsys-screen.ppm\n' | socat - UNIX-CONNECT:/tmp/opsys-mon.sock
python3 tools/vga_decode.py /tmp/opsys-screen.ppm
```

`tools/vga_decode.py` 的约定：term 用 8x16 字模画在 9x20 的单元网格上（1024x768 → 113x38 格），字形为白字（`0xFFFFFF`）配深蓝底（`0x082860`），光标格反色；可 `--font` 指定 `term/font.h`（`tools/vga_decode.py:12-30`）。若拿到的解码结果是整屏乱码，先怀疑 GRUB 没进线性帧缓冲（见 Q12/Q23）。

### Q: 为什么第一次写磁盘卷会弹权限询问？如何用 `perm_answer` / `perm` 命令处理？拒绝会怎样？

**结论**：因为 Standard 角色对 `DATA_DOCS_READ` 有链式 ALLOW、对**写**没有任何链规则，于是落到「默认拒绝 → Powerbox 用户授权流」。默认拒绝时 VFS 会把待决询问推给 term 面板，并返回 `-105 (VFS_ERR_ACCESS)`；用户授权后**重试同一条命令**才会成功。拒绝只会让这一次（以及未授权期间的每次）操作继续返回 `-105`，不会破坏系统状态。

**依据**：

- 默认规则表：STANDARD 只有 `ATOM_DATA_DOCS_READ → ALLOW`，写「没有链规则，落到默认拒绝 → Powerbox」（`user/services/perm/perm-manager.c:1335-1382`，特别是 `:1367-1370`）。
- VFS 侧每次操作都同步问 perm：`PermCheck()` 返回 0 表示覆盖请求 access 的授予存在，否则返回 `VFS_ERR_ACCESS`，且 perm-manager 会同时创建 PENDING 询问并推送 UI（`user/services/vfs/vfs_server.c:299-353`）；询问创建处见 `user/services/perm/perm-manager.c:840-851`（`resp->ret = VFS_ERR_ACCESS`、`AuditAppend(... DENY ...)`、`NotifyUi(q)`）。
- 面板与应答：term 的 `perm.ui` 线程在 PENDING 期间持有键盘焦点，直接按 `y`/`n` 即可（`docs/tui_design.md:81-93`；验收流程描述见 `docs/ops_format.md:201-208`）。命令行方式：

```text
opsys:/$ tee /Volumes/Disk/hello.txt hello-disk-123
tee: open FAILED (-105)
perm: app 0x5e11e5 requests /Volumes/Disk/hello.txt (W) - perm_answer 513 y/n
opsys:/$ perm_query            # 也可用它拿 id/url/access（shell.c:3542-3569）
opsys:/$ perm_answer 513 y
perm_answer: query 513 -> ALLOWED (0)
opsys:/$ tee /Volumes/Disk/hello.txt hello-disk-123
tee: 14 bytes written to /Volumes/Disk/hello.txt
```

（上面的往返顺序与文案来自 `docs/vfs_design.md:538-551` 的 Phase 1 验收记录；命令注册见 `user/services/shell/shell.c:3796-3803`，实现在 `:3491-3523`。）

- 拒绝的后果：`DoAnswer` 在 `allow == 0` 时只把询问标记为 `PERM_QUERY_DENIED` 并返回 0 —— 不写入任何 grant（`user/services/perm/perm-manager.c:895-898`），因此原操作仍是 `-105`；撤销已有授权用 `perm_revoke`，撤销后立即失效（`docs/vfs_design.md:591-592`）。
- 粒度与次数：授权按 `(subject, resource)` 记录，多次 PENDING 需要多轮 `y`（`docs/ops_format.md:231-232`）。
- 门控：应答者必须持 `ATOM_SERVICE_MANAGE`，应用永远拿不到该原子，因此**应用无法自答自己的询问**（`user/services/perm/perm-manager.c:863-874`、`docs/ops_format.md:130`）。shell 在内核的 blob 种子名单里（`kernel/syscall/process_desc.c:314-317` 含 `"shell"`），种子链是 init → manager → shell（`kernel/kernel_main.c:251-268` + `kernel/syscall/process_desc.c:310-330`）。

> 代码事实核对：`user/services/user/main.c:646-647` 的注释仍写着「the shell … is not management-plane (no `ATOM_SERVICE_MANAGE`)」，这与上面的种子名单不一致，属过期注释。

### Q: `.ops` 应用如何打包与安装？

**结论**：格式是「16 字节小端头 + manifest 文本 + 单个 ELF」，主机侧用 `scripts/ops_pack.py` 打包/校验；系统内用 `pkg install <name> [--perms=a,b,c]` 从内核 blob 安装、`pkg run <app_id>` 运行。应用权限**只能由 manifest 声明**，pkg-manager 按声明向应用 subject 签发原子，应用无法自授。

**依据**：

- 布局与校验规则：`docs/ops_format.md:16-29`（magic `0x3153504F`＝`"OPS1"`、version=1、`manifest_len ≤ 512`、`payload_len > 0`；magic 错 → `ERR_INVAL`）；manifest 键与原子名封闭集合见 `:31-57`、`:59-86`（`service.manage`/`cap.grant_self`/`sys.debug` 三个管理面原子禁授）。
- 安装/运行/签名流程：`docs/ops_format.md:87-116`（安装写入 `/Volumes/Users/Apps/<name>/app.ops`；应用启动后 `pkg_ready()` 握手，pkg-manager 用 `ipc_recv_from` + `proc_info_by_subject` 交叉校验 pid 与进程名，再逐个原子签发）。
- 主机侧工具用法（`scripts/ops_pack.py:19-21`）：

```bash
python3 scripts/ops_pack.py pack build/user/services/hello.elf manifest.txt /tmp/hello.ops
python3 scripts/ops_pack.py check /tmp/hello.ops
```

- 系统内（`user/services/shell/shell.c:3671-3722`）：

```text
opsys:/$ pkg install hello
opsys:/$ pkg install sbox_demo --perms=sys.set_time
opsys:/$ pkg list
opsys:/$ pkg run sbox_demo
opsys:/$ pkg remove sbox_demo
```

- 注意：安装到 `/Volumes/Users`（内存卷，重启不保留），并且**首次写 Users 卷会触发 Powerbox**（`docs/ops_format.md:95-99`）。完整验收记录（含 `sbox_demo_noperm` 自授失败）见 `docs/ops_format.md:193-216`。

### Q: 如何退出/关机/重启？`stop` 命令为何要输入密码？

**结论**：`shutdown` / `reboot` 需要 `ATOM_SYS_SHUTDOWN`，而该原子在 **OWNER/ADMIN 登录时**由 user 服务授予调用者 subject —— 未登录时执行只会打印 "syscall failed, system still running"。`stop <svc>` 要密码是因为它是管理面操作：TUI 确认 + 当前账户二次认证 + user 服务侧的 OWNER/ADMIN 复核与关键服务拒绝名单。

**依据**：

- `shutdown` / `reboot` 的门控：`kernel/syscall/syscall.c:1356-1366`（reboot，8042 复位线）与 `:1398-1421`（shutdown，先 ACPI `0x604` S5，失败回落复位线），两者都在任何副作用前检查 `ATOM_SYS_SHUTDOWN`，未命中返回 `ERR_NOCAP`；shell 命令实现在 `user/services/shell/shell.c:2875-2895`，失败时会打印 "reboot: syscall failed, system still running"。
- 原子从哪来：OWNER/ADMIN 登录授予（`user/services/user/main.c:260-268`），登出撤销（`:297-299`）。所以正确顺序是：

```text
opsys:/$ login admin admin
login: ok - 'admin' (OWNER)
opsys:/$ shutdown        # 或 reboot
```

没有账户时的替代做法是走 QEMU 侧：monitor 里 `system_reset`（重启）或 `quit`（关闭 QEMU）——持久化验收就是这么做的（`scripts/smoke_test.py:606-612`）。

- `stop <svc>` 的三步（`user/services/shell/shell.c:4191-4259`）：TUI 确认框 → `USER_OP_WHOAMI` 取当前账户（未登录直接报 "not logged in - run 'login' first"）→ 掩码输入密码走 `USER_OP_VERIFY` → 再发 `USER_OP_STOP`。
- user 服务侧二次校验与关键服务名单（`user/services/user/main.c:574-607`）：`serial`、`term`、`keyboard`、`vfs`、`fs_mem_driver`、`fs_virtio_blk_driver`、`perm`、`manager`、`user` 一律拒绝关闭；`kill` 由持有 `ATOM_SERVICE_MANAGE` 的 user 服务执行，shell 自身没有杀进程能力（`docs/permission_model.md:400-408`）。
- `exit` 只是退出 shell 线程（`user/services/shell/shell.c:2866-2873`）；因为 shell 在 manager 的可重启名单里（`user/services/manager/manager.c:473`），它会被自动拉回来。

### Q: 如何跑 GUI 桌面、TUI 文件管理器、窗口管理器演示？

**结论**：三条路径分别是 `gui`（像素桌面）、`fm [dir]`（TUI 文件管理器）、`exec wm_demo`（字符窗口桌面）；三者都由已在启动时拉起、平时 idle 的服务（`gui` / `wm` / `term`）驱动。

| 演示 | 命令 | 界面 | 退出方式 | 关键实现 |
| --- | --- | --- | --- | --- |
| 像素桌面 | `gui` | gui 合成器 + 三个窗口（Keys/Canvas/Info） | 窗口内按 `q` 或 Esc | `user/services/shell/shell.c:2454-2479` 拉起 `gui_demo`；`user/services/gui_demo/main.c:18-25` |
| TUI 文件管理器 | `fm [dir]` | libtui 菜单 | `q` 取消/退出 | `user/services/shell/shell.c:2081-2117`、注册帮助文本 `:3832` |
| 字符窗口桌面 | `exec wm_demo` | wm 注册表 + term 合成（3 窗口） | `q` 关闭会话 | `user/services/wm_demo/main.c:18-24`；`user/services/wm/main.c:18-40` |
| 其他示例 | `exec hello` / `exec tui_demo` / `exec window_demo` | 各自演示 | 见各服务输出 | blob 名单见 `Makefile:206` |

细节：

- `gui` 命令会阻塞等 `gui_demo` 退出（`ProcessWait`），退出后回到文本 shell；期间 gui 服务已把键盘焦点拿走（`user/services/gui/main.c:1130-1137`），退出时释放焦点并用 `TERM_OP_REDRAW` 让 term 重画文本屏（`:1145-1172`）。
- wm 演示是**字符单元**窗口：焦点窗口最后绘制（视觉上置顶，标题带 `*`），键位 `1-9` 聚焦、`h/j/k/l` 移动、`q` 退出（`user/services/wm/main.c:15-40`）；自动化验证脚本 `scripts/verify_wm.py`（8/8）。
- `fm` 从当前目录开始，`/` 会先列卷（``... (ro)`` 后缀表示只读卷），再逐级进入（`user/services/shell/shell.c:2090-2117`）。
- 需要 `gui`/`wm`/`term` 服务本身在跑：manager 在启动 shell 前依次拉起 `user` → `wm` → `policy` → `shell`（`user/services/manager/manager.c:643-663`），`gui` 与 `net` 同在服务表里（`:145-149`）。

### Q: CJK 输入法如何开启（`ime` 命令 / Ctrl+Space）？

**结论**：shell 内置拼音输入法，`ime on|off` 或 `Ctrl+Space` 切换；开启后输入小写拼音、用空格或数字 `1-9` 选候选并提交汉字（UTF-8 写进行编辑缓冲）。引擎只覆盖 `font_cjk.h` 里存在的码点，因此提交的每个字 term/gui 都能渲染。

**依据**：

- 切换：`KBD_CTRL_SPACE 0x80`（`user/services/shell/shell.c:135-136`），按键处理 `:1232-1233`（`case KBD_CTRL_SPACE: s_ime_on = !s_ime_on;`）；命令实现 `:756-773`（含状态行 `IME on/off (Ctrl+Space toggles; pinyin + Space/digits 1-9)`）。
- 组合态规则：组合期间编辑键被吞掉（`:1031`、`:1286`），任何光标移动/行改写会丢弃组合（`:797-799`）；提交时会打印 `ime: commit '<字>' U+XXXX`（`:743-753`）。
- 键盘服务本身只发 ASCII，没有直接输入中文的通道，所以采用「ASCII 组合 + 候选表」方案（`user/lib/libime/ime.h:19-26`）；候选集受字体覆盖限制（同处注释）。
- term / gui 的 CJK 渲染：gui 侧引入 16x16 汉字字形表（`user/services/gui/main.c:69`），标题栏 UTF-8 解码用 `libc/utf8.h`（`:64`）。

---

## D. 故障排查

### Q: 启动卡在自检 / 出现 `SELFTEST FAILURE` 怎么办？

**结论**：这是 init 的**故意 fail-fast**：任一自检套件计数不齐，init 打印醒目横幅后停止启动后续阶段（内核仍存活，已拉起的服务继续跑）。排查主线是「用串口日志定位是哪个套件失败」，而不是去改测试。

**依据**：

- 行为实现：`user/services/init/main.c:2488-2499`（`!!! SELFTEST FAILURE: %s %d/%d passed !!!` + `init: boot aborted after failed self-check` + `ThreadYield()` 死循环；注释说明「kernel stays alive; services already spawned keep running, but init stops launching further phases」）。
- 检查点顺序：`RunTests()`（经典套件，但**最后**才判失败）→ 拉起 manager → P1 → P2 Gate → P2 VFS → KBD Focus → P3 Crash Recovery → P4 资源耗尽 → P5 零拷贝 → 经典套件判失败（`user/services/init/main.c:2503-2586`）。
- 定位命令（串口锚点与自动化脚本一致，`scripts/smoke_test.py:349-383`）：

```bash
grep -E '=== (Results|P1 Permissions|P2 Gate|P2 VFS|KBD Focus|P3 Crash Recovery|P4|P5)|SELFTEST FAILURE|KERNEL PANIC' build/serial.log
grep -E 'manager: (starting|flaky marked FAILED)|proc: CREATE' build/serial.log
```

常见原因与对策：

1. **某个服务没起来**（端口没注册 → 依赖它的套件失败）：看 `svc term/keyboard/vfs/perm/device_mgr/pkg started`、`serial service ready` 这类行是否齐全（`scripts/smoke_test.py:369-379`）。
2. **blob 与 ELF 不同步**：内核 blob 表上限很小且注册失败会 panic（`docs/kernel_roadmap.md:218`：上限 22 + 注册失败 panic），先 `make clean && make iso` 全量重建。
3. **P3 崩溃恢复失败**：该测试要 kill `pkg` 并等它被自动重启（`user/services/init/main.c:2557-2560`）；若管理器重启策略没生效，日志里不会出现 `restart 1/3`（`user/services/manager/manager.c:456-463`）。
4. **看到 `STACK SMASHING DETECTED` 不要慌**：`canarytest` 服务故意触发它，属预期锚点（`user/services/canarytest/main.c:18-27`、`scripts/smoke_test.py:351`）。

### Q: QEMU 黑屏 / 无输出 / GRUB 不进内核的排查顺序？

**结论**：按「ISO 是否为最新 → 图形后端是否可用 → 串口通道是否通 → GRUB 是否进内核 → 内核是否已起来 → 是否只是 term 没接管屏幕」的顺序二分，每一步都有确定的观测点。

1. **ISO 是否最新**：`make iso`；`build/isodir/boot/kernel.elf` 应与根目录 `kernel.elf` 同源（`Makefile:318-321`）。
2. **有没有图形后端**：QEMU 在无 `DISPLAY` 时会退化为 VNC 服务器（本机实测输出 `VNC server running on ::1:5900`），此时本地终端当然「黑屏」。用 `-vnc 127.0.0.1:0` 接 VNC 客户端，或干脆 `-nographic`；`make debug` 用的就是 `-nographic -serial mon:stdio`（`Makefile:335-345`），需要监控台时按 `Ctrl-A c` 切到 `(qemu)` 提示符执行 `screendump`。
3. **串口通道**：确保没有第二个进程占用它，并优先用文件而不是 stdio 抓证据 —— `-serial file:build/serial.log`（`scripts/smoke_test.py:653-656`）。`make run` 用的是 `-serial stdio` 且额外带 `-d int,cpu_reset,guest_errors`（`Makefile:327-333`），QEMU 的中断日志会和串口输出混在同一终端里（见 Q25）。
4. **GRUB 有没有进内核**：如果 GRUB 菜单出现过但随后无输出，怀疑 `loadfont`/`gfxterm`（回落文本控制台，见 Q12）或 `multiboot2` 加载失败；`boot/grub.cfg` 里有专门的 `Diagnostics: videoinfo` 条目（`boot/grub.cfg:26-29`）可以先确认图形模式。
5. **内核是否起来**：内核早期日志直接裸写 COM1；panic 会画红色 `KERNEL PANIC` 屏并同时在串口留字（`kernel/panic.c:297-308`，panic 路径保留内核裸写见 `docs/kernel_roadmap.md:111`）。若串口一个字都没有，检查内存参数（Makefile 用 `-m 256M`）与 ISO 是否被 GRUB 真正加载。
6. **串口正常但屏幕「黑」**：可能只是 term 尚未接管。term 支持 VGA 文本（`0xB8000`）与 Linear RGB 双模式（`docs/tui_design.md:60-65`）；用 Q16 的 `screendump` + `tools/vga_decode.py` 判断到底是「真的没画」还是「画在了另一种模式里」（解码整屏乱码通常意味着落回了文本模式）。

### Q: 服务崩溃后会自动重启吗？哪些不会，为什么？

**结论**：**只有 5 个服务真正有自动重启**：`perm`、`pkg`、`device_mgr`、`shell`、`user`（外加启动期的 `flaky` 演示）。上限 `MAX_RESTARTS = 3`，用尽后标记 `FAILED` 并停止重试。`serial`/`term`/`keyboard` 与 `vfs`/两个 fs 驱动、`net` 不在重启名单内。

**依据**：

- 服务表里 `restartable` 标志与真正启动 monitor 的名单**不是同一个集合**：表中 `wm`/`policy`/`gui` 也写了 `restartable = 1`（`user/services/manager/manager.c:145-147`），但 `StartServiceMonitors()` 只为 `SVC_PERM, SVC_PKG, SVC_DEVICE_MGR, SVC_SHELL, SVC_USER` 建监控线程（`user/services/manager/manager.c:472-480`）——也就是说**这三个服务实际上没有自动重启**，改代码时别被标志位误导。
- 重启策略：`ServiceMonitor()` 阻塞在 `ProcessWait()`，退出后 `restart_count++` 并重新 spawn，超过 `MAX_RESTARTS(3)` 打印 `manager: %s marked FAILED` 后 `break`（`user/services/manager/manager.c:440-465`）；`flaky` 的监控原地跑在主线程（`:555`），这是启动日志里 `manager: flaky marked FAILED` 的来源（`scripts/smoke_test.py:380-381`）。
- 为什么不重启硬件/状态型服务：`docs/kernel_roadmap.md:216` 与 `user/services/manager/manager.c:151-156` 的注释——只有「自包含、近乎无状态」的服务才适合；`vfs` 与 fs 驱动持有命名空间/挂载状态，需要 `-ESTALE` 重开语义（该语义尚未落地，见 `docs/kernel_roadmap.md:200-201`），`serial`/`term`/`keyboard` 持有 IRQ/端口/framebuffer，重启会打断显示与输入。
- 崩溃清理是干净的：`process_reap` 会销毁死亡进程的端口、唤醒阻塞对端并释放注册名（`docs/kernel_roadmap.md:198-199`），因此重启后的服务能重新 `PortRegister`。
- 演示与验证：`flaky` 服务故意 sleep 后 `return 7`（`user/services/flaky/main.c:18-27`），启动日志里应看到 `restart 1/3 … 3/3` 与最终 `FAILED`。

### Q: 串口大量重复日志、执行缓慢、键盘无响应分别先查什么？

**先查三件事**：

| 症状 | 第一嫌疑 | 判据 / 处理 |
| --- | --- | --- |
| 串口刷重复日志 | `make run` 自带 `-d int,cpu_reset,guest_errors`，QEMU 把每次中断/异常打到 stderr，与 `-serial stdio` 混在同一终端 | `Makefile:327-333`；自己起 QEMU 去掉 `-d`（Q16 的命令行） |
| 同上（另一种） | 服务崩溃-重启循环，每次重启都重打 banner | 日志里 `manager: X exited (code N), restart k/3`（`user/services/manager/manager.c:456-463`）；`flaky` 演示是预期行为（`user/services/flaky/main.c:18-27`） |
| 执行缓慢 | 没有 KVM，纯 TCG 软件模拟 | Makefile 的 QEMU 命令行没有 `-enable-kvm`（`Makefile:327-345`）；本机若存在 `/dev/kvm`，手工加 `-enable-kvm -cpu host`；另外 `-d int` 日志本身也会显著拖慢并刷屏 |
| 键盘无响应 | 键盘焦点被别的会话/面板持有 | 见下 |

**键盘焦点的机制与自救**：`keyboard` 服务维护一个「焦点所有者」`s_focus_owner`，持有焦点时只有该所有者的 park 项会被投递，其它阻塞读取（例如 shell 的 `READ_BLOCK`）一直挂着（`user/services/keyboard/keyboard.c:190-195`、`:315-338`）。会取焦点的客户端：term 的 Powerbox 面板线程（`user/services/term/term.c:1625-1626`、`:1904`）、`wm`（`user/services/wm/main.c:462`）、`gui`（`user/services/gui/main.c:1130-1137`）、`window_demo`（`user/services/window_demo/main.c:113`）。

- `TAKE_FOCUS` 只做一次赋值、**没有所有权校验、也不检查旧所有者是否还活着**（`user/services/keyboard/keyboard.c:587-592`）；`RELEASE_FOCUS` 才要求必须是持有者，否则 `ERR_NOCAP`（`:607-615`）。
- 因此最常见的「键盘死了」是某个会话没正常退出（本该 `q` 退出并释放）。自救顺序：先让当前会话退出（面板答 `y`/`n`，会话按 `q`）；若确实卡住，任何客户端再取一次焦点即可（例如重新 `gui` 或 `exec wm_demo`，退出时它们会释放）。
- 还要排除输入法组合态：组合状态下编辑键会被吞掉（`user/services/shell/shell.c:1031`、`:1286`），执行 `ime off` 清组合（`:757-773`）。
- 若完全没有任何按键到达（连 `Ctrl+Space` 都无效），转到服务层：`ps` 看 `keyboard` 是否还活着，串口里找 `keyboard started (PID=N)`；`keyboard` 不可重启（Q24），必要时只能复位系统。

### Q: 如何用 GDB 断到 Ring 3（`make debug` + `target remote`）？

**结论**：`make debug` 起 QEMU 的 gdbstub 并停在复位点（`-s -S`），GDB 侧 `target remote :1234`。内核有符号表但**没有 DWARF 行号信息**（编译时没加 `-g`），所以只能按符号/反汇编断点；断到用户态要额外 `add-symbol-file` 用户的 ELF 并用**硬件断点**（用户代码页是 R+X，GDB 的软件断点写不进去）。

```bash
make debug                     # 终端 1：ISO + QEMU，gdbstub 监听 1234，暂停等待
```

```gdb
# 终端 2
gdb kernel.elf
(gdb) target remote :1234
(gdb) break KernelMain        # 内核 C 入口（boot.asm 的 extern 符号）
(gdb) continue
# …等 shell 起来后，切到用户态
(gdb) add-symbol-file build/user/services/shell.elf 0x400000
(gdb) hbreak ShellMain        # 必须用硬件断点：用户 .text 是 R+X
(gdb) continue
(gdb) info registers rip
```

**依据与注意事项**：

- `debug` 目标：`-s -S` 且会打印连接提示（`Makefile:335-345`）；`scripts/run.sh --debug` 给出同样的操作提示，但它建议的 `break kernel_main` 是**过期的**（重命名后符号为 `KernelMain`，见 `scripts/run.sh:109-121` 与 `4877fcd` 提交记录）；符号确实存在：`readelf -sW kernel.elf` 可见 `KernelMain`，由 `kernel/arch/x86_64/boot.asm:104,554-555` 调用。
- 用户程序统一链接在 `0x400000`（`scripts/user.ld:11-19`，实测 `readelf -h build/user/services/shell.elf` 入口即 `0x400000`），内核按 `proc_seg_desc_t` 逐段映射并按段保护位建 PTE（`kernel/syscall/process_desc.c:96-113`）。
- 为什么必须 `hbreak`：用户 ELF 强制 W^X —— `.text` 是 `R+X`（`scripts/user.ld:11-14` 的 PHDRS 注释即写明原因），内核只在该段带 `PROT_WRITE` 时才置 `PTE_WRITABLE`（`kernel/syscall/process_desc.c:100-105`），所以 GDB 无法把 `int3` 写进代码页。硬件断点数量有限（x86 通常 4 个），够用但要省着点。
- 所有服务都映射在 `0x400000`：同一个地址可能命中的是不同进程。一次只 `add-symbol-file` 你要调的那个服务的 ELF；用串口里的 `proc: CREATE name=shell entry=0x400000 segs=N`（`kernel/syscall/process_desc.c:267`）判断进程何时出现。
- 想看断点停在哪：`info registers rip` + `x/10i $pc`；内核 panic 路径也可作为观测点（`kernel/panic.c`），历史上有用 GDB 破坏 canary 验证 panic 的记录（`docs/kernel_roadmap.md:134`）。

---

## 附：本文核对过的关键出处

| 主题 | 主要出处 |
| --- | --- |
| 微内核分层与逐子系统结论 | `docs/microkernel_audit.md`、`Makefile:60-96` |
| 权限模型 / 决策下沉 | `docs/permission_model.md`、`kernel/include/kernel/syscall_numbers.h:148-152`、`kernel/syscall/syscall.c:903-921` |
| VFS 对象模型与书签 | `docs/vfs_design.md`、`user/services/vfs/vfs.h`、`user/services/vfs/vfs_server.c:299-353` |
| 信号 Ring 3 化 | `docs/kernel_roadmap.md:110,138,186`、`user/runtime/signal_user.c`、`kernel/process/signal.c:29-44` |
| TUI / wm / gui 三层界面 | `docs/tui_design.md`、`user/services/wm/main.c`、`user/services/gui/main.c` |
| `.ops` 格式与沙盒授权 | `docs/ops_format.md`、`scripts/ops_pack.py` |
| 服务管理器与重启策略 | `user/services/manager/manager.c:118-156,440-480` |
| 账户与退出保护 | `user/services/user/main.c`、`user/services/user/user.h`、`user/services/shell/shell.c:4191-4259` |
| QEMU 观测与回归锚点 | `scripts/smoke_test.py:333-383,653-661`、`tools/vga_decode.py` |

> 返回 [文档索引](README.md)
