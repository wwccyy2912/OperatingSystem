# OpSys 开发者指南（参与贡献手册）

> 适用版本：OpSys v0.8-dev（git HEAD 105805d）　|　最后更新：2026-09-19
>
> 带你给 OpSys 提交第一个改动：先核实仓库导览、代码风格、许可与微内核纪律，再用四份可复制的任务手册完成「新增服务 / 新增系统调用 / 新增 Shell 命令 / 新增客户端库」，最后走构建验证、调试与协作流程。

## 一、这份文档怎么用

本文只写**代码事实**，关键结论标注来源（`路径` 或 `路径:行号`）。发现与源码不符时**以源码为准**，并把不符处当文档缺陷修掉。

阅读顺序建议：二（导览）→ 三（风格）→ 五（纪律）→ 对应任务手册（六~九）→ 十（验证）→ 十三（陷阱）。查证某个具体事实时用第十三节的出处列直接跳源码。

## 二、仓库导览与阅读顺序

### 2.1 顶层条目职责

| 目录 / 文件 | 一句话职责 | 来源 |
| --- | --- | --- |
| `kernel/` | Ring 0 微内核全部代码（GPLv3） | `kernel/LICENSE` |
| `user/` | Ring 3 用户态：C 运行时 + 客户端库 + 25 个服务进程 | `user/runtime/`、`user/lib/`、`user/services/` |
| `boot/` | 唯一引导产物：GRUB2 配置（Multiboot2 + gfxterm + serial） | `boot/grub.cfg` |
| `docs/` | 设计与规范文档（CC BY 4.0） | `docs/LICENSE` |
| `scripts/` | 构建/运行封装与宿主机自动化验收脚本（bash + Python + 链接脚本） | `scripts/` |
| `tools/` | 主机侧调试工具，目前只有 `vga_decode.py` | `tools/` |
| `Makefile` / `.clang-format` | 唯一构建入口 / 唯一代码风格事实源 | `Makefile`、`.clang-format` |
| `build/`、`kernel.elf`、`disk.img` | 构建与运行产物，均在 `.gitignore` 中 | `.gitignore` |

### 2.2 内核目录（`kernel/`，13,523 行 `.c/.S/.asm` + 3,557 行头文件）

| 子目录 | 职责 | 代表文件 |
| --- | --- | --- |
| `arch/x86_64/` | 引导、GDT/IDT、上下文切换、syscall 入口、串口、RTC、PRNG、栈金丝雀、legacy virtio-blk | `boot.asm`、`syscall_entry.S`、`idt.c`、`stack_chk.c` |
| `mm/` | PMM（物理页）、VMM（页表）、vspace（虚拟区间）、ELF 引导装载、共享页池、红黑树 | `pmm.c`、`vmm.c`、`shm.c` |
| `sched/` | 就绪队列 + 睡眠链表调度器、线程表、线程上下文改写 | `sched.c`、`thread.c`、`thread_ctx.c` |
| `process/` | 进程生命周期与回收、POSIX 信号投递 | `process.c`、`signal.c` |
| `ipc/` | 端口与 call/reply（应答等待链）、IRQ 转发、通知、内核互斥锁 | `ipc.c`、`irq.c`、`mutex.c` |
| `cap/` | 每进程能力表、`cap_lookup`、atom 查询、按 atom+scope 批量吊销 | `cap.c` |
| `syscall/` | 分发表 + 大部分 handler、描述符式进程创建、PCI 枚举与配置空间 | `syscall.c`、`process_desc.c`、`pci.c` |
| `blob/`、`gfx/` | 服务 ELF 的名字注册表 / framebuffer 描述与映射（**不做绘制**） | `blob.c`、`framebuffer.c` |
| `include/kernel/`、`kernel_main.c`、`panic.c` | 内核头文件（含唯一事实源 `syscall_numbers.h`）／启动分阶段编排／统一 panic 路径 | `types.h`、`cap.h`、`ipc.h` |

### 2.3 用户态目录（`user/`，31,375 行 `.c/.S` + 13,064 行头文件）

| 子目录 | 职责 | 来源 |
| --- | --- | --- |
| `runtime/` | C 运行时：`crt0.S`、`malloc`、`errno`、`exit/atexit`、用户态信号 dispatcher、栈金丝雀 | `user/runtime/` |
| `lib/libc/` | 自有 libc（stdio/stdlib/string/ctype/math/time/threads/wchar/utf8/wctype 等） | `Makefile:105-115` |
| `lib/libos/` | 系统调用包装 `syscalls.c/.h`、用户态 ELF 解析 `elf_parse.c`、用户态自旋锁 `spinlock.h` | `Makefile:117-118` |
| `lib/libipc/` | `IpcConnect(name)` / `IpcRequest(port,...)` 两个便捷封装 | `user/lib/libipc/ipc.c` |
| `lib/libfs/`、`libpkg/` | `vfs` 客户端（卷/句柄/书签/枚举/零拷贝读）、`pkg` 客户端 | `user/lib/libfs/fs.c`、`libpkg/pkg.c` |
| `lib/libtui/`、`libwm/` | 文本 UI 组件（状态栏/边框盒/快照恢复/输入组件）、`wm` 客户端 | `user/lib/libtui/tui.c`、`libwm/wm.c` |
| `lib/libgui/`、`libime/` | 像素绘制库（点/矩形/线/blit/文本）+ 字模、拼音输入法引擎 | `user/lib/libgui/gui.c`、`libime/ime.c` |
| `lib/font_cjk.h` | 16×16 CJK 点阵（GB2312 6763 汉字，按 Unicode 二分查找） | `user/lib/font_cjk.h:19-21` |
| `services/` | 25 个独立服务进程，每目录一个入口文件 + 协议头 | `Makefile:98-152` |

### 2.4 服务清单

目录 25 个：`init manager serial keyboard term shell flaky crashpeer canarytest hello vfs perm device_mgr pkg sbox_demo runtime_demo tui_demo window_demo user wm wm_demo policy gui gui_demo net`。`vfs` 目录内含三个入口（`vfs_server.c` / `fs_mem_driver.c` / `fs_virtio_blk_driver.c`），`net` 含两个（`main.c` + `proto.c`）。`SVC_NAMES` 因此有 27 个镜像名（`Makefile:206`）。

### 2.5 脚本与工具

| 文件 | 行数 | 用途 |
| --- | --- | --- |
| `scripts/build.sh` / `run.sh` / `user.ld` | — | `make` 封装（`-v`／`-j N`）／QEMU 启动封装（`--serial` 落 `build/serial.log`、`--debug` 起 GDB stub）／用户态链接脚本（固定 `0x400000`，W^X 三段 PHDR） |
| `scripts/smoke_test.py` | 727 | 三轮冒烟：串口 + VGA screendump + `sendkey` 键盘注入 |
| `scripts/verify_*.py` | 255/256/203/278 | `wm` / 账户与退出保护 / 最小窗口闭环 / 三类 demo 验证 |
| `scripts/accept.py`、`ops_pack.py`、`rename_to_pascal.py` | 259、232、161 | Powerbox 书签授权十步验收、`.ops` 包打包与校验、一次性命名统一工具（历史工具，保留备查） |
| `tools/vga_decode.py` | 132 | screendump PPM → 文本（8×16 字形、9×20 网格） |

### 2.6 建议阅读顺序

```text
(1) 构建与运行      README.md 第四节   ->  make iso && make run
(2) 启动链路        kernel/arch/x86_64/boot.asm -> kernel/kernel_main.c（Stage 1..11）
(3) syscall 主入口  kernel/include/kernel/syscall_numbers.h -> kernel/syscall/syscall.c:1669-1764
(4) 用户态 ABI      user/lib/libos/syscalls.h -> syscalls.c
(5) 服务端到端      policy/main.c（最短的服务）-> manager.c（编排）-> init/main.c（自检）
(6) 交互与协议      shell/shell.c（REPL）-> vfs/vfs.h（最完整的协议头 + _Static_assert）
```

> 只读 `kernel/` 会误判架构：OpSys 的"策略"全在 Ring 3，内核里找不到文件系统、终端渲染或权限策略的实现。

## 三、代码风格规范

### 3.1 `.clang-format` 逐条核实

