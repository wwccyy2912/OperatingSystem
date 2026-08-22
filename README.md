# OpSys

**x86_64 微内核操作系统**

OpSys 是一个从零开始的 64 位微内核操作系统项目，采用「机制在内核、策略在用户态」的设计哲学。内核仅保留调度、内存管理、IPC、能力校验等最小机制；全部设备驱动与服务（串口、键盘、终端、VFS、权限管理、设备管理、包管理等）运行在 Ring 3 用户态，通过同步 IPC 消息传递协作。

**注：本项目完全使用AI生成**

## 目录

- [功能特性](#功能特性)
- [系统架构](#系统架构)
- [项目结构](#项目结构)
- [环境依赖](#环境依赖)
- [安装指南](#安装指南)
- [使用方法](#使用方法)
- [测试与调试](#测试与调试)
- [贡献规范](#贡献规范)
- [许可证](#许可证)
- [特别鸣谢](#特别鸣谢)

---

## 功能特性

### 微内核架构

- **Ring 0 极简内核**（~11,000 行 .c/.S，含头 ~14,000）：调度器、PMM/VMM、IPC 通道、能力表、中断转发、Mutex Fast-Path。
- **Ring 3 全量服务**：所有设备驱动（串口、键盘、virtio-blk）与系统服务（VFS、权限、终端、shell、包管理）均为独立用户态进程。
- **同步 IPC**：`ipc_call` / `ipc_recv`，服务间靠注册端口名寻址（`port_register` / `port_get`）。
- **能力（Capability）系统**：所有资源访问通过能力句柄门控，新 syscall 必须经 `cap_lookup(RIGHT_*)` 校验。

### 安全模型

- **内核签发 SubjectID**（u64，不可伪造）：取代传统 UID/GID 与客户端自报 `app_id_hash`，身份由内核在 IPC 交付时填充。
- **基于属性的动态权限模型**（ABAC）：五层架构 — 身份层 / 角色层 / 权限原子层 / 上下文层 / 策略层；彻底取消 Root 账户。
- **Powerbox 授权流**：无授权访问触发 perm-manager → term 文本弹窗 → 用户 y/n 确认 → 能力签发。
- **沙盒应用**：`.ops` 包格式，应用权限由 manifest 声明，pkg-manager 按 manifest 签发原子能力；应用无法自授。

### VFS（对象句柄 + 安全书签）

- 摒弃 POSIX「一切皆 fd」，采用面向对象资源句柄（Volume / Item / FileHandle / Enumerator）。
- **Security-Scoped Bookmark**：路径不出服务端，客户端持书签（不透明 blob）访问；文件移动后书签仍有效（parent_id 追踪）。
- **virtio-blk 真实磁盘持久化**：跨重启数据保留；内存卷（32MB RAM）+ 磁盘卷双支持。
- **FSKit-lite 驱动协议**：用户态文件系统驱动，挂载握手后由 vfs_server 调用。

### TUI 文本用户界面

- VGA 文本模式 + Linear RGB 双模式支持。
- 状态栏、边框盒子、任意位置文本渲染、光标控制。
- Powerbox 权限询问面板集成。

### 自有 C Runtime

- 零系统调用 malloc 快路径（无竞争自旋锁，无 syscall）。
- ASLR 堆随机化 + 守卫页保护。
- `.init_array` 全局构造 + `.fini_array` 析构 + `atexit` 处理。
- POSIX 风格信号处理（`signal` / `kill` / `sigreturn`，语义在 Ring 3：`user/runtime/signal_user.c`）。
- 就地 `realloc` 扩展（吸收后续空闲块，避免 O(n²) 拷贝）。

---

## 系统架构

```
┌─────────────────────────────────────────────────────────────┐
│ 应用层（.ops 沙盒应用）                                      │
├─────────────────────────────────────────────────────────────┤
│ 用户态服务（Ring 3，独立进程）                                │
│  init · manager · shell · term · serial · keyboard          │
│  vfs · fs_mem_driver · fs_virtio_blk_driver                 │
│  perm · device_mgr · pkg                                    │
├─────────────────────────────────────────────────────────────┤
│ 客户端库（libipc · libos · libc · libfs · libpkg · libtui）  │
├─────────────────────────────────────────────────────────────┤
│ C Runtime（crt0 · malloc · errno · exit · signal）           │
├─────────────────────────────────────────────────────────────┤
│ 微内核（Ring 0）                                              │
│  调度 · PMM/VMM · IPC · Capability · IRQ 转发 · Mutex       │
└─────────────────────────────────────────────────────────────┘
```

**IPC 流**：应用/服务 → `ipc_call(port, req, resp)` → 目标服务处理 → `ipc_reply`。请求/响应 = 固定结构体。

**身份流**：内核签发 `subject_id` → 服务用 `ipc_recv_from` 取调用者真实 subject → perm-manager 校验 → 能力签发/拒绝。

---

## 项目结构

```
OpSys/
├── kernel/                   # 内核（Ring 0，GPLv3）
│   ├── arch/x86_64/          # 平台相关：启动、GDT/IDT、中断、上下文切换、virtio-blk
│   ├── mm/                   # 内存管理：PMM、VMM、vspace、ELF 引导、rbtree
│   ├── sched/                # 调度器、线程、thread_ctx（TCB_WriteRegisters）
│   ├── ipc/                  # IPC 通道、IRQ 转发、Mutex、Notify
│   ├── cap/                  # 能力系统
│   ├── syscall/             # 系统调用入口、进程创建、PCI 枚举
│   ├── blob/                 # 服务 ELF blob 嵌入
│   ├── gfx/                  # Framebuffer（仅 init/query，绘制已移 Ring 3）
│   ├── process/              # 进程管理、信号
│   └── include/kernel/       # 内核头文件
├── user/                     # 用户态
│   ├── lib/                  # libc（LGPLv3）+ SDK 客户端库（GPLv3）
│   │   ├── libc/             # C 标准库子集 — LGPLv3
│   │   ├── libos/            # 系统调用包装、ELF 解析、自旋锁 — GPLv3
│   │   ├── libipc/           # IPC 客户端 — GPLv3
│   │   ├── libfs/            # VFS 客户端 — GPLv3
│   │   ├── libpkg/           # 包管理客户端 — GPLv3
│   │   └── libtui/           # TUI 客户端库 — GPLv3
│   ├── runtime/              # C Runtime — LGPLv3
│   │   ├── crt0.S            # 启动代码
│   │   ├── malloc.c          # 堆分配器（ASLR + 就地扩展）
│   │   ├── init.c / exit.c   # 全局构造/析构、atexit
│   │   ├── errno.c           # 错误码
│   │   └── signal_user.c     # Ring 3 信号语义（dispatcher + handler 表）
│   └── services/             # 用户态服务 — 核心 GPLv3 / 扩展 LGPLv3
│       ├── init/             # 第一个用户程序（含回归测试套件）
│       ├── manager/          # 服务管理器（拉起全部服务）
│       ├── shell/            # 命令行
│       ├── term/             # 终端服务（VGA/Linear RGB + Powerbox UI）
│       ├── serial/           # 串口驱动
│       ├── keyboard/         # 键盘驱动（焦点路由）
│       ├── vfs/              # VFS 服务 + 驱动（内存卷/virtio-blk）
│       ├── perm/             # 权限管理器（perm-engine）
│       ├── device_mgr/       # 设备管理器（PCI 枚举）
│       ├── pkg/              # 包管理器（.ops 安装/运行/沙盒授权）
│       ├── hello/            # 示例应用
│       ├── sbox_demo/        # 沙盒演示
│       ├── runtime_demo/     # Runtime 演示（已实现，未接入 Makefile）
│       ├── tui_demo/         # TUI 演示（已实现，未接入 Makefile）
│       └── flaky/            # 故障测试服务
├── boot/                     # GRUB 引导配置
├── scripts/                  # 辅助脚本
│   ├── accept.py             # QEMU 验收测试（sendkey + 串口镜像）
│   ├── smoke_test.py         # 三轮回合冒烟测试（R1 基线 / R2 盲区 / R3 压力）
│   ├── ops_pack.py           # .ops 包打包工具
│   ├── build.sh / run.sh     # 构建运行快捷脚本
│   └── user.ld               # 用户态链接脚本
├── tools/                    # 测试工具
│   └── vga_decode.py         # VGA screendump PPM 解码（8x16 字模 / 9x20 网格）
├── docs/                     # 设计文档（CC BY 4.0）
│   ├── requirements.md       # 需求规格说明书
│   ├── kernel_roadmap.md     # 内核开发路线图（Ring 0/3 归属定案）
│   ├── vfs_design.md         # VFS 服务设计
│   ├── permission_model.md   # 权限模型设计
│   ├── tui_design.md         # TUI 设计
│   ├── runtime_design.md     # Runtime 设计
│   ├── runtime_quick_ref.md  # Runtime 快速参考
│   └── ops_format.md         # .ops 包格式规范
├── Makefile                  # 统一构建系统
└── AGENTS.md                 # AI 协作指引
```

---

## 环境依赖

| 工具                 | 用途               | 安装示例                        |
| -------------------- | ------------------ | ------------------------------- |
| **GCC**（支持 C11）  | 编译 C 源码        | `apt install gcc`               |
| **NASM**             | 汇编 .asm/.S 文件  | `apt install nasm`              |
| **ld** 或 **ld.lld** | 链接内核与用户 ELF | `apt install binutils` 或 `lld` |
| **GRUB2**            | 生成可引导 ISO     | `apt install grub2-tools`       |
| **QEMU**（x86_64）   | 运行与测试         | `apt install qemu-system-x86`   |
| **GDB**（可选）      | 内核调试           | `apt install gdb`               |
| **Python 3**（可选） | 验收测试与打包脚本 | `apt install python3`           |

---

## 安装指南

### 1. 克隆仓库

```bash
git clone <repo-url> OpSys
cd OpSys
```

### 2. 构建内核与用户服务

```bash
# 完整构建（内核 + 全部用户服务 + ISO）
make iso

# 仅构建内核
make kernel.elf

# 仅构建用户服务
make init_user
```

构建产物：

- `kernel.elf` — 内核 ELF（含嵌入的服务 blob）
- `build/opsos.iso` — GRUB 引导的 ISO 镜像
- `build/user/services/*.elf` — 各用户服务独立 ELF

### 3. 准备磁盘镜像（virtio-blk 持久化卷）

```bash
# 创建空磁盘镜像（首次运行前）
qemu-img create disk.img 8M
```

> `disk.img` 已在 `.gitignore` 中，不会提交到版本库。

---

## 使用方法

### 启动系统

```bash
# 构建并在 QEMU 中运行
make run
```

QEMU 参数：`-nographic -serial mon:stdio`（串口输出即终端），virtio-blk 挂载 `disk.img`。

### Shell 命令

系统启动后进入 shell，支持以下命令（节选）：

| 命令                      | 说明                                         |
| ------------------------- | -------------------------------------------- |
| `ls <url>`               | 列出目录内容（如 `ls /Volumes/System/`）     |
| `cat <url>`              | 查看文件内容                                 |
| `tee <url> <text>`       | 写入文件（首次写磁盘卷触发 Powerbox 授权）   |
| `stat <url>`             | 查看卷/文件信息                              |
| `bm_create <url> <r\|w>`  | 创建书签                                     |
| `bm_resolve`              | 解析书签                                     |
| `perm_answer <id> <y\|n>` | 响应 Powerbox 权限询问                       |
| `perm_revoke`             | 撤销权限                                     |
| `pkg install <name>`      | 安装应用                                     |
| `pkg list`                | 列出已安装应用                               |
| `pkg run <app_id>`        | 运行应用                                     |

### Powerbox 授权流程

当应用/服务首次访问未授权资源时：

1. VFS → perm-manager 创建 PENDING query
2. perm-manager → term 的 `perm.ui` 端口推送询问
3. term 渲染 TUI 面板：`perm: <app_name> (PID n) 请求访问 <url> (<R/W>) — 输入 perm_answer <id> y/n`，面板显示 `Allow? (y/n)` 询问，确认后显示 `Result: ALLOWED/DENIED` 与 `Access: ...`
4. 用户执行 `perm_answer <id> y` 授权或 `n` 拒绝

---

## 测试与调试

### 回归测试

init 进程内置全套回归测试套件（49 项），涵盖：

- 经典内核测试（31 项：IPC、内存映射、线程、能力）
- P1 权限套件（10 项：角色解析、规则链、能力签发）
- P2 权限门控（3 项 Gate）
- P2V 能力抹位验证（4 项，即 P2 的 VFS 能力抹位套件）
- KBD 焦点测试（1 项：TAKE_FOCUS/RELEASE_FOCUS 所有权往返，非 owner 释放返回 ERR_NOCAP）

```bash
make iso && make run
# 观察串口输出：31/31 + 10/10 + 3/3 + 4/4 + KBD Focus: 1/1 全过
```

### 单文件快速编译验证

修改单个文件后，无需跑完整 ISO，先验证编译：

```bash
# 用户服务
make build/user/services/<svc>/<file>.c.o

# 内核
make build/kernel/<path>/<file>.c.o
```

### GDB 调试

```bash
make debug
# 另一终端：
gdb kernel.elf -ex 'target remote :1234'
```

### 验收脚本

```bash
python3 scripts/smoke_test.py          # 三轮冒烟：R1 基线 + R2 盲区 + R3 压力
python3 scripts/smoke_test.py --drive  # 追加磁盘卷持久化（R2.7/R3.5，需 disk.img）
python3 scripts/accept.py              # QEMU sendkey 注入 + 串口镜像日志
python3 scripts/ops_pack.py            # 打包/校验 .ops 文件
```

---

## 贡献规范

### 代码风格

- **缩进**：4 空格；**行宽**：≤ 100 字符；**编码**：UTF-8 + LF。
- **括号**：K&R 风格。
- **命名**：
  - 类型：`_t` 后缀（如 `cap_t`、`vfs_handle_t`）
  - 全局变量：`g_` 前缀；静态变量：`s_` 前缀
  - 函数：`模块_动词_名词`（如 `pmm_alloc_page`、`vfs_resolve_bookmark`）
  - 常量/宏：全大写（如 `PAGE_SIZE`、`MAX_MSG_SIZE`）
- **头文件保护**：`#ifndef KERNEL_XXX_H` / `#define` / `#endif`
- **错误处理**：返回错误码（负数），结果走指针参数
- **注释**：公共接口函数须有 Doxygen 风格注释

### 汇编（NASM）

- Intel 语法；System V AMD64 ABI（RDI/RSI/RDX/RCX/R8/R9）
- 全局标号 `_` 前缀（`_start`）；局部标号 `.` 前缀（`.loop`）
- Callee-saved 寄存器（RBX、RBP、R12-R15）必须恢复

### 架构约束（改代码前必读）

- **微内核纪律**：调度/PMM/VMM/IPC/cap 在 Ring 0；全部驱动与服务在 Ring 3。
- **新服务接入**需改 Makefile 4 处：`USER_C`、`USER_SVC_ENTRY_OBJ`、`SVC_NAMES`、`SVC_LINK_RULE`。
- **新 syscall** 必须 `cap_lookup(RIGHT_*)` 门控；编号追加（不重排），`SYS_COUNT` 同步。
- **身份模型不可回退**：权限身份是内核签发的 `subject_id`，`app_id_hash` 已废弃。
- **禁止改 `user/services/vfs/fs_mem_driver.c`**（128KB staging buffer 是既有决策）。
- **头文件契约冻结后**并行任务间不可互相改契约。
- **不 `git add/commit`** 除非用户明确要求。

### 代码格式化

项目使用 [clang-format](https://clang.llvm.org/docs/ClangFormat.html) 统一代码风格，配置见 [.clang-format](.clang-format)（4 空格缩进、K&R 括号、行宽 ≤ 100）。

```bash
make format        # 格式化全部 C 源码（排除冻结/生成文件）
make format-check  # 检查格式是否合规（CI 用）
```

排除文件（不可格式化）：

- `user/services/vfs/fs_mem_driver.c` — 项目规则冻结
- `user/services/term/font.h` — 生成的字体位图数据
- `kernel/include/kernel/panic_font.h` — 生成的字体位图数据

### 构建验证流程

1. 单文件编译验证（快速）
2. `make iso` 全量构建（0 新警告）
3. `make run` QEMU 回归（49 项全过）

详细规范见 [AGENTS.md](AGENTS.md) 与 [docs/](docs/) 目录。

---

## 许可证

本项目采用多组件许可证模型，各组件的许可证独立适用：

| 组件                        | 许可证                                                                   | LICENSE 文件位置                                                                   | 适用范围                                        |
| --------------------------- | ------------------------------------------------------------------------ | ---------------------------------------------------------------------------------- | ----------------------------------------------- |
| **Kernel**                  | [GPLv3](https://www.gnu.org/licenses/gpl-3.0.html)                       | [kernel/LICENSE](kernel/LICENSE)                                                   | `kernel/` 目录全部代码                          |
| **System Core**             | [GPLv3](https://www.gnu.org/licenses/gpl-3.0.html)                       | [user/services/LICENSE](user/services/LICENSE)                                     | init · manager · perm · vfs                     |
| **System Extension**        | [LGPLv3](https://www.gnu.org/licenses/lgpl-3.0.html)                     | [user/services/LICENSE](user/services/LICENSE)                                     | serial · keyboard · term · device_mgr           |
| **libc**                    | [LGPLv3](https://www.gnu.org/licenses/lgpl-3.0.html)                     | [user/lib/LICENSE](user/lib/LICENSE)                                               | `user/lib/libc/`                                |
| **SDK**（libos/libipc/…）   | [GPLv3](https://www.gnu.org/licenses/gpl-3.0.html)                       | [user/lib/LICENSE](user/lib/LICENSE)                                               | `user/lib/libos/` ~ `libtui/`                  |
| **Runtime**                 | [LGPLv3](https://www.gnu.org/licenses/lgpl-3.0.html)                     | [user/runtime/LICENSE](user/runtime/LICENSE)                                       | `user/runtime/` 全部运行时                      |
| **Documentation**           | [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/)                | [docs/LICENSE](docs/LICENSE)                                                       | `docs/` 目录全部文档                            |
| **Applications**            | 由作者自行决定                                                            | —                                                                                  | `hello/` · `sbox_demo/` · `runtime_demo/` 等   |

> **许可证逻辑**：核心系统组件（内核 + system core + SDK）采用 GPLv3，确保修改回馈社区；扩展组件与 libc/runtime 采用 LGPLv3，允许闭源应用链接；应用层由作者自行选择许可证。

---

## 里程碑

| 版本            | 内容                                             | 状态   |
| --------------- | ------------------------------------------------ | ------ |
| v0.1            | 内核 + init + 串口 + IPC                         | 已完成 |
| v0.2            | 能力系统 + 权限模型（P0/P1/P2）                  | 已完成 |
| VFS Phase 0/1/2 | 对象模型 + 内存卷 + virtio-blk + 书签 + Powerbox | 已完成 |
| v0.3            | 包管理器 + .ops 沙盒应用                         | 已完成 |
| v0.4            | 图形 + 窗口管理器                                | 规划   |
| v0.5            | 音频 + 网络                                      | 规划   |
| v1.0            | 稳定版（多核、性能优化、文档齐全）               | 规划   |

详细路线图见 [docs/kernel_roadmap.md](docs/kernel_roadmap.md) 与 [docs/requirements.md](docs/requirements.md)。

### 特别鸣谢

#### 工具
**1.OpenCode ：这个工具和调用的Big Pickle完成了起步设计和大部分架构**
**2.Trae CN ：这个工具和配套的AI大模型完成了代码格式整理和修复部分Bug的工作**
**3.DeepSeek Harness ：这个工具和配套的DeepSeek v4 flash出色的完成了操作系统生产环境大部分的代码功能添加和优化**
#### AI大模型
**1.OpenCode Big Pickle**
**2.DeepSeek v4 flash**
**3.GLM 5.2**
