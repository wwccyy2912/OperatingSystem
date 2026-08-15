# AGENTS.md

OpSys — x86_64 微内核操作系统（C11 + NASM，无 libc，自有 runtime）。项目文档齐全，先读 `docs/`：`requirements.md`（需求与里程碑）、`kernel_roadmap.md`（Ring 0/3 归属定案）、`vfs_design.md`、`permission_model.md`、`tui_design.md`、`runtime_design.md`。

## 构建与运行

- 构建：`make iso` → `build/opsos.iso`（GRUB 引导）。**纪律：必须 0 新警告**（`-Wall -Wextra -O2`，警告即失败）。
- 单文件快速编译验证：`make build/user/services/<svc>/<file>.c.o`（或内核 `build/kernel/...`），比跑完整 ISO 快得多。
- 运行：`make run`（QEMU `-nographic -serial mon:stdio`，virtio-blk 挂 `disk.img`）。
- 回归测试：QEMU 串口输出驱动，init 内置测试套件（31 项：经典 + P1 权限 + P2V 能力抹位），以 `make iso` + QEMU 跑通为准。
- 调试：`make debug`（GDB stub :1234）+ `gdb kernel.elf`。内核栈 canary（`-mstack-protector-guard=global`）由 `kernel/arch/x86_64/stack_chk.c` 提供。

## 架构要点（改代码前必读）

- **微内核**：调度/PMM/VMM/IPC/cap 在 Ring 0；全部设备驱动与服务在 Ring 3（串口、键盘、term、vfs、perm、device_mgr 等）。
- **服务 = 独立进程**：`user/services/<svc>/main.c`，每个服务 ELF 作为 blob 嵌入 `kernel.elf`，由 `manager` 用 `SYS_PROCESS_CREATE` 拉起。新服务要改 4 处：Makefile 的 `USER_C`、`USER_SVC_ENTRY_OBJ`、`SVC_NAMES`、`SVC_LINK_RULE`。
- **IPC**：同步消息传递，`ipc_call`/`ipc_recv`，服务间靠注册的端口名（`port_register`/`port_get`）寻址。请求/响应 = 固定结构体（见各服务的 `*_req_*`/`*_resp_*`）。
- **身份模型（重要，勿回退）**：权限身份是内核签发的 `subject_id`（u64，不可伪造，服务用 `ipc_recv_from` 取调用者真实 subject）。`app_id_hash`（自报 u32）已废弃，勿再引入。获取自身：`get_subject()`；进程元数据：`proc_info_by_subject()`（`proc_ident_t`：pid/name/uuid_hi/uuid_lo）。
- **权限流程**：vfs 对无授权访问 → perm-manager（Powerbox）创建 PENDING query → 推 `PERM_OP_UI_SHOW` 给 term 的 `perm.ui` 端口 → term 渲染 TUI 面板 → 用户 y/n → `PERM_OP_ANSWER`。UI 面板必须显示应用名 + PID（`name (PID n)`），**禁止再显示 hash 编号**。
- **键盘焦点**：keyboard 服务多槽 parked（`s_park[4]`）+ 焦点路由（`s_focus_owner`）。`KBD_OP_TAKE_FOCUS`/`KBD_OP_RELEASE_FOCUS`。焦点持有期间仅焦点所有者收键。
- **TUI 基础设施**：`user/lib/libtui/tui.c/.h` + `user/services/tui_demo/` 已实现**但未接入 Makefile**（tui_design.md v1.0 称 Phase 2 实装完成）。term 面板渲染走 `user/services/term/term.c`（VGA 文本帧缓冲，字体仅 0x20-0x7E → 边框只用 ASCII `+ - |`）。

## 关键约束

- **禁改 `user/services/vfs/fs_mem_driver.c`**（用户明令；其 128KB staging buffer 是为容纳 init.elf 的既有决策）。
- 不 `git add/commit` 除非用户明确要求。仓库大量工作区改动是进行中的迁移，勿误提交。
- 头文件契约冻结后并行任务间不可互相改契约（先定契约、再并行实现）。
- 内核新 syscall 必须 `cap_lookup(RIGHT_*)` 门控；syscall 编号追加（不重排），`SYS_COUNT` 必须同步（`kernel/include/kernel/syscall.h`）。
- 用户态服务共享对象（runtime/libc/libos/libipc/crt0）+ 各自 entry obj 链接为独立 ELF；禁止 `as any` 式强转、空 catch、类型错误掩盖。

## 代码风格

- 4 空格缩进，K&R 括号；类型 `_t` 后缀；全局 `g_`、静态 `s_` 前缀；函数 `模块_动词_名词`。
- 命名空间宏头文件保护（`#ifndef KERNEL_XXX_H`）；错误码负数，结果走指针参数。
- 行宽 ≤ 100；UTF-8 + LF；公共接口 Doxygen 式注释。

## 测试提示

- init/main.c 内置全套回归测试（P1/P2V 权限套件）。跑测试 = 启动 QEMU 看串口输出（31/31 全过）。
- 新增/改动服务后用单文件编译先验证，再 `make iso` 全量 + QEMU 回归。
- 性能基准沿用串口 + screendump 流程（见 kernel_roadmap.md §5.1）。