| 项目 | 配置值（均在 `.clang-format` 内） | 含义 |
| --- | --- | --- |
| 基础风格 | `BasedOnStyle: LLVM`、`Language: Cpp` | LLVM 基线；C 文件也走 C++ 解析器 |
| 缩进 | `IndentWidth: 4`、`TabWidth: 4`、`UseTab: Never`、`ContinuationIndentWidth: 4` | 4 空格，禁止 Tab；续行再缩进 4 |
| `case` 标签 | `IndentCaseLabels: false` | `case` 与 `switch` 同级 |
| 列宽 | `ColumnLimit: 100` | 硬上限 100 列 |
| 括号风格 | `BreakBeforeBraces: Attach` | K&R（附着式），函数左括号不换行 |
| 短块 / 短函数 / 短枚举 | `AllowShortBlocksOnASingleLine: Empty`、`AllowShortFunctionsOnASingleLine: Empty`、`AllowShortEnumsOnASingleLine: false` | 只有空体允许单行 |
| 短 if / 循环 / case | `AllowShortIfStatementsOnASingleLine: Never`、`AllowShortLoopsOnASingleLine: false`、`AllowShortCaseLabelsOnASingleLine: false` | **`if (x) return;` 必须两行**，一律不许单行 |
| 指针对齐 | `PointerAlignment: Right`、`DerivePointerAlignment: false` | `char *p`（星号贴变量名），不从代码推断 |
| 连续对齐 | `AlignConsecutiveAssignments/Declarations/BitFields/Macros: Consecutive` | 连续赋值/声明/位域/宏的等号与名字对齐 |
| 参数换行 | `BinPackArguments: false`、`BinPackParameters: false` | 一行放不下时**每参一行**，不打包 |
| 允许整体换行 | `AllowAllArgumentsOnNextLine: true`、`AllowAllParametersOfDeclarationOnNextLine: true` | 允许把全部实参/形参挪到下一行 |
| include 顺序 | `IncludeBlocks: Preserve`、`SortIncludes: Never` | **保留原顺序、绝不排序**（freestanding 下有隐藏依赖） |
| 空格 | `SpaceBeforeParens: ControlStatements`、`SpacesInParentheses: false`、`SpaceAfterCStyleCast: false` | `if (` 有空格、`Foo(` 无空格、`(u32)x` |
| 尾随注释 / 空行 | `SpacesBeforeTrailingComments: 1`、`AlignTrailingComments: true`、`MaxEmptyLinesToKeep: 2`、`KeepEmptyLinesAtTheStartOfBlocks: false` | 注释前 1 空格并对齐；块首不留空行 |
| 其它 | `ReflowComments: true`、`AlwaysBreakAfterReturnType: None`、`Cpp11BracedListStyle: true` | 注释可回流；返回类型不单独占行 |
| 换行符 / 编码 | — | 统一 UTF-8 + LF（文件头注释声明） |

### 3.2 命名约定（以当前代码为准）

> **重要更正**：`docs/requirements.md` 第十一节与旧版 README 的「全局函数 `模块名_动词_名词`（如 `pmm_alloc_page`）」**已过时**。提交 `4877fcd`（kernel，约 317 个函数）与 `aa2b97b`（user，968 个函数）已把自定义函数统一为 **PascalCase**。下表为 HEAD `105805d` 实测。

| 类别 | 约定 | 现存实例（已核实） |
| --- | --- | --- |
| 类型 | `_t` 后缀，小写 | `cap_t port_t rights_t error_t`（`kernel/include/kernel/types.h:41-54,64-76`）、`rtc_time_t`（`rtc.h:37`）、`cmd_func_t`（`user/services/shell/shell.h`） |
| 自定义函数 | **PascalCase** | `PmmAllocPages`（`pmm.h:50`）、`SchedGetTicks`（`sched.h:81`）、`IpcSend`（`ipc.h:86`）；用户态 `IpcCall`／`PortGet`／`ThreadCreate` |
| 变量 | snake_case（**未**随函数改名） | `msg_len`、`resp_len`、`restart_count`（`user/services/manager/manager.c:125-130`） |
| 宏 / 常量 | 全大写 + 下划线 | `PAGE_SIZE`、`MAX_MSG_SIZE`、`RIGHT_ALL`、`VFS_IPC_MAX` |
| 文件级静态变量 | `s_` 前缀 | `s_ipc_lock`（`kernel/ipc/ipc.c`）、`s_services`（`manager.c:132`）、`s_cmd_head`（`shell.c:246`） |
| 全局变量 | `g_` 前缀 | `g_ipc_recv_port` 等（`user/services/init/main.c:105-108`）、`g_tss`（`user/lib/libc/threads.c:346`） |
| 汇编标号 | 全局 `_` 前缀、局部 `.` 前缀 | `global _start` / `_start:`（`boot.asm:103,109`）、`.dump_loop:`（`boot.asm:143`） |

**PascalCase 例外清单**（改名脚本刻意保留，不要"顺手统一"）：

| 例外 | 实例 | 保留原因 |
| --- | --- | --- |
| C 入口与 crt 符号 | `main`、`_start`、`_init`、`_fini` | ABI 固定 |
| libc / C11 标准函数 | `printf malloc strtok_r thrd_create mtx_lock cnd_wait timespec_get` | 标准名不可改（`user/lib/libc/threads.c`、`time.c`） |
| 汇编定义 / C 侧 extern 符号 | `context_switch enter_user_mode isr_handler syscall_entry_fast syscall_dispatch signal_check_syscall` | 与 `.S`／`.asm` 强耦合（`kernel/include/kernel/thread.h:195,221`、`syscall.h:56`） |
| syscall handler 与适配器 | `sys_debug_log`、`sc_sys_*` | 与 `SYSCALLn()`／`sc_##fn` token-pasting 宏强耦合（`syscall.c:1572-1610`、`syscall_handlers.h:29-53`） |
| 遗留未改名的指针返回函数 | `cap_lookup cap_table_create process_create process_current thread_current sched_get_current fb_get_info rb_min` | 改名脚本未覆盖 `type *name(` 形式（工具见 `scripts/rename_to_pascal.py`） |

### 3.3 文件头：SPDX 许可头 + 结构图注释

每个源文件顶部是「许可块 + 一行说明 + Copyright + 正文 + 四段式结构注释」。C/H 与 `.ld` 用 `/* */`，NASM 用 `;`，Python/bash/Makefile 用 `#`。模板（照抄，只替换说明与正文）取自 `kernel/ipc/ipc.c:1-47` 与 `user/lib/libfs/fs.c:1-58`：

```c
/*
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details: <https://www.gnu.org/licenses/>.
 *
 * foo.c - One-line description of this file
 * Copyright (c) 2026 OpSys Project
 *
 * Free-form prose: what this file does, the "hard rules" a reader must
 * not violate, and the design-doc section it implements.
 *
 * ------------------------------------------------------------------
 * Structure (component name):
 *   ASCII diagram of the call/data flow, one line per layer.
 * How it works:
 *   The mechanism in 2-4 sentences.
 * Purpose:
 *   Why this file exists / what it owns.
 * Caveats:
 *   Known limits, stale assumptions, things that will bite you.
 * ------------------------------------------------------------------
 */
```

四段式标题固定为 `Structure` / `How it works` / `Purpose` / `Caveats`（由 `4877fcd`、`aa2b97b` 引入，覆盖 30 余个核心文件）。字模数据与极小测试桩通常只有一行说明；新增服务主体、库主体、内核子系统文件建议补齐。

### 3.4 其它硬约定

| 约定 | 要求 | 来源 |
| --- | --- | --- |
| 头文件保护 | `#ifndef KERNEL_XXX_H`／`#define`／`#endif`，`#endif` 后写注释 | `kernel/include/kernel/cap.h:23-24,255`、`user/services/vfs/vfs.h:33-34` |
| 错误处理 | 返回负错误码，结果走指针出参 | `kernel/include/kernel/types.h:64-76` |
| 公共接口注释 | Doxygen 风格（`@param`／`@return`） | `kernel/include/kernel/cap.h:110-135` |
| 公共 IPC 协议头 | 必须带布局与尺寸的编译期断言 | `user/services/vfs/vfs.h:556-560` |
| 固定宽度类型 | 不包含 `kernel/types.h` 时自行 `typedef uint32_t u32;` 等 | `user/services/policy/main.c:66-69` |

## 四、许可与 SPDX

### 4.1 目录 LICENSE 现状（实测）

| 目录 | `LICENSE` 正文 | 行数 | README 声明的适用范围 |
| --- | --- | --- | --- |
| `kernel/` | GNU GPL **Version 3** | 553 | 内核全部代码 |
| `user/services/` | GNU GPL **Version 3** | 553 | System Core（init/manager/perm/vfs）GPLv3；System Extension（serial/keyboard/term/device_mgr）标为 LGPLv3 |
| `user/lib/` | GNU **Lesser** GPL **Version 3** | 681 | libc 标为 LGPLv3；其余 SDK（libos/libipc/libfs/libtui/libwm/libgui/libime/libpkg）标为 GPLv3 |
| `user/runtime/` | GNU **Lesser** GPL **Version 3** | 681 | C 运行时全部 |
| `docs/` | Creative Commons **Attribution 4.0 International** | 305 | `docs/` 全部文档 |
| 仓库根 | 无 `LICENSE` | — | 根目录只有 README 与构建产物 |

### 4.2 SPDX 标识串的**实际写法**（关键）

对 `git ls-files` 全量源码扫描：

| 事实 | 数据 |
| --- | --- |
| 携带 SPDX 标识的文件 | `.c` 85、`.h` 87、`.S` 4、`.asm` 1、`.py` 9、`.sh` 3、`.ld` 2、`Makefile` 1 —— **覆盖率 100%，无遗漏** |
| 出现的许可标识串 | 只有一种：`SPDX-License-Identifier: GPL-3.0-or-later`（181 处） |
| LGPL 标识串 / CC BY 标识串出现次数 | **均为 0**（`docs/*.md` 不带 SPDX 头） |

> **存在不一致，务必知情**：`user/lib/LICENSE` 与 `user/runtime/LICENSE` 是 LGPLv3 正文，README 第十一节也声明 libc/runtime 为 LGPLv3，但**当前没有任何源文件写 LGPL SPDX 标识**——`user/lib/libc/*.c`、`user/runtime/*.c` 的头都是 `GPL-3.0-or-later`（例：`user/lib/libc/ctype.c:3`、`user/runtime/malloc.c:3`）。这是既成事实。

### 4.3 新增文件该加什么头

1. **照抄现有头部**（3.3 模板），SPDX 行写 `SPDX-License-Identifier: GPL-3.0-or-later`——树内唯一有 181 个实例可对照的写法。注释起始符按语言选：NASM `;`（`kernel/arch/x86_64/context_switch.S:1-14`）、链接脚本 `/* */`（`scripts/user.ld:1`）、Python/bash 在 `#!` 行后紧接 SPDX（`scripts/smoke_test.py:1-2`、`run.sh:1-2`）、`Makefile` 用 `#`（`Makefile:1`）。
2. 若确有为 libc/runtime 引入 **LGPL** 文件的意图，**不要自己发明头**：树内没有 LGPL SPDX 参照，且会立刻造成"同一目录两种许可"的分裂。应先作为项目级决定统一（改 `user/lib/LICENSE` 的适用范围说明 + 全目录补齐），再动代码。
3. `docs/` 下的 Markdown 不带 SPDX 头，只受 `docs/LICENSE`（CC BY 4.0）约束。

## 五、微内核纪律（改代码前必读）

### 5.1 Ring 0 / Ring 3 归属表

综合 `docs/kernel_roadmap.md` 第三节「Ring 0 / Ring 3 归属决定（最终版）」与 `docs/microkernel_audit.md` 第一、三节：

| 组件 | 最终归属 | 核心理由 | 状态 |
| --- | --- | --- | --- |
| 调度器 / PMM / VMM | **Ring 0** | 必须特权指令，无替代 | 定案（现状） |
| 中断转发（IRQ → notify） | **Ring 0**（极简） | 查表转发，纳秒级 | 定案（现状） |
| 内核 Mutex / 同步原语 | **Ring 0**（Fast-Path） | 高频；用户态模拟需多次 syscall，开销 3~5 倍 | 定案（推翻纯理论建议） |
| IPC 底层通道、能力表校验 | **Ring 0** | 数据搬运机制；安全检查必经之路 | 定案（现状） |
| ELF 解析 | **Ring 3** | 冷路径；解析攻击面移出 TCB（`kernel/mm/elf_boot.c` 仅留 init 装载） | 已完成 |
| Framebuffer 绘制 | **Ring 3** | 批量搬运，零 syscall + SIMD；内核只留 `FB_GET_INFO`／`FB_MAP` | 已完成 |
| 串口 / 键盘 / 普通驱动 | **Ring 3** | 低频中断，IPC 开销可忽略 | 定案（现状） |
| POSIX 信号语义 | **Ring 3**（库） | 低频异常流；内核只留 checkpoint 投递 | 已完成 |
| 运行时串口日志 | **Ring 3**；panic 保留内核裸写 | 调试时 panic 裸写不经格式化 | 定案 |
| 磁盘 DMA（legacy virtio-blk，627 行）、PCI 配置空间访问 | **Ring 0** | 迁出需把 DMA/中断路径全部下沉，收益小于风险；PCI 配置访问需 I/O 特权（`net` 经 `SYS_PCI_CFG_*`） | 定案（保持现状） |
| Futex 化 Mutex、MMIO 设备映射增强 | Ring 0（**推迟**） | 单核收益有限；Phase 1 配套 | 推迟到 SMP / virtio 阶段 |

`docs/microkernel_audit.md` 第三节结论：当前内核已是**功能型微内核（pragmatic microkernel）分层**——内核保留调度、内存、cap、IPC、中断、驱动硬件访问、syscall；serial/keyboard/term/vfs/net/gui/wm/shell/init 全部是独立进程，通过 IPC + 能力授权交互，互不共享地址空间；无必须迁出的内核模块。

### 5.2 四条不可回退的红线

1. **微内核纪律** —— 调度/PMM/VMM/IPC/cap/IRQ 转发在 Ring 0；**所有设备驱动与服务在 Ring 3**。新增功能先问"能不能放用户态"。
2. **身份内核化** —— 权限身份是内核在 IPC 交付时填充的 `subject_id`（`u64`，不可伪造），取代 UID/GID 与自报的 `app_id_hash`（`kernel/include/kernel/types.h:50-54`；填充路径 `kernel/syscall/syscall.c:479-511`）。请求体里的身份字段一律不可信。
3. **决策下沉，零 IPC** —— 敏感 syscall 的授权是**纯内核能力表查找**，不在 syscall 路径回调用户态策略服务。范例 `kernel/syscall/syscall.c:911-937`：`sys_set_time` 先 `CapLookupByAtom(proc->cap_table, proc->subject_id, ATOM_SYS_SET_TIME, 0)`，未命中直接 `ERR_NOCAP`。
4. **无 Root** —— 权限 = 角色 + 原子权限（Atom）+ 上下文 + 策略四层合成，外加用户显式授权（Powerbox）。

### 5.3 归属决策树

```text
新功能
  +-- 需要特权指令（页表 / CR3 / cli-sti / MSR / IRQ 门）？ --> Ring 0
  +-- 是"每次操作都走"的热路径（锁 / malloc / cap 校验）？ --> Ring 0
  +-- 是安全判定本身（能力表查询 / subject 校验）？ --------> Ring 0
  +-- 其它（策略 / 渲染 / 协议栈 / 文件系统 / 驱动逻辑） --> Ring 3 服务
                                                           + 内核只加"机制" syscall
```

## 六、任务手册 A：新增一个用户态服务

### A.1 步骤总览

```text
(1) user/services/<name>/<name>.c    入口 main() + 服务线程
(2) Makefile 4 处                    USER_C / USER_SVC_ENTRY_OBJ / SVC_NAMES / SVC_LINK_RULE
(3) kernel/blob/blob.c 2 处          extern 声明 + BLOB_REG 注册        <== 最容易漏
    kernel/include/kernel/blob.h     BLOB_MAX_ENTRIES 必须抬高（当前恰好已满 28）
(4) user/services/manager/manager.c  s_services[] + SVC_* 宏 + 拉起顺序 + 监控策略
(5) 注册端口名 + (6) 写协议头        IpcPortCreate() + PortRegister()；op 枚举 + req/resp + _Static_assert
(7) 可选 user/lib/lib<name>/         客户端库（再加一行 USER_C）
```

### A.2 第 1 步：写入口（骨架，可直接复制改名）

范本 `user/services/policy/main.c:256-322`：

```c
/* ... 完整许可块与四段式结构注释见 3.3 模板 ...
 * sensor.c - Example sensor service (ring-3, independent process)
 * Copyright (c) 2026 OpSys Project
 */
#include "sensor.h"                     /* wire protocol (own header) */
#include "../lib/libc/stdio.h"
#include "../lib/libos/syscalls.h"
#include <stdint.h>

typedef uint32_t u32;
typedef int32_t  i32;

/* one op handler: fill resp, then reply to the caller */
static void DoRead(int token, sensor_req_t *req) {
    sensor_resp_t resp;
    resp.ret   = 0;
    resp.value = 42; /* ... real work ... */
    (void)IpcReply(token, &resp, (int)sizeof(resp));
}

/* server thread: create port, register name, serve forever */
static void SensorServerMain(void *arg) {
    (void)arg;
    int port = IpcPortCreate();
    if (port < 0) {
        printf("sensor: ipc_port_create failed (%d)\n", port);
        ThreadExit(1);
    }
    int ret = PortRegister(SENSOR_PORT_NAME, port);
    if (ret < 0) {
        printf("sensor: PortRegister('%s') failed (%d)\n", SENSOR_PORT_NAME, ret);
        ThreadExit(1);
    }
    printf("sensor: port %d registered as '%s'\n", port, SENSOR_PORT_NAME);

    u8 s_req[sizeof(sensor_req_t)]; /* sized for the LARGEST request struct */

    for (;;) {
        int msg_len = (int)sizeof(s_req);
        int token   = 0;
        int r       = IpcRecv(port, s_req, &msg_len, &token);
        if (r < 0) {
            printf("sensor: ipc_recv failed (%d)\n", r);
            continue;
        }
        switch (((sensor_req_t *)s_req)->op) {
        case SENSOR_OP_READ:
            DoRead(token, (sensor_req_t *)s_req);
            break;
        default: {
            i32 err = -2; /* ERR_INVAL */
            (void)IpcReply(token, &err, (int)sizeof(err));
            break;
        }
        }
    }
}

int main(void) {
    printf("sensor: starting\n");
    if (ThreadCreate(SensorServerMain, NULL, 10) < 0) {
        printf("sensor: server thread_create failed\n");
        return 1;
    }
    for (;;)
        ThreadYield();
    return 0; /* unreachable */
}
```

`main()` 里**不要**直接跑 recv 循环（服务就无法并发响应）。规范做法是 `ThreadCreate(ServerMain, NULL, 10)` + 主线程 `ThreadYield()` 空转，与 `user/services/policy/main.c:308-322` 一致。需要不可伪造的调用方身份时用 `IpcRecvFrom(port, buf, &msg_len, &token, &caller_subject)`（`user/services/policy/main.c:274-283`）。

### A.3 第 2 步：Makefile 四处

| # | 位置 | 行号 | 加什么 |
| --- | --- | --- | --- |
| 1 | `USER_C` 列表 | `Makefile:98-152` | `user/services/sensor/sensor.c \` |
| 2 | `USER_SVC_ENTRY_OBJ` | `Makefile:175-203` | `build/user/services/sensor/sensor.c.o \` |
| 3 | `SVC_NAMES` | `Makefile:206` | 追加名字 `sensor` |
| 4 | `SVC_LINK_RULE` | `Makefile:263-288` | `$(eval $(call SVC_LINK_RULE,sensor,sensor/sensor.c.o))` |

**为什么必须四处**：`Makefile:204` 的 `USER_SHARED_OBJ := $(filter-out $(USER_SVC_ENTRY_OBJ), $(USER_OBJ))`——每个服务 ELF = 共享对象 + **自己唯一的**入口对象；漏加第 2 处，该服务的 `main()` 会进共享池并被链进其它所有服务。`Makefile:256-257` 注释明确：`SVC_LINK_RULE` 必须一个服务一条 `$(eval ...)`，多目标写在同一个 recipe 上只会挂到最后一个。链接两个 `.c` 的服务有先例（`net`，`Makefile:290-292`）：写一条显式规则。

### A.4 第 3 步：`kernel/blob/blob.c` 两处（极易漏）

服务 ELF 靠 `ld -r -b binary` + `objcopy --redefine-sym` 变成 `build/<name>_blob.o` 再链进内核（`Makefile:295-305`），但内核**不会自动发现**它们：

| # | 位置 | 行号 | 加什么 |
| --- | --- | --- | --- |
| 1 | extern 符号块 | `kernel/blob/blob.c:47-74` | `extern char sensor_elf_start[], sensor_elf_end[], sensor_elf_size[];` |
| 2 | `BlobInit()` 注册块 | `kernel/blob/blob.c:89-120` | `BLOB_REG("sensor", sensor_elf_start, (u64)sensor_elf_size);` |
| 3 | 表容量 | `kernel/include/kernel/blob.h:31-32` | **必须同时抬高 `BLOB_MAX_ENTRIES`** |

> **容量恰好已满**：`BLOB_MAX_ENTRIES` 定义为 `28`（注释写 "26 services + alias + headroom"），而 `BlobInit()` 实际注册 **28** 条（27 个镜像名 + `sbox_demo_noperm` 别名，`kernel/blob/blob.c:89-120`）。再加一条会触发 `BlobRegister` 的 `ERR_NOMEM`，而 `BLOB_REG` 宏对失败**直接 panic**（`blob.c:83-87`）——漏改这一处的结果是**开机 panic**，不是静默失败（投产加固刻意的 fail-fast，见 `docs/kernel_roadmap.md` 第七之二节）。

### A.5 第 4 步：manager 登记与重启策略

在 `user/services/manager/manager.c` 改三处：

1. **`s_services[]` 表**（`manager.c:132-149`）：追加 `{"sensor", -1, 0, 0},`。字段为 `name / pid / restart_count / restartable`（结构体 `manager.c:125-130`），`pid = -1` 表示未拉起。
2. **`SVC_*` 索引宏**（`manager.c:158-173`）：追加 `#define SVC_SENSOR 16`（当前最大是 `SVC_NET 15`）。**宏值必须等于在 `s_services[]` 中的物理下标**。
3. **拉起顺序（`main()`）**：插入 `ManagerWrite("manager: starting sensor\n"); if (SpawnService(&s_services[SVC_SENSOR], 0) < 0) for (;;) ThreadYield();`

| 重启策略 | 做法 | 适用 | 现存例子 |
| --- | --- | --- | --- |
| 不监控 | `restartable` 写 `0`，且不加入监控数组 | 持有硬件 / IRQ / 命名空间状态，重启需重新握手或撕裂状态 | `serial`（IRQ4）、`term`（帧缓冲）、`keyboard`（IRQ1）、`vfs`、两个 fs 驱动、`net` |
| 自动重启 | `restartable` 写 `1` + 把宏加进 `StartServiceMonitors()` 的 `s_restartable[]`（`manager.c:472-485`） | 无状态 / 自包含，崩溃后可从零重建 | 现为 `{SVC_PERM, SVC_PKG, SVC_DEVICE_MGR, SVC_SHELL, SVC_USER}` |

监控实现：每服务一个 `ThreadCreate(ServiceMonitor, &s_services[i], 10)`，阻塞在 `ProcessWait(svc->pid, &exit_code)`，退出后递增 `restart_count` 并重新 `SpawnService`，达 `MAX_RESTARTS = 3`（`manager.c:119`）后打印 `marked FAILED` 并结束（`manager.c:440-465`）。

> **表格陷阱**：`s_services[]` 的 `restartable` 与实际监控集合**不一致**——`flaky`、`wm`、`policy`、`gui` 的该字段都是 `1`，但它们不在 `s_restartable[]` 中，不会被重启。以 `StartServiceMonitors()` 为准。`manager.c:151-156` 的注释也仍写着"perm/pkg/device_mgr/shell"（漏了后加入的 `user`），属注释滞后。

### A.6 第 5 步：注册端口名

`int port = IpcPortCreate();`（`SYS_IPC_PORT_CREATE`）+ `PortRegister("sensor", port);`（`SYS_PORT_REGISTER`）；客户端用 `PortGet("sensor")`（`SYS_PORT_GET`）解析。两处实现见 `kernel/syscall/syscall.c:942-959`。**端口名全局唯一**，动手前先 `grep -rn 'PortRegister(' user/`。现有名字：`init serial term perm.ui keyboard vfs vfs.fs.mem vfs.fs.virtio_blk perm device_mgr pkg user policy wm gui net`。对端死亡由 `process_reap` → `IpcCleanupProcess` 统一清理（销毁端口、唤醒阻塞对端为 `ERR_NOENT`、回收注册名，见 `kernel/include/kernel/ipc.h`），这是服务能干净重启的前提。

### A.7 第 6~7 步：协议头与客户端库

协议头放服务目录下（`user/services/<name>/<name>.h`），写法见第九节。客户端库只在**多个调用方**时才抽：`user/lib/lib<name>/<name>.c` 加入 `USER_C`，**不加入** `USER_SVC_ENTRY_OBJ`（库不带 `main`）。

### A.8 自检清单

- [ ] `make build/user/services/sensor/sensor.c.o` 单文件编译通过、零警告
- [ ] `make iso` 全量 0 新警告；`make run` 里出现 `manager: sensor started (PID=...)` 与 `sensor: port N registered as 'sensor'`
- [ ] `kernel/blob/blob.c` 已登记且 `BLOB_MAX_ENTRIES` 已抬高
- [ ] shell 的 `ports` 命令能看到新端口（`CmdPorts`；端口表见 `user/services/shell/shell.c:3763` 起的注册块）
- [ ] `kill <pid>` 后行为符合所选重启策略；`make format-check` 通过

## 七、任务手册 B：新增一个系统调用

### B.1 编号规则：追加，绝不重排

`kernel/include/kernel/syscall_numbers.h` 是**唯一事实源**，内核（`kernel/include/kernel/syscall.h`）与用户态（`user/lib/libos/syscalls.h:28`）都包含它。

| 项目 | 值 | 来源 |
| --- | --- | --- |
| 已定义编号 / 最大编号 | 69 个 / `72`（`SYS_PCI_CFG_WRITE`，文件末行） | `syscall_numbers.h:189` |
| 空洞（未使用） | `26`、`27`、`28`、`43` | 全文件扫描 |
| 分发表大小 | `#define SYS_COUNT (SYS_PCI_CFG_WRITE + 1)` = 73 | `kernel/include/kernel/syscall.h:34` |
| 漂移守卫 | `_Static_assert(SYS_PCI_CFG_WRITE + 1 == SYS_COUNT, "SYS_COUNT drift")` | `syscall.h:38` |

两条路径：**推荐追加到末尾**（下一个是 `73`），并把 `SYS_COUNT` 改成引用新常量、同步 `_Static_assert`；或**复用空洞**（26/27/28/43，当前分发表中为 `NULL`→`ERR_INVAL`，`kernel/syscall/syscall.c:1756-1761`），但要先确认没有外部依赖已按"未定义"语义使用过它们。

> **禁止重排**：编号是内核与用户态共享的 ABI 常量。`syscall_numbers.h:74-76` 记着一次真实教训——16 位 I/O 本想用 48/49，但已被 `SYS_SIGNAL`／`SYS_KILL` 占用，会在分发表中互相遮蔽，最后挪到 69/70。

### B.2 分发表与 handler

分发表是**指定初始化器的函数指针数组**，不是 switch（宏与适配器 `kernel/syscall/syscall.c:1572-1610`，表 `1669-1739`，分发 `1749-1764`）：

```c
typedef i64 (*syscall_fn_t)(u64 a1, u64 a2, u64 a3, u64 a4, u64 a5);
SYSCALL1(sys_foo_bar)                       /* 把自然参数的 handler 适配成 5 参原型 */

static const syscall_fn_t s_syscall_table[SYS_COUNT] = {
    [SYS_FOO_BAR] = sc_sys_foo_bar,
};
```

handler 在本文件时用文件内 `SYSCALLn()` 适配器；放在别的子系统（驱动/调度/内存）时写成 `sc_sys_*` 形式并在 `kernel/include/kernel/syscall_handlers.h` 声明（现有先例：`vspace.c`、`thread_ctx.c`、`pci.c`、`process_desc.c`、`virtio_blk.c`、`shm.c`，见该头文件 28-53 行）。参数 ABI：`RAX` = 编号，`RDI/RSI/RDX/R10/R8` = arg1..arg5（`kernel/arch/x86_64/syscall_entry.S:48,52`）。

### B.3 能力门控：必须选一种形态

=== `docs/kernel_roadmap.md` 1.3 节的纪律原话：**任何新 syscall 必须 `cap_lookup(RIGHT_*)` 校验**。

| 形态 | 用法 | 适用 | 现存实例 |
| --- | --- | --- | --- |
| 句柄 + 权限位 | `cap_lookup(table, handle, need)`（`kernel/include/kernel/cap.h:208`） | 调用方已持有能力句柄（对象式资源） | `MAP_MEMORY`／`IRQ`／`IO_PORT 路径 |
| 无句柄的 rights 扫描 | 直接遍历 `cap_table->entries`，自己做惰性过期 + 权限位判断 | 能力由 `obj_id` 命名（I/O 端口区间、PCI 设备索引），调用方不持句柄 | `ProcHasIoPortCap`（`syscall.c:1247-1270`）、`ProcHasPciDevCap`（`kernel/arch/x86_64/virtio_blk.c:550-568`） |
| atom 门控（决策下沉） | `CapLookupByAtom(table, subject, atom, scope_hash)`（`cap.h:238`，实现 `kernel/cap/cap.c:407`） | 语义级权限（`ATOM_*`，`kernel/include/kernel/atom.h:31-61`），零 IPC 的纯本地判定 | `sys_set_time`（`syscall.c:911-937`，`ATOM_SYS_SET_TIME`）、`SYS_SHM_*`（管理原子） |

门控必须**在任何用户内存访问或硬件访问之前**执行，未命中返回 `ERR_NOCAP`（`-3`，`kernel/include/kernel/types.h:68`）；其它约定返回值：`ERR_INVAL` 参数错、`ERR_FAULT` 用户指针不可读/不可写。用户指针先过 `ValidateUserPtr(ptr, size, need_write)`（`syscall.c:84-88`，委托 `VmmValidateUserPtr`：非零、不溢出、逐页已映射、写路径逐页可写）。

### B.4 用户态包装

在 `user/lib/libos/syscalls.h` 声明 + `syscalls.c` 实现，全部走唯一的 `sys_call(num, a1..a5)` 内联汇编（`syscalls.h:85-102`）；注意它把 arg4/arg5 钉在 `r10`／`r8`，clobber 列表含 `rcx`／`r11`。

```c
/* syscalls.h: gated on ATOM_XXX / CAP_TYPE_YYY; 0 on success,
 * ERR_NOCAP unauthorized, ERR_INVAL bad arg. */
int FooBar(unsigned long v);
/* syscalls.c: */
int FooBar(unsigned long v) {
    return (int)sys_call(SYS_FOO_BAR, (long)v, 0, 0, 0, 0);
}
```

### B.5 在 init 自检中加测试

| 套件 | 宏 | 累计计数 | 入口函数 | 来源 |
| --- | --- | --- | --- | --- |
| 经典 syscall | `TEST/PASS/FAIL/ASSERT` | `tests_run/tests_pass` | `RunTests()` | `user/services/init/main.c:60-83,1019` |
| 另外 6 个套件 | `P1_TEST`／`P2V`／`KBD`／`P3`／`P4`／`P5` 同名模式 | `p1_ p2v_ kbd_ p3_ p4_ p5_` | `RunP2GateTests`／`RunP1PermTests`／`RunP2VfsTests`／`RunKbdFocusTests`／`RunCrashRecoveryTests`／`RunResourceExhaustionTests`／`RunZeroCopyTests` | `init/main.c:1107,1536-1559,1644,2085,2203,2281,2375,2478` |

在 `main()`（`init/main.c:2503-2586`）里每个套件跑完立即比对 `pass != run` 并调 `BootSelftestFail(suite, pass, run)`——打印横幅后**永久停机**（`init/main.c:2494-2499`）。正反两条断言参考 `init/main.c:1563-1602`：

```c
static void TestFooBarGate(void) {
    P2_TEST("foo_bar unauthorized -> ERR_NOCAP");
    P2_ASSERT(FooBar(0) == ERR_NOCAP, "unauthorized foo_bar accepted");
    P2_PASS();

    P2_TEST("foo_bar authorized -> 0");
    int h = CapCreateAtom(ATOM_SYS_SET_TIME, RIGHT_ALL, 0, 0, 0); /* 换成你自己的 atom */
    P2_ASSERT(h > 0, "CapCreateAtom failed");
    P2_ASSERT(FooBar(1) == 0, "authorized foo_bar failed");
    P2_PASS();
}
```

### B.6 自检清单

- [ ] 编号**追加在末尾**（或确认空洞可安全复用），`SYS_COUNT` 与 `_Static_assert` 同步
- [ ] 用户指针先 `ValidateUserPtr`（写路径 `need_write = true`）
- [ ] 能力 / atom 门控在任何内存或硬件访问之前，未授权返回 `ERR_NOCAP`
- [ ] handler 登记进 `s_syscall_table`（否则 `ERR_INVAL`）；跨文件时在 `syscall_handlers.h` 声明
- [ ] `libos` 包装 + 注释写清门控与错误码；init 自检加"未授权拒绝 + 授权成功"两条断言
- [ ] `make run` 中 8 个套件全绿；安全相关改动在 `docs/kernel_roadmap.md` 第七之二节加固表补一行（含验证列）

## 八、任务手册 C：新增一条 Shell 命令

### C.1 注册 API

```c
/* user/services/shell/shell.h */
typedef int (*cmd_func_t)(int argc, char *argv[]);
int ShellRegisterCommand(const char *name, const char *help, cmd_func_t func);
```

实现 `user/services/shell/shell.c:395-432`：`name`／`help` 会被 `strdup` 复制（可用字面量），节点**追加到链表尾**，因此 `help` 输出顺序 = 注册顺序；重名返回 `ERR_INVAL`，内存不足 `ERR_NOMEM`。命令集是**运行时链表**而非编译期表（`shell.c:223-247`）。**单写者规则**：链表只在 shell 开始读输入**之前**写；shell 自己在 `ShellMain()` 顶部注册内置命令（`shell.c:3763` 起，当前共 61 条）。`shell.h:17-24` 允许其它服务在拉起 shell 之前注册，但**当前树内没有服务这么做**，全部调用点都在 `shell.c` 内。

### C.2 三步加一条命令

1. 前置声明区（`shell.c:160-221`）加 `static int CmdFoo(int argc, char *argv[]);`
2. 注册块（`shell.c:3763` 起）加 `ShellRegisterCommand("foo", "Do the foo thing: foo <name> [count]", CmdFoo);`
3. 实现 `CmdFoo`，输出一律 `ShellWrite`／`ShellPrintf`；若受策略管辖，`policy` 返回 `DENY` 的命令不会被执行（`shell.c:1414-1418`）

### C.3 参数解析约定（`ParseLine`，`shell.c:1368-1398`）

| 约定 | 行为 |
| --- | --- |
| 分隔符 | 空格与 Tab |
| 引号 | `"..."` 内空格不切分，闭合引号就地改成 `'\0'` |
| `argv[0]` | 命令名本身；业务参数从 `argv[1]` 开始 |
| 参数上限 | `MAX_ARGS 16`（`shell.c:101`），**超出的参数被静默丢弃** |
| 就地修改 | `ParseLine` 直接改写行缓冲，解析后不要复用原行 |
| 路径参数 | 仅 `ls/cat/tee/mkdir/rm/stat/fallocate/mv/bm_create` 的首个路径参数按 `s_cwd` 重写（`shell.c:1420-1461`）；新命令吃相对路径必须进这个白名单 |

命令函数返回值：成功 `0`，参数错负值（实现里普遍 `-1`，策略拒绝 `-9`）；`Execute()` 透传该返回值（`shell.c:1464-1467`）。

### C.4 输出必须走 term 端口，绝不用 printf

| 通道 | 去向 | 用途 | 来源 |
| --- | --- | --- | --- |
| `ShellWrite`／`ShellPutc` | `IpcCall(TERM_OP_WRITE)` 到 `s_term_port`（32 字节分块） | **用户可见的命令输出** | `shell.c:447-480` |
| `ShellPrintf` | 同上；`%d %x %s %c %%%` + `%0Nd`／`%0Nx` 零填充 | 格式化输出 | `shell.c:519-582` |
| `printf`（libc） | `DebugLog → SYS_DEBUG_LOG → COM1` | **只作调试** | `user/lib/libc/stdio.c:468-482`、`kernel/syscall/syscall.c:109-188` |

> `ShellPrintf` 的注释仍写 "through the serial service"（`shell.c:519`），但实现发往 `s_term_port`——**注释滞后，以代码为准**。

### C.5 骨架

```c
/* (1) forward declaration (shell.c:160-221 区域) */
static int CmdFoo(int argc, char *argv[]);

/* (2) implementation: output MUST go through ShellWrite/ShellPrintf
 * (term port); a libc printf would land on the serial debug channel. */
static int CmdFoo(int argc, char *argv[]) {
    if (argc < 2) {
        ShellWrite("Usage: foo <name> [count]\n");
        return -1;
    }
    int count = (argc >= 3) ? atoi(argv[2]) : 1;
    if (count <= 0) {
        ShellWrite("foo: count must be > 0\n");
        return -1;
    }
    ShellPrintf("foo: name=%s count=%d\n", argv[1], count);
    return 0;
}

/* (3) registration (inside ShellMain(), before the REPL starts) */
ShellRegisterCommand("foo", "Do the foo thing: foo <name> [count]", CmdFoo);
```

## 九、任务手册 D：新增客户端库 / 修改 IPC 协议

### 9.1 传输约束（先记住这三条）

| 约束 | 数值 / 规则 | 来源 |
| --- | --- | --- |
| 单条消息上限 | `MAX_MSG_SIZE = 4096`；超限 `ERR_INVAL` | `kernel/include/kernel/types.h:125`；检查点 `kernel/ipc/ipc.c:393,514,574` |
| 编码 | 原生小端结构体直拷，无序列化层；`req[0] = op` 惯例 | `user/services/vfs/vfs.h:22-26` |
| 分块 | 大负载必须自己分块；`vfs` 把读写都压到 4032 字节 | `user/services/vfs/vfs.h:131-136` |

### 9.2 协议头文件的写法（照抄 `user/services/vfs/vfs.h`）

1. 守卫用 `USER_SERVICES_<X>_<X>_H` 风格（`vfs.h:33-34`）。
2. **不包含 `kernel/types.h`**，自己 `typedef uint8_t u8;` 等（`vfs.h:29-31`；原因见第十三节第 6 条）。
3. op 用 `enum { X_OP_A = 1, ... }` 显式赋值，**新 op 只能追加**（`vfs.h:160-178`）。
4. req 首字段 `u32 op`，resp 首字段 `i32 ret`。
5. **必须**给每个可能超限的结构体加 `_Static_assert`：

```c
#define VFS_IPC_MAX 4096

typedef struct {
    u32 op;
    u32 len;
    u8  data[VFS_IPC_MAX - 8];
} vfs_req_write_t;

_Static_assert(sizeof(vfs_req_write_t) <= VFS_IPC_MAX, "write req exceeds IPC limit");
```

现存断言点：`user/services/vfs/vfs.h:343,556-560`、`user/services/gui/gui.h:147-148`、`user/services/net/net.h:79-80`、`user/services/net/main.c:135,156`、`kernel/include/kernel/syscall.h:38`、`kernel/sched/thread_ctx.c:53`。

### 9.3 客户端调用的标准形态

`user/lib/libfs/fs.c:90-111` 是范本，四个动作缺一不可：

```c
int FsGetItem(const char *url, vfs_item_info_t *out_item) {
    int port = FsPort();                 /* 1. 惰性解析并缓存端口（PortGet） */
    if (port < 0)
        return port;

    vfs_req_get_item_t *req = (vfs_req_get_item_t *)s_req;
    memset(req, 0, sizeof(*req));        /* 2. 零填充整个请求结构体 */
    req->op = VFS_OP_GET_ITEM;
    strncpy(req->path, url, sizeof(req->path) - 1);

    vfs_resp_get_item_t *resp     = (vfs_resp_get_item_t *)s_resp;
    int                  resp_len = (int)sizeof(*resp);   /* 3. 缓冲区真实大小 */
    int                  r        = IpcCall(port, req, (int)sizeof(*req), resp, &resp_len);
    if (r < 0)
        return r;                        /* 4. 传输错误 */
    if (resp->ret < 0)
        return resp->ret;                /* 4. 业务错误 */
    *out_item = resp->item;
    return 0;
}
```

**响应缓冲区必须与服务端可能回复的最大长度一致。** 内核在 `IpcCall` 返回路径上按服务端给的 `resp_len` **无条件** `memcpy` 到调用方缓冲区（`kernel/ipc/ipc.c:562-565`），而 syscall 层只用调用方**输入的** `resp_len` 做一次 `ValidateUserPtr`（`kernel/syscall/syscall.c:524-531`）。缓冲区开小了 = 越界写，内核不会替你挡。

### 9.4 结构体布局必须逐字节一致

| 场景 | 规则 | 实例 |
| --- | --- | --- |
| 内核 ↔ 用户态共享 ABI | 头文件只 include `<stdint.h>`（或什么都不 include），**不得** include `kernel/types.h`，两端共用同一份头 | `kernel/include/kernel/pci.h:32`、`blk.h:28`、`atom.h`、`syscall_numbers.h` |
| 无法共享的内核头 | 用户态**手抄**结构体并注明 "layout MUST match" | `user/lib/libos/syscalls.h:140-148`（`rtc_time_t`，对应 `kernel/include/kernel/rtc.h:37`）、`syscalls.h:197-234`（`proc_info_t`／`proc_ident_t`） |
| 服务 ↔ 客户端 | 协议头放在服务目录，客户端库 include 同一份 | `user/services/vfs/vfs.h` 被 vfs_server／两个驱动／libfs 四方共用（`vfs.h:17-21`） |

**版本兼容注意事项**：**只追加**（新 op 追加编号、新字段追加到结构体**尾部**；中间插字段会改掉后续所有字段的偏移）；**别动已有字段的类型或顺序**（`_Static_assert` 只守总尺寸，守不住偏移，带签名的持久化结构要靠 magic + version 自查——`VFS_BOOKMARK_MAGIC`／`VFS_BOOKMARK_VERSION 2`（`vfs.h:324-325`）、`PERM_POLICY_MAGIC`／`PERM_POLICY_VERSION 1`（`user/services/perm/perm.h:350-351`））；**契约冻结**——`docs/ops_format.md` 与其头文件 `user/services/pkg/pkg.h` 已宣告 Phase A 冻结，冻结后不得单方面修改已定义字段/op，要先与编排者确认并升版本号；**别忘了分块**——改 payload 尺寸要重算分块常量并复跑 `_Static_assert`。

### 9.5 新增客户端库步骤

1. `user/lib/lib<name>/<name>.c` + `<name>.h`；`.c` 加入 `USER_C`（**不加入** `USER_SVC_ENTRY_OBJ`）
2. 头文件写清端口名、op 语义、返回约定（`0`／负错误码）、线程安全约束——`libfs` 明确声明是单线程库（`s_req`／`s_resp` 是共享静态缓冲，`user/lib/libfs/fs.c:22-27`）
3. 加 `_Static_assert` 守卫结构体尺寸
4. 至少接一个调用方并跑通端到端验证

## 十、构建与验证流程

### 10.1 环境依赖

| 工具 | 用途 | 说明 |
| --- | --- | --- |
| `gcc` / `nasm` / `ld.lld` 或 `ld` | 内核与用户态 C（`-std=c11 -Wall -Wextra -O2`）；`.asm` 与 `.S` 同为 NASM、Intel 语法、`-f elf64`；ELF 链接优先 `ld.lld` | `Makefile:18,37,40,53` |
| `grub2-mkrescue`／`xorriso`、`qemu-system-x86_64`、`python3` | ISO 生成、运行与自动化 | `Makefile:322-323,327-345` |
| `clang-format` | 风格校验，**需自备**（当前开发容器内未安装，`make format-check` 会报"未找到命令"，这不是代码缺陷） | `Makefile:355-371` |

### 10.2 构建目标

| 目标 | 作用 | 行号 |
| --- | --- | --- |
| `make all` / `kernel.elf`、`make init_user`、`make iso` | 只构建内核 ELF（含全部服务 blob）／只构建用户态 init ELF／**推荐入口**：全套 + GRUB ISO | `Makefile:234-235,253,318-323` |
| `make run` | 构建 ISO 并在 QEMU 运行（`-serial stdio`、`-d int,cpu_reset,guest_errors`、virtio-blk 挂 `disk.img`） | `Makefile:326-333` |
| `make debug` | QEMU + GDB stub（`-s -S`，端口 1234，`-nographic`） | `Makefile:335-345` |
| `make format` / `format-check` / `clean` / `help` | 格式化／校验／删除 `build/`／列出目标 | `Makefile:355-371,348-349,217-230` |

依赖跟踪自动完成：`-include $(KERNEL_C_OBJ:.o=.d) $(USER_OBJ:.o=.d)`（`Makefile:374`，由 `-MD -MP` 生成）。

### 10.3 提交前四步验证（顺序不要颠倒）

```bash
# (1) 单文件快速编译验证 —— 目标只在该文件已列入 USER_C（或 KERNEL_C）后才存在
make build/user/services/sensor/sensor.c.o
make build/kernel/ipc/ipc.c.o

# (2) 全量构建：必须 0 新警告（-Wall -Wextra）
make iso

# (3) 运行回归：启动自检 8 个套件必须全绿
make run
# 期望 init: ALL SELFTESTS PASSED (N/N)；失败则 !!! SELFTEST FAILURE ... !!! 并永久停机

# (4) 风格校验
make format-check
```

已验证的行为细节：`make build/user/services/hello/main.c.o` 与 `make build/kernel/ipc/ipc.c.o` 有规则；`make build/user/services/foobar/main.c.o` 会直接报"没有规则可制作目标"——所以第 (1) 步必须在改完 `USER_C` 之后做。

### 10.4 禁止格式化的文件及其原因

| 文件 | 原因 | 排除方式 |
| --- | --- | --- |
| `user/services/vfs/fs_mem_driver.c` | 项目规则冻结（`.clang-format` 头注释写明 "frozen by project rule"；旧 README 补充：与既有的 128 KB staging buffer 决策相关） | `Makefile:358,368` 的 `grep -v 'fs_mem_driver.c'` |
| `user/services/term/font.h`、`kernel/include/kernel/panic_font.h` | 生成的字模位图数据 | `Makefile:359-360,369-370` 的 `grep -v 'term/font.h'` 与 `grep -v 'kernel/panic_font.h'`（后者能命中 `kernel/include/kernel/panic_font.h`） |

两个目标的实际命令都是 `find kernel/ user/ -name '*.c' -o -name '*.h' | grep -v ... | xargs clang-format`（`Makefile:355-371`），排除清单**完全一致**。注意清单里**没有** `user/lib/font_cjk.h`（1.29 MB 生成数据）与 `user/lib/libgui/font.h`——它们仍在 `find` 的匹配范围内，不要对它们跑 `make format`（或先把它们补进清单），否则会产生一次无意义的大体积重排 diff。

## 十一、调试手段

### 11.1 串口日志（第一手段）

```text
用户态 printf -> DebugLog(buf[512]) -> SYS_DEBUG_LOG -> sys_debug_log()
  -> 分页安全拷贝（<=512 B）-> UTF-8 边界回退 -> 令牌桶限流 -> COM1 -> -serial stdio
```

| 要点 | 事实 | 来源 |
| --- | --- | --- |
| 内核早期日志 | `SerialPuts` 在 Stage 1 即可用；`boot.asm` 早期用单字符标记 | `kernel/kernel_main.c:94` |
| 上限与限流 | `printf` 缓冲 512 B（对齐内核 `DEBUG_LOG_MAX`）；令牌桶每 tick 补 2048 B、桶上限 4096 B，**超出部分被静默丢弃**——`user/services/hello/main.c:26-32` 就是靠 `printf` 后 `Sleep(50)` 规避预算 | `user/lib/libc/stdio.c:469-472`、`kernel/syscall/syscall.c:104-107,162-183` |
| 落盘 | 直接重定向 `make run` 的输出（`-serial stdio`）最稳妥；`scripts/run.sh --serial` 追加的 `-serial` 会落到 **COM2**，`build/serial.log` 恒为 0 字节（已知缺陷） | `Makefile:326-333`、`scripts/run.sh:88-100` |

> 屏幕与串口**严格分离**：shell 的提示符、回显、命令结果都在帧缓冲上（经 `term` 端口），串口只有 `printf` 调试通道。串口看不到 shell 输出是**设计如此**。

### 11.2 QEMU monitor：sendkey + screendump

`make run` 没开 monitor；自动化脚本用 Unix socket：

```bash
qemu-system-x86_64 -cdrom build/opsos.iso -m 256M \
  -serial file:build/serial.log \
  -monitor unix:/tmp/opsys-mon.sock,server=on,wait=off
# 再用 socket 发 monitor 命令：sendkey t-e-e-space-s-l-a-s-h-space-h-i / sendkey ret
#                             screendump /tmp/screen.ppm
```

脚本实现参考 `scripts/smoke_test.py:76-126`（`mon_cmd`／`type_command`）与 `smoke_test.py:653-661`（QEMU 命令行）。

### 11.3 screendump → 文本：`tools/vga_decode.py`

```bash
python3 tools/vga_decode.py /tmp/screen.ppm
python3 tools/vga_decode.py /tmp/screen.ppm --font user/services/term/font.h   # 默认值
```

输出 113×38 文本网格（1024×768 帧缓冲，cell 9×20 px）；字形像素白（`0xFFFFFF`）、底色深蓝（`0x082860`），光标格反色，亮度阈值 400，每字形允许 8 bit 误差。字库解析按**块顺序**（index i 对应字符 `0x20+i`），因为转义引号 `'\''` 会让基于注释的正则误解析（`tools/vga_decode.py:36-47`）。

### 11.4 GDB stub 与自检回归网

`make debug` 打印 "GDB stub listening on port 1234" 并让 QEMU 停在 `-S`，然后 `gdb kernel.elf -ex 'target remote :1234'`；`kernel.elf` 带符号，可直接断点内核函数。历史上用这个手段验证过栈金丝雀：破掉 canary 后实测触发 `panic("stack smashing detected")` 并 halt（`docs/kernel_roadmap.md` 第四之二节的 P0 行）。

启动自检本身就是回归网：8 个套件顺序执行（经典 syscall / P1 权限引擎 / P2 syscall 门控 / P2 VFS 授权 / KBD 焦点 / P3 崩溃恢复 / P4 资源耗尽 / P5 零拷贝），结论见 `user/services/init/main.c:2503-2586`；任一套件 `pass != run` 就 `BootSelftestFail` 停机并打印横幅（`init/main.c:2494-2499`），不会"带病继续启动"。加测试方法见 B.5；宿主机侧脚本矩阵见 2.5。

## 十二、提交与协作约定

### 12.1 不要擅自 `git add` / `git commit`

该约定**源自旧版 README** 的「架构约束（改代码前必读）」小节，原文为「**不 `git add/commit`** 除非用户明确要求」（可用 `git show HEAD~20:README.md` 查到）。**当前版本 README 已不再收录这一条**，仓库里也没有任何流程（Makefile、脚本、CI 配置）依赖自动提交；因此它仍是**合理且建议保留**的协作约定：改动留在工作区，由人类决定何时、如何提交。只有任务明确要求时才提交，提交信息按 12.2 的风格写。

### 12.2 提交信息风格（观察 `git log` 的实际风格）

格式：**`前缀: 简述`**（半角冒号 + 一个空格 + 中文简述）。前缀用**子系统名**或**版本号**：

| 前缀类型 | 实测出现的值 |
| --- | --- |
| 子系统 / 领域 | `kernel:` `user:` `gui:`（也出现 `GUI:`）`net:` `tui:` `utf8:` `build:` `license:` `readme:` `fix:` `delete:` `shell:` |
| 版本 / 阶段 | `v0.7.1:` `v0.7:` `v0.6.5:` `v0.5:` `v1.3:` `v0.4:` |

正文惯例（见 `4877fcd`／`aa2b97b`／`105805d`）：中文、分点说明动机、末尾写"验证："并给出可复核的观测结果。

```text
build: Makefile/ld/sh 脚本 GPLv3 许可头 + 恢复文件许可头补齐

1. 变更点一：子要点（为什么这么改、影响哪些文件）
2. 变更点二：...

验证：make iso 0 警告；QEMU 启动冒烟通过（shell 提示符 + init 回归 34/34
     + P1 权限 10/10 + P2/P3 测试组）
```

不要写"fix bug""update"这类无信息量的简述。

### 12.3 大改动必须同步 `docs/`

| 改动类型 | 需要同步的文档 |
| --- | --- |
| 新增 / 改变 Ring 归属、内核原语 | `docs/kernel_roadmap.md`（第三节归属表 + 第四/六节决策记录 + **第七之二节加固表**：每加一项都要写"内容 + 验证"两列） |
| 新增 / 移除内核子系统或迁移驱动 | `docs/microkernel_audit.md` |
| 权限模型、atom、门控语义；VFS 对象模型 / 书签 / 驱动协议 | `docs/permission_model.md`、`docs/vfs_design.md` |
| `.ops` 包格式与沙盒授权 | `docs/ops_format.md`（**已冻结**，改动需先确认并升版本） |
| TUI / 运行时 / 测试结论 / 需求规格 | `docs/tui_design.md`、`docs/runtime_design.md`、`docs/test_report.md`、`docs/requirements.md` |

### 12.4 其它协作约束

- **冻结文件**：不要改 `user/services/vfs/fs_mem_driver.c`（见 10.4）；不要擅自改已冻结的契约头（`user/services/pkg/pkg.h`、`docs/ops_format.md`）。
- **契约先行**：多个并行任务要碰同一个协议头时，先定契约再各自实现；冻结后单方面改动会静默破坏对方。
- **不要提交产物 / 不要新增根文件**：`build/`、`kernel.elf`、`*.o`、`*.d`、`disk.img`、`__pycache__/` 都已在 `.gitignore` 中（仓库根还散落 `list.sh`、`output.txt`、几个 `*.patch`，同样被忽略，不属于源码结构）。另外目前**没有** `AGENTS.md`（提交历史里有 `delete: 删除文件 AGENTS.md`），而 `.clang-format` 头注释仍写着 "matches AGENTS.md conventions"——以 `.clang-format` 本身为准，不要因为找不到它而新造一个。

## 十三、任务手册 E：新增一个权限原子 / 一条角色规则

权限点是**封闭集合**：新增一个原子意味着同时改内核枚举、策略种子、门控点和应用包映射。
参考手册见 [permission_reference.md](permission_reference.md)。

| # | 动作 | 位置 | 要点 |
| --- | --- | --- | --- |
| 1 | 追加原子 | `kernel/include/kernel/atom.h` | 加在 `ATOM_MAX` **之前**；**绝不重排已有值** —— 枚举值会进策略快照与 `.ops` manifest，重排等于悄悄改变已有授权的含义 |
| 2 | 决定默认规则 | `user/services/perm/perm-manager.c` 的 `SeedRules()` | 三种选择：显式 `ALLOW`、显式 `DENY`、**不加规则**（= 默认拒绝 → 弹 Powerbox 询问用户）。给哪个角色加、加什么，直接决定首次访问的体验 |
| 3 | 内核侧门控（若对应敏感系统调用） | `kernel/syscall/syscall.c` | 在**任何副作用之前**做 `CapLookupByAtom(...) == CAP_NULL → ERR_NOCAP`；自检里要有一条"未授权 → `ERR_NOCAP` 且无副作用"的用例 |
| 4 | 服务侧门控（若对应服务操作） | 例如 `user/services/net/main.c` 的 `NetAuthorize()` | 服务必须用 `IpcRecvFrom` 取真实调用者，再 `CapHasAtom(subject, atom)`；**这条查询本身要求调用者（服务）持 `ATOM_SERVICE_MANAGE`** |
| 5 | VFS 路径挂钩（若对应文件访问） | `user/services/vfs/vfs_server.c` | 判权入口是 `PermCheck(resource, access, url, subject, &granted)`；`access` 位到原子的映射在 perm 引擎的 `AtomFromAccess()`（读→DOCS_READ、写→DOCS_WRITE、其余→`ATOM_NONE`） |
| 6 | 映射应用包声明 | `user/services/pkg/pkg_manager.c` 的原子名表 + [ops_format.md](ops_format.md) §4 | manifest 里的权限名是封闭集合；**管理面原子（如 `ATOM_SERVICE_MANAGE`）必须拒绝授予沙盒应用** |
| 7 | 补测试 | `user/services/init/main.c`（P1/P2/P6 套件）、`scripts/verify_tools.py` | 至少覆盖：默认规则生效、显式拒绝不弹窗、授权后可访问、撤销后再次被拒 |
| 8 | 补文档 | [permission_reference.md](permission_reference.md) §三/§四 | 原子表与规则种子表要同步，否则文档立刻开始骗人 |

**检查清单**：

1. `ATOM_MAX` 仍然是"最大原子 + 1"，且没有任何已有值被改动；
2. 新原子的默认行为是**拒绝**（安全默认），需要放行的地方显式加 `ALLOW`；
3. 内核门控在副作用之前；
4. 服务侧门控用的是 `IpcRecvFrom` 的 subject，**没有读请求体里的身份字段**；
5. `.ops` manifest 映射已更新，且管理面原子不可被应用声明；
6. `make iso -j4` 0 警告 + 启动自检全绿（9 个套件）。

---

## 十四、任务手册 F：新增一个 VFS / 驱动操作

VFS 是"每次操作都要判权"的仲裁点，因此新增一个操作要同时照顾协议、判权、错误码与两边驱动。

| # | 动作 | 位置 | 要点 |
| --- | --- | --- | --- |
| 1 | 追加 opcode 与帧 | `user/services/vfs/vfs.h` | 编号**追加不重排**；写清请求/响应结构，并用 `_Static_assert(sizeof(...) <= VFS_IPC_MAX)` 守住 4096 字节上限 |
| 2 | 服务端实现 | `user/services/vfs/vfs_server.c` | 新增 `DoXxx()` + 分发表 `case`；**入口先做长度校验**（`msg_len < sizeof(req)` → `ERR_INVAL`）；**每次操作都要重跑 `PermCheck`**（不要缓存上次结果） |
| 3 | 驱动协议 | `vfs.h` 的 `DRV_OP_*` | 优先**复用既有能力**：例如 `VFS_OP_TRUNCATE` 直接复用 `DRV_OP_WRITE` 的 `len==0`（目标长度走 `offset`）约定，而不是新增 opcode。若确实要新增，同样"追加不重排"，并且**两个驱动都要实现或明确返回 `ERR_INVAL`（不支持）** |
| 4 | 命名空间与错误码 | 服务端 | 名称校验（空 / `.` / `..` / 含分隔符 / 超长）只做一次，放在服务端；错误码按语义选：`VFS_ERR_STALE`（曾经有、现在没了）、`VFS_ERR_PERM`（句柄权限不足）、`VFS_ERR_READONLY`、`VFS_ERR_NOSPC`、`VFS_ERR_EXISTS`、`VFS_ERR_ACCESS`（判权被拒）。**不要让客户端靠猜来区分** |
| 5 | 客户端包装 | `user/lib/libfs/fs.c` + `fs.h` | 沿用静态 `s_req`/`s_resp` 复用与 `FsPort()` 惰性解析；头文件写 Doxygen（Returns / Errors） |
| 6 | 补测试 | `user/services/init/main.c` 的 P6/P2V 套件、`scripts/verify_tools.py` | 覆盖成功路径、每一类错误路径，以及"撤销授权后该操作立即失效" |
| 7 | 补文档 | [vfs_design.md](vfs_design.md)、[service_reference.md](service_reference.md) §6.4/§6.16 | opcode 表要同步 |

**检查清单**：

1. 协议结构 `_Static_assert` 通过（≤ 4096）；
2. 服务端每个新 op 都有长度校验与判权复检；
3. 两个驱动对同一个新操作的行为一致（或明确声明"不支持"）；
4. 失效句柄返回 `VFS_ERR_STALE` 而不是 `ERR_NOENT`；
5. libfs 包装已加、Doxygen 已写；
6. `make iso -j4` 0 警告 + 启动自检全绿。

---

## 十五、常见陷阱清单

| # | 陷阱 | 正确做法 | 出处 |
| --- | --- | --- | --- |
| 1 | **用户态 `printf` 走串口，不上屏** | 面向用户的输出走 `term` 端口（shell 内用 `ShellWrite`／`ShellPrintf`）；`printf` 只作调试 | `user/lib/libc/stdio.c:468-482`、`kernel/syscall/syscall.c:109-188` |
| 2 | **串口日志被限流截断** | 别在热循环里打日志；爆发式日志按 tick 预算（2048 B/tick、桶 4096 B）分批 + `Sleep` | `kernel/syscall/syscall.c:104-107,162-183`、`user/services/hello/main.c:26-32` |
| 3 | **IPC 结构体不零填充 / 只发部分字节** | `memset(req, 0, sizeof(*req))` 后用 `IpcCall(port, req, sizeof(*req), ...)` 发**整个定长结构体** | `user/lib/libfs/fs.c:97-104` |
| 4 | **响应缓冲区开小 → 内核越界写** | `resp_len = (int)sizeof(*resp)`，resp 指向完整响应结构体 | `kernel/ipc/ipc.c:562-565`、`kernel/syscall/syscall.c:524-531` |
| 5 | **消息超 4096 字节** | 分块（`vfs` 用 4032）；协议头加 `_Static_assert` 守卫 | `kernel/include/kernel/types.h:125`、`kernel/ipc/ipc.c:393,514,574`、`user/services/vfs/vfs.h:131-136,556-560` |
| 6 | **用户态直接 include 内核头文件** | `-I kernel/include` **确实**在用户态编译参数里（`Makefile:52`），且故意共享一小组纯 ABI 头（`kernel/syscall_numbers.h`、`pci.h`、`blk.h`、`atom.h`——只 include `<stdint.h>` 或什么都不 include）。但 `kernel/types.h` **绝不能**被用户态包含：它的 `error_t` 枚举（`OK`／`ERR_*`）会与 `user/lib/libos/syscalls.h:40-50` 的宏直接冲突。要内核结构体时，要么共享纯 ABI 头，要么手抄并注明 "layout MUST match" | `user/services/manager/manager.c:107-111`、`user/services/vfs/vfs.h:29-31`、`user/lib/libos/syscalls.h:140-148,197-234` |
| 7 | **栈保护 canary 必须用 `-mstack-protector-guard=global`** | 默认 guard 在 `%fs:0x28`（TLS 槽），freestanding 内核没有设置 FS base，会去读物理地址 `0x28`（实模式 IVT 区）而不是内核自己的 canary | `Makefile:25-31`；运行实现 `kernel/arch/x86_64/stack_chk.c`、`user/runtime/stack_chk.c` |
| 8 | `if (x) return;` 写成一行 | `AllowShortIfStatementsOnASingleLine: Never` 会拆成两行；写完直接 `make format`，别手写紧凑风格 | `.clang-format` |
| 9 | 引入编译警告 | `-Wall -Wextra` 下必须 0 新警告 | `Makefile:37,53` |
| 10 | **注释可能过期，读注释要回源码** | `shell.c:519` 写 "through the serial service" 但 `ShellPutc` 实际发往 `s_term_port`（`shell.c:447-458`）；`shell.h:19` 写 "12 built-ins" 而实际注册 61 条；`manager.c:151-156` 的重启策略注释漏了 `user` | 三处均已核实 |
| 11 | 新服务漏登记 blob 表 | `kernel/blob/blob.c` 两处 + `BLOB_MAX_ENTRIES`；当前表**恰好已满 28 条**，漏改 = `BlobRegister` 返回 `ERR_NOMEM` → `BLOB_REG` panic → **开机 panic** | `kernel/blob/blob.c:47-74,83-87,89-120`、`kernel/include/kernel/blob.h:31-32` |
| 12 | 新服务漏加 `USER_SVC_ENTRY_OBJ` | 该服务的 `main()` 会进共享对象池，被链进所有其它服务 | `Makefile:175-204` |
| 13 | `SVC_*` 宏值与 `s_services[]` 下标不一致 | 宏值必须等于物理下标，否则拉起错误的服务 | `user/services/manager/manager.c:132-173` |
| 14 | 以为 `s_services[]` 的 `restartable` 是生效开关 | 真正生效的是 `StartServiceMonitors()` 的 `s_restartable[]` | `user/services/manager/manager.c:472-485` |
| 15 | 对生成字模文件跑 `make format` | 排除清单只含 `term/font.h`、`kernel/include/kernel/panic_font.h`、`fs_mem_driver.c`；`user/lib/font_cjk.h` 与 `user/lib/libgui/font.h` **不在**清单里 | `Makefile:355-371` |
| 16 | 忘了 `SYS_COUNT` 同步 | 追加编号到 73 时，`#define SYS_COUNT (SYS_PCI_CFG_WRITE + 1)` 与 `_Static_assert` 都要更新，否则越界一律 `ERR_INVAL` | `kernel/include/kernel/syscall.h:34,38`、`kernel/syscall/syscall.c:1756-1761` |
| 17 | 在持有内核 IPC 锁时阻塞 | 硬规则：**绝不**在持 `s_ipc_lock` 时调 `ThreadYield`／`SchedReschedule`／`SchedSleep`（IF=0 下阻塞 = 必定死锁） | `kernel/ipc/ipc.c` 头部 "Lock discipline (hard rule)" |
| 18 | shell 命令参数超 16 个 / 相对路径不生效 | `MAX_ARGS 16`，超出被静默丢弃；吃相对路径的新命令要进 `Execute()` 的 `is_path_cmd` 白名单 | `user/services/shell/shell.c:101,1368-1398,1420-1461` |
| 19 | 新 syscall 忘记门控或门控放晚了 | 任何新 syscall 必须 `cap_lookup(RIGHT_*)` 或 atom 门控，且门控在任何内存/硬件访问之前 | `docs/kernel_roadmap.md` 1.3 节；范例 `kernel/syscall/syscall.c:911-937,1247-1270` |

> 返回 [文档索引](README.md)
