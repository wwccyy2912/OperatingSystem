# OpSys 系统调用参考手册（System Call Reference）

> 适用版本：OpSys v0.8-dev（git HEAD 105805d）　|　最后更新：2026-09-19
>
> 本文逐条列出 OpSys 微内核对外暴露的全部 69 个系统调用：编号、参数寄存器、用户态包装函数、返回值、内核 handler 实现位置与权限门控（capability gate）。

本文所有编号、参数、返回值与门控条件均取自仓库源码，关键结论都在行内标注了出处文件（必要时带行号）。**编号的唯一事实源是 `kernel/include/kernel/syscall_numbers.h`**，任何与此表不一致的注释（例如 `user/lib/libos/syscalls.h` 仍写着 "INT 0x80"）都以该头文件与分发逻辑为准。

---

## 一、总览

### 1.1 编号单一事实源

`kernel/include/kernel/syscall_numbers.h` 同时被内核与用户态包含：

| 侧 | 引入点 | 说明 |
|----|--------|------|
| 内核 | `kernel/include/kernel/syscall.h:26` | 定义 `SYS_COUNT` 并做 `_Static_assert` 防漂移 |
| 用户态 | `user/lib/libos/syscalls.h:28` | libos 包装函数用它填 `sys_call()` 的编号 |

该头文件刻意使用 `#define` 而不是 `enum`，理由写在文件头注释（`syscall_numbers.h:18-20`）：既要能在 C 的枚举初始化器里用，也要能参与预处理比较。

表长度常量：

```c
/* kernel/include/kernel/syscall.h:34-38 */
#define SYS_COUNT (SYS_HALT + 1)           /* = 74 */
_Static_assert(SYS_HALT + 1 == SYS_COUNT, "SYS_COUNT drift");
```

因此 `num >= 73` 一律被 `syscall_dispatch()` 以 `ERR_INVAL` 拒绝（`kernel/syscall/syscall.c:1756-1761`）。

### 1.2 调用约定（Calling Convention）

| 项 | 寄存器 | 出处 |
|----|--------|------|
| 系统调用号 | `RAX` | `kernel/arch/x86_64/syscall_entry.S:47` |
| 参数 1 | `RDI` | `syscall_entry.S:48` |
| 参数 2 | `RSI` | `syscall_entry.S:48` |
| 参数 3 | `RDX` | `syscall_entry.S:48` |
| 参数 4 | `R10` | `syscall_entry.S:48` |
| 参数 5 | `R8` | `syscall_entry.S:48` |
| 返回值 | `RAX`（`i64`，负数为错误码） | `syscall_entry.S:280`、`kernel/include/kernel/syscall.h:40-43` |
| 被破坏寄存器 | `RCX`（用户 RIP）、`R11`（用户 RFLAGS） | `syscall_entry.S:21-28`、`user/lib/libos/syscalls.h:91-94` |

内核侧统一入口是：

```c
i64 syscall_dispatch(u64 num, u64 arg1, u64 arg2, u64 arg3, u64 arg4, u64 arg5);
```

汇编把保存下来的 `RAX/RDI/RSI/RDX/R10/R8` 按 System V AMD64 顺序搬到 `RDI/RSI/RDX/RCX/R8/R9` 再调用它（`syscall_entry.S:268-275`）。

### 1.3 两条入口路径

`syscall_entry.S` 提供两个入口，汇聚到同一个 `syscall_common`：

```text
 (A) syscall 指令 (LSTAR 快速路径，不切栈)
     RAX=num, RDI/RSI/RDX/R10/R8=args; SFMASK 屏蔽 IF/TF/DF
     -> syscall_entry_fast: swapgs; 用户 RSP 存入 thread.syscall_save_rsp;
        在 kstack_top-40 处用 GS 相对存储合成 [RIP][CS][RFLAGS][RSP][SS]
 (B) int 0x80 (IDT 向量 0x80，兼容/回退路径)
     CPU 在 TSS.RSP0 压 5 qword 帧（RCX/R11 被破坏）
     -> syscall_entry_stub: swapgs; 把 40 字节帧拷到本线程内核栈
                        |
        两条路径 ---------+---> syscall_common
                                 保存 15 个 GPR -> call syscall_dispatch(num,a1..a5)
                                 -> 返回值写回 RAX 槽 -> call signal_check_syscall(frame)
                                 -> 显式 wrmsr 恢复 GS/MSR_KERNEL_GS_BASE
                                 -> 弹出 15 个 GPR -> iretq 回用户态
```

要点与出处：

* 快速路径入口只做 `swapgs`，不动 `RSP`/`TSS`，帧靠 GS 相对存储合成，因此不需要空余的暂存寄存器（`syscall_entry.S:140-165`）。
* 出口是 `iretq` 而不是 `sysretq`：注释记录了 `sysretq` 在 QEMU TCG 下抛 `#GP`（error 0x28）这一未解决的虚拟化交互（`syscall_entry.S:354-360`）。
* 阻塞式系统调用可能在处理中途让出 CPU，所以出口不依赖入口的 `swapgs` 配对，而是显式 `wrmsr` 写入 GS 状态（`syscall_entry.S:300-323`）。
* **全程 `IF=0`**：INT 路径靠中断门，SYSCALL 路径靠 `SFMASK` 的 bit 1。这使 "先校验用户指针、再拷贝" 在构造上无竞态（`syscall_entry.S:56-60`）。
* 首次进入 `syscall_dispatch()` 时（在处理该调用之前）会一次性重编程 PIT（通道 0，mode 2，`1193182/100` = 100 Hz）并解除 IRQ0/IRQ2 屏蔽，`scheduler_started` 保证只做一次（`kernel/syscall/syscall.c:1541-1554`、`1750-1754`）。

### 1.4 用户态包装：sys_call()

```c
/* user/lib/libos/syscalls.h:85-102（逐字） */
static inline long sys_call(long num, long a1, long a2, long a3, long a4, long a5) {
    long ret;
    register long a4_reg __asm__("r10") = a4;
    register long a5_reg __asm__("r8")  = a5;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(num), "D"(a1), "S"(a2), "d"(a3), "r"(a4_reg), "r"(a5_reg)
                     : "memory", "rcx", "r11");
    return ret;
}
```

注释解释了为什么 a4/a5 必须用显式寄存器变量钉住：普通 `"r"` 约束会让 GCC 自选寄存器，5 参数的 `IpcCall()` 会把垃圾送进 arg4/arg5（`syscalls.h:87-96`）。

`user/lib/libos/syscalls.c` 里每个 `SYS_*` 对应一个薄包装，包装**不做任何校验、缓冲或加锁**（`syscalls.c:39-42`）。极少数调用在成功时永不返回：`ThreadExit`、`sys_reboot`、`sys_shutdown`、`sys_panic`。

### 1.5 分发与实现文件分布

`syscall.c` 用指定初始化器（designated initializer）建表，O(1) 查表，空洞槽位保持 `NULL` 并返回 `ERR_INVAL`：

```c
/* kernel/syscall/syscall.c:1756-1761 */
if (num >= SYS_COUNT) return (i64)ERR_INVAL;
syscall_fn_t fn = s_syscall_table[num];
if (!fn) return (i64)ERR_INVAL;
return fn(arg1, arg2, arg3, arg4, arg5);
```

handler 分布在多个子系统文件中：`kernel/syscall/syscall.c`（绝大多数，用 `SYSCALLn()` 宏生成统一 5 参适配器，`1572-1667`）、`kernel/syscall/process_desc.c`（`SYS_PROCESS_CREATE`）、`kernel/syscall/pci.c`（4 个 PCI 调用）、`kernel/mm/vspace.c`（`SYS_VSPACE_ALLOC`）、`kernel/mm/shm.c`（`SYS_SHM_*`）、`kernel/sched/thread_ctx.c`（`SYS_THREAD_SET_CTX`）、`kernel/arch/x86_64/virtio_blk.c`（`SYS_BLK_*`）。

外部队列 handler 的声明集中在 `kernel/include/kernel/syscall_handlers.h`（`syscall_handlers.h:28-53`）。

### 1.6 编号空洞

编号**不连续**。已定义 70 个编号，未定义但落在 `SYS_COUNT` 范围内的空洞是：

| 空洞编号 | 状态 |
|----------|------|
| 26、27、28 | 未定义；表项为 `NULL`，调用返回 `ERR_INVAL` |
| 43 | 未定义（42 是 `SYS_GET_RTC_TIME`，44 是 `SYS_FB_GET_INFO`），同样 `ERR_INVAL` |

`syscall.c:1566` 的注释也明确说 "Unimplemented numbers (reserved FB/PCI/signal slots, gaps) stay NULL and are rejected with ERR_INVAL"。

16 位 I/O 端口调用（69/70）之所以排在表尾，是因为 48/49 已被 `SYS_SIGNAL`/`SYS_KILL` 占用，若放在 48/49 会在分发表里互相遮蔽（`syscall_numbers.h:74-78`）。

---

## 二、统一错误码

### 2.1 错误码表

内核定义在 `kernel/include/kernel/types.h:64-76`，用户态镜像在 `user/lib/libos/syscalls.h:40-50`，两侧数值一致：

| 名称 | 值 | 含义 | 典型触发点 |
|------|----|------|-----------|
| `OK` | 0 | 成功 | 全部成功路径 |
| `ERR_NOMEM` | -1 | 物理内存/表项耗尽 | `IpcPortCreate()` 无空闲端口、`MutexCreate()` 表满、`CapCreate` 槽位满 |
| `ERR_INVAL` | -2 | 参数非法 | 编号越界、未对齐、`count == 0`、`signum` 越界 |
| `ERR_NOCAP` | -3 | 缺少能力/原子 | `ATOM_SYS_SET_TIME` 门控、I/O 端口门控、PCI 门控 |
| `ERR_NOENT` | -4 | 对象不存在 | 端口不存在、PID/TID 不存在、blob 未注册、过期能力 |
| `ERR_BUSY` | -5 | 已有他人在用 | 端口名重复注册、重复 join、重复 wait、已回复过的 token |
| `ERR_AGAIN` | -6 | 请重试 | virtio-blk DMA 超时（`virtio_blk.c:583` 注释） |
| `ERR_FAULT` | -7 | 用户指针非法/设备错误 | 指针未映射、跨出 `USER_PTR_MAX`、块设备状态非 OK |
| `ERR_OVERFLOW` | -8 | 目标缓冲不足 | `BlobGet()` 目标缓冲小于 blob、端口注册表满（64） |
| `ERR_DENIED` | -9 | 明确拒绝 | 自铸 `CAP_TYPE_DAC_OVERRIDE`、授予时 rights 不是子集、非 mutex 属主 unlock |
| `ERR_INTERRUPTED` | -10 | 阻塞等待被信号终止打断 | `SIGKILL` 打断阻塞中的 `IpcRecv()`/`IpcCall()`（`kernel/ipc/ipc.c:609-673`） |

### 2.2 使用约定（容易踩的坑）

1. **返回值复用**：句柄、端口号、TID、PID、计数都是非负数据；错误是负数。判定一律用 `< 0`。
2. **有例外**：`SYS_MAP_MEMORY` 失败时返回 **0**（不是负数），成功时返回映射到的虚拟地址（`syscall.c:560-621`，所有失败分支都是 `return 0;`）。因此 `map_memory()` 的返回值不能按 `< 0` 判断。
3. **纯数据返回**：`SYS_GET_TIME`（tick 数）、`SYS_GET_FREE_PAGES`（页数）、`SYS_CAP_HAS_ATOM`（1/0）、`SYS_WAIT_NOTIFICATION`（消费掉的 pending 位图）都可能实际为 0，需要按语义而非符号判断。
4. **`ERR_INTERRUPTED` 只来自进程级 IPC 阻塞**：mutex 等待被打断时，`MutexLock()` 返回的是 `ERR_NOENT`（`kernel/include/kernel/mutex.h:90-98`）。
5. **门控先于一切**：所有受门控的 syscall 都是 "GATE FIRST"——先查内核 cap 表，再看参数、再碰硬件。未授权调用返回 `ERR_NOCAP` 且**没有任何副作用**（例如 `SYS_REBOOT` 不会碰 8042 复位线，`syscall.c:1356-1367`）。

---

## 三、系统调用总表（按编号）

列说明：`包装` 指 `user/lib/libos/syscalls.h` 中声明的用户态函数；`SC` = `kernel/syscall/syscall.c`。门控列写 "无" 表示内核不做能力检查（但仍可能有同进程/同主体约束，见 §5.4）。

| # | 名称 | 用户态包装 | 参数 (a1..a5) | 返回 | 实现（handler） | 门控 |
|---|------|-----------|---------------|------|-----------------|------|
| 0 | `SYS_DEBUG_LOG` | `DebugLog(str)` | str | 0 / `ERR_FAULT` | SC `sys_debug_log` | 无（限速，见 §4.1） |
| 1 | `SYS_CAP_CREATE` | `CapCreate`, `CapCreateObj` | type, rights, obj_id | handle>0 / `ERR_*` | SC `sys_cap_create` | 条件：`PCI_DEV`/`IO_PORT` 需 `ATOM_SERVICE_MANAGE`；`DAC_OVERRIDE` 一律 `ERR_DENIED` |
| 2 | `SYS_CAP_GRANT` | `CapGrant` | handle, target_pid, rights | 新 handle / `ERR_DENIED`/`ERR_NOENT`/`ERR_FAULT` | SC `sys_cap_grant` | 源 cap 需 `RIGHT_GRANT`，rights 必须是子集 |
| 3 | `SYS_CAP_REVOKE` | `CapRevoke` | handle | `OK`/`ERR_*` | SC `sys_cap_revoke` | 无 |
| 4 | `SYS_IPC_SEND` | `IpcSend` | port, msg, len | `OK`/`ERR_*` | SC `sys_ipc_send` | 无 |
| 5 | `SYS_IPC_RECV` | `IpcRecv` | port, buf, &len, &tok | `OK`/`ERR_*` | SC `sys_ipc_recv` | 无 |
| 6 | `SYS_IPC_CALL` | `IpcCall` | port, req, req_len, resp, &resp_len | `OK`/`ERR_*` | SC `sys_ipc_call` | 无 |
| 7 | `SYS_IPC_PORT_CREATE` | `IpcPortCreate` | — | port>=1 / `ERR_NOMEM` | SC `sys_ipc_port_create` | 无 |
| 8 | `SYS_MAP_MEMORY` | `map_memory` | cap, offset(virt), size, prot | 虚拟地址 / **0=失败** | SC `sys_map_memory` | `CAP_TYPE_MEM` + `RIGHT_WRITE` |
| 9 | `SYS_UNMAP_MEMORY` | `UnmapMemory` | addr, size | `OK`/`ERR_INVAL`/`ERR_FAULT` | SC `sys_unmap_memory` | 无（限本进程地址空间） |
| 10 | `SYS_THREAD_CREATE` | `ThreadCreate` | entry, arg, priority | tid / 负错误 | SC `sys_thread_create` | 无（entry 必须 `< USER_PTR_MAX`） |
| 11 | `SYS_THREAD_EXIT` | `ThreadExit` | code | 不返回 | SC `sys_thread_exit` | 无 |
| 12 | `SYS_THREAD_YIELD` | `ThreadYield` | — | 0 | SC `sys_thread_yield` | 无 |
| 13 | `SYS_THREAD_SET_AFFINITY` | `ThreadSetAffinity` | tid, cpu | `OK`/`ERR_INVAL` | SC `sys_thread_set_affinity` | 无 |
| 14 | `SYS_THREAD_JOIN` | `ThreadJoin` | tid, &exit_code | 0 / `ERR_NOENT`/`ERR_INVAL`/`ERR_BUSY`/`ERR_FAULT` | SC `sys_thread_join` | 无（不能 join 自己） |
| 15 | `SYS_GET_TIME` | `GetTime` | — | tick 数 | SC `sys_get_time` | 无 |
| 16 | `SYS_SLEEP` | `Sleep` | ticks | 0 / `ERR_INVAL` | SC `sys_sleep` | 无 |
| 17 | `SYS_PORT_REGISTER` | `PortRegister` | name, port | `OK`/`ERR_*` | SC `sys_port_register` | 无 |
| 18 | `SYS_PORT_GET` | `PortGet` | name | port / `ERR_NOENT`/`ERR_FAULT` | SC `sys_port_get` | 无 |
| 19 | `SYS_GET_FREE_PAGES` | `GetFreePages` | — | 空闲页数 | SC `sys_get_free_pages` | 无 |
| 20 | `SYS_GET_PID` | `GetPid` | — | pid / `ERR_FAULT` | SC `sys_get_pid` | 无 |
| 21 | `SYS_PROCESS_CREATE` | `ProcessCreate` | name, desc, blob, size | pid / 负错误 | `process_desc.c` `sc_sys_process_create` | 软门控：调用者需 `ATOM_SERVICE_MANAGE` 才能获得 blob 身份种子（§4.12） |
| 22 | `SYS_MUTEX_CREATE` | `MutexCreate` | — | handle>=1 / `ERR_NOMEM` | SC `sys_mutex_create` | 无 |
| 23 | `SYS_MUTEX_LOCK` | `MutexLock` | handle | `OK`/`ERR_NOENT`/`ERR_BUSY` | SC `sys_mutex_lock` | 无 |
| 24 | `SYS_MUTEX_UNLOCK` | `MutexUnlock` | handle | `OK`/`ERR_NOENT`/`ERR_DENIED` | SC `sys_mutex_unlock` | 无（必须是属主） |
| 25 | `SYS_MUTEX_DESTROY` | `MutexDestroy` | handle | 恒 0 | SC `sys_mutex_destroy` | 无 |
| 29 | `SYS_DEBUG_GETCHAR` | `DebugGetchar` | — | 字符 / `ERR_NOCAP` | SC `sys_debug_getchar` | `CAP_TYPE_IO_PORT` + `RIGHT_READ` 覆盖 0x3F8 |
| 30 | `SYS_NOTIFY` | `Notify` | target_tid, mask | `OK`/`ERR_NOENT`/`ERR_FAULT` | SC `sys_notify` | 目标必须是**同进程**线程 |
| 31 | `SYS_WAIT_NOTIFICATION` | `WaitNotification` | mask | 上次 pending 位图 | SC `sys_wait_notification` | 无 |
| 32 | `SYS_BIND_IRQ` | `BindIrq` | cap, irq, mask | `OK`/`ERR_*` | SC `sys_bind_irq` | `CAP_TYPE_IRQ` + `RIGHT_READ`，且 `obj_id == irq` |
| 33 | `SYS_UNBIND_IRQ` | `UnbindIrq` | cap, irq | `OK`/`ERR_*` | SC `sys_unbind_irq` | 同上 |
| 34 | `SYS_IPC_REPLY` | `IpcReply` | token, resp, len | `OK`/`ERR_*` | SC `sys_ipc_reply` | 无（token 即凭证） |
| 35 | `SYS_IO_READ8` | `IoRead8` | port | 值 / `ERR_NOCAP` | SC `sys_io_read8` | `CAP_TYPE_IO_PORT` + `RIGHT_READ` 覆盖该端口 |
| 36 | `SYS_IO_WRITE8` | `IoWrite8` | port, val | 0 / `ERR_NOCAP` | SC `sys_io_write8` | `CAP_TYPE_IO_PORT` + `RIGHT_WRITE` |
| 37 | `SYS_REBOOT` | `sys_reboot` | — | 不返回 / `ERR_NOCAP` | SC `sys_reboot` | `ATOM_SYS_SHUTDOWN` |
| 38 | `SYS_PANIC` | `sys_panic` | — | 不返回 / `ERR_NOCAP` | SC `sc_sys_panic` | `ATOM_SYS_DEBUG` |
| 39 | `SYS_SHUTDOWN` | `sys_shutdown` | — | 不返回 / `ERR_NOCAP` | SC `sys_shutdown` | `ATOM_SYS_SHUTDOWN` |
| 40 | `SYS_BLOB_GET` | `BlobGet` | name, buf, buf_size | blob 字节数 / `ERR_*` | SC `sys_blob_get` | 无 |
| 41 | `SYS_PROCESS_WAIT` | `ProcessWait` | pid, &exit_code | pid（并回收）/ `ERR_*` | SC `sys_process_wait` | 无（不能等自己） |
| 42 | `SYS_GET_RTC_TIME` | `OsGetRtcTime` | out(rtc_time_t*) | 0 / `ERR_FAULT` | SC `sys_rtc_time` | 无 |
| 44 | `SYS_FB_GET_INFO` | `FbGetInfo` | out(fb_user_info_t*) | 0 / `ERR_*` | SC `sys_fb_get_info` | `ATOM_SERVICE_MANAGE` |
| 45 | `SYS_FB_MAP` | `fb_map` | virt, size | 虚拟地址 / 负错误 | SC `sys_fb_map` | `ATOM_SERVICE_MANAGE` |
| 46 | `SYS_PCI_GET_COUNT` | `PciGetCount` | — | 设备数 | `pci.c` `sc_sys_pci_get_count` | 无 |
| 47 | `SYS_PCI_GET_DEVICE` | `PciGetDevice` | index, out | 0 / `ERR_INVAL`/`ERR_FAULT` | `pci.c` `sc_sys_pci_get_device` | 无 |
| 48 | `SYS_SIGNAL` | 无（运行时直呼 `sys_call`） | dispatcher | `OK`/`ERR_INVAL`/`ERR_NOMEM` | SC `sys_signal` | 无（地址必须落在用户态） |
| 49 | `SYS_KILL` | `Kill` | pid, signum | `OK`/`ERR_*` | SC `sys_kill` | 杀自己无门槛；杀别进程需 `ATOM_SERVICE_MANAGE` |
| 50 | `SYS_SIGRETURN` | 无（运行时直呼 `sys_call`） | frame | 恢复后的 RAX / 负错误 | SC `sys_sigreturn` | 无 |
| 51 | `SYS_GET_HEAP_BASE` | `GetHeapBase` | — | heap 基址 / `ERR_FAULT` | SC `sys_get_heap_base` | 无 |
| 52 | `SYS_PROCESS_LIST` | `ProcessList` | buf, max_entries | 写入条数 / `ERR_FAULT` | SC `sys_process_list` | 无 |
| 53 | `SYS_VSPACE_ALLOC` | `vspace_alloc` | size, flags | 基址 / `ERR_*` | `vspace.c` `sc_sys_vspace_alloc` | 无 |
| 54 | `SYS_THREAD_SET_CTX` | `ThreadSetCtx` | tid, ctx, ctx_size | `OK`/`ERR_*` | `thread_ctx.c` `sc_sys_thread_set_ctx` | 无（目标须同进程） |
| 55 | `SYS_GET_SUBJECT` | `GetSubject` | — | subject / `ERR_FAULT` | SC `sys_get_subject` | 无 |
| 56 | `SYS_IPC_RECV_FROM` | `IpcRecvFrom` | port, buf, &len, &tok, &sender_subject | `OK`/`ERR_*` | SC `sys_ipc_recv_from` | 无 |
| 57 | `SYS_CAP_CREATE_ATOM` | `CapCreateAtom` | atom, rights, expiry, quota, scope | handle>0 / `ERR_*` | SC `sys_cap_create_atom` | `ATOM_CAP_GRANT_SELF` |
| 58 | `SYS_CAP_CONSUME` | `CapConsume` | handle | 0 / `ERR_NOENT`/`ERR_INVAL`/`ERR_FAULT` | SC `sys_cap_consume` | 无 |
| 59 | `SYS_CAP_REVOKE_BY_ATOM` | `CapRevokeByAtom` | subject, atom, scope | 撤销条数 / `ERR_*` | SC `sys_cap_revoke_by_atom` | `ATOM_SERVICE_MANAGE` |
| 60 | `SYS_CAP_GRANT_TO_SUBJECT` | `CapGrantToSubject` | subject, atom, rights, expiry, quota | handle>0 / `ERR_*` | SC `sys_cap_grant_to_subject` | `ATOM_SERVICE_MANAGE` |
| 61 | `SYS_SET_TIME` | `OsSetTime` | t(rtc_time_t*) | 0 / `ERR_NOCAP`/`ERR_INVAL`/`ERR_FAULT` | SC `sys_set_time` | `ATOM_SYS_SET_TIME` |
| 62 | `SYS_BLK_READ` | `sys_blk_read` | disk, lba, count, buf | 0 / `ERR_*` | `virtio_blk.c` `sc_sys_blk_read` | `CAP_TYPE_PCI_DEV`（`obj_id=disk`，`R\|W`） |
| 63 | `SYS_BLK_WRITE` | `sys_blk_write` | disk, lba, count, buf | 0 / `ERR_*` | `virtio_blk.c` `sc_sys_blk_write` | 同 62 |
| 64 | `SYS_BLK_INFO` | `sys_blk_info` | disk, out(blk_info_t*) | 0 / `ERR_*` | `virtio_blk.c` `sc_sys_blk_info` | 同 62 |
| 65 | `SYS_PROC_INFO_BY_SUBJECT` | `ProcInfoBySubject` | subject, out(proc_ident_t*) | 0 / `ERR_NOENT`/`ERR_FAULT` | SC `sys_proc_info_by_subject` | 无 |
| 66 | `SYS_CAP_HAS_ATOM` | `CapHasAtom` | subject, atom | 1/0 / `ERR_*` | SC `sys_cap_has_atom` | `ATOM_SERVICE_MANAGE` |
| 67 | `SYS_SHM_CREATE` | `ShmCreate` | count, virt | 物理基址 / 负错误 | `shm.c` `sc_sys_shm_create` | `ATOM_SERVICE_MANAGE` |
| 68 | `SYS_SHM_MAP` | `ShmMap` | phys_base, count, subject, virt | 0 / 负错误 | `shm.c` `sc_sys_shm_map` | `ATOM_SERVICE_MANAGE` + 池表校验 |
| 69 | `SYS_IO_READ16` | `IoRead16` | port | 值 / `ERR_NOCAP` | SC `sys_io_read16` | `CAP_TYPE_IO_PORT` + `RIGHT_READ` |
| 70 | `SYS_IO_WRITE16` | `IoWrite16` | port, val | 0 / `ERR_NOCAP` | SC `sys_io_write16` | `CAP_TYPE_IO_PORT` + `RIGHT_WRITE` |
| 71 | `SYS_PCI_CFG_READ` | `PciCfgRead32` | index, offset | u32 dword / `ERR_NOCAP`/`ERR_INVAL` | `pci.c` `sc_sys_pci_cfg_read` | `CAP_TYPE_PCI_DEV`(`R\|W`) **且** `ATOM_SERVICE_MANAGE` |
| 72 | `SYS_PCI_CFG_WRITE` | `PciCfgWrite32` | index, offset, val | 0 / `ERR_NOCAP`/`ERR_INVAL` | `pci.c` `sc_sys_pci_cfg_write` | 同 71 |
| 73 | `SYS_HALT` | `sys_halt` | — | 不返回 / `ERR_NOCAP` | SC `sc_sys_halt` | `ATOM_SYS_SHUTDOWN` |

---

## 四、分组详述

### 4.1 调试与串口

| # | 调用 | 语义 |
|---|------|------|
| 0 | `DebugLog(str)` | 把用户字符串写到 COM1 |
| 29 | `DebugGetchar()` | 从 COM1 读一个字符 |

`SYS_DEBUG_LOG` 做了三层保护（`syscall.c:93-188`）：

* 逐页校验用户指针，遇到未映射页或 512 字节上限即停止（`DEBUG_LOG_MAX = 512`，与用户态 printf 缓冲同尺寸）；
* 截断时回退到 UTF-8 字符边界，避免半个字形；
* 令牌桶限速：每 tick 补充 2048 字节，桶上限 4096 字节（`DEBUG_LOG_TICK_BUDGET`/`DEBUG_LOG_BUCKET_MAX`）。原因是串口 TX 忙等且系统调用期间 `IF=0`，无节流的热循环会卡死整个系统。

`SYS_DEBUG_GETCHAR` 受 I/O 端口能力门控：调用者必须持有覆盖 0x3F8（`SERIAL_COM1_BASE`）且带 `RIGHT_READ` 的 `CAP_TYPE_IO_PORT` 能力，否则 `ERR_NOCAP`（`syscall.c:1165-1174`）。返回值是 `SerialGetchar()` 的字符（按 `unsigned char` 扩展）。

```c
DebugLog("hello from ring 3\n");   /* SYS_DEBUG_LOG(0) */
int ch = DebugGetchar();           /* SYS_DEBUG_GETCHAR(29)；无 COM1 端口能力 -> ERR_NOCAP */
if (ch >= 0) DebugLog("got a byte\n");
```

### 4.2 能力管理（Capability）与 atom 生命周期

| # | 调用 | 语义 |
|---|------|------|
| 1 | `CapCreate(type,rights)` / `CapCreateObj(type,rights,obj_id)` | 在本进程 cap 表铸造一个句柄 |
| 2 | `CapGrant(handle, target_pid, rights)` | 把句柄（权限取交集）授予另一进程 |
| 3 | `CapRevoke(handle)` | 撤销本进程的一个句柄 |
| 57 | `CapCreateAtom(atom,rights,expiry,quota,scope)` | 铸造带生命周期字段的 atom 能力（持有者=自己） |
| 58 | `CapConsume(handle)` | 消耗一次配额（含惰性过期检查） |
| 59 | `CapRevokeByAtom(subject,atom,scope)` | 跨全部 cap 表按 (subject, atom, scope) 批量撤销 |
| 60 | `CapGrantToSubject(subject,atom,rights,expiry,quota)` | 直接把 atom 能力签发给某 subject 的进程 |
| 66 | `CapHasAtom(subject,atom)` | 只读查询：该 subject 是否持有活的 atom 能力 |

**句柄编码**：32 位 `[INDEX:24][GEN:8]`，PID 不编码——每个进程有自己的 cap 表，因此 PID 冗余（`kernel/cap/cap.c:83-101`）。

**`SYS_CAP_CREATE` 的 obj_id 语义**（`syscall.c:190-247`）：

| type | obj_id 含义 | 校验 |
|------|-------------|------|
| `CAP_TYPE_IRQ` | IRQ 线号 | `obj_id >= 16` → `ERR_INVAL` |
| `CAP_TYPE_IO_PORT` | `(count << 16) \| base_port` | 无前置校验，使用时匹配 |
| `CAP_TYPE_PCI_DEV` | PCI 表索引 | `obj_id >= PciDeviceCount()` → `ERR_INVAL` |
| `CAP_TYPE_DAC_OVERRIDE` | — | 一律 `ERR_DENIED`（只能内核签发） |
| 其它 | 未使用（0） | — |

另外，铸造 `CAP_TYPE_PCI_DEV`/`CAP_TYPE_IO_PORT` 需要调用者持有 `ATOM_SERVICE_MANAGE`，否则 `ERR_NOCAP`（`syscall.c:228-240`）。`type == 0` 会被归一化成 `CAP_TYPE_KERNEL` 以保持向后兼容（`syscall.c:207`）。

**atom 生命周期**（对照 `cap_entry_t` 的四个新字段，`kernel/include/kernel/cap.h:51-62`）：

```text
 签发 (57 / 60)                              使用                      撤销
 CapCreateAtom(atom, rights, expiry,        CapLookupByAtom(subject,   CapRevokeByAtom(59)
   quota, scope)  持有者 = 调用者              atom, scope)               或 quota 耗尽自动撤销
 CapGrantToSubject(60) 持有者 = target        命中且未过期 -> 放行
 expiry 绝对 tick(0=永久) / quota 剩余次数(0=无限) / scope 64 位哈希(0=不限)
   否则 ERR_NOCAP；过期条目在 cap_lookup 命中时就地作废 (cap.c:265-275)
```

行为细节（都有源码支撑）：

* `CapConsume`：`quota == 0`（无限）返回 `OK` 且不消耗；正配额减到 0 时条目被撤销，之后再 `CapConsume` 返回 `ERR_NOENT`。过期条目被就地撤销后同样报 `ERR_NOENT`（`cap.h:132-143`，实测见 `user/services/init/main.c:484-506`）。
* `CapGrantToSubject` 对 `subject == 0` 返回 `ERR_NOENT`（0 是内核），`atom >= ATOM_MAX` 或 rights 越出 `RIGHT_ALL` 返回 `ERR_INVAL`（`syscall.c:369-399`）。
* `CapHasAtom` 对未知 subject 返回 `ERR_NOENT`，`ERR_NOENT` 而不是 0 是为了不泄漏存在性（`syscall.c:408-429`）。

```c
/* 自检样式的能力生命周期示例（对照 user/services/init/main.c:484-538） */
uint64_t me = GetSubject();                       /* SYS_GET_SUBJECT */

int perm = CapCreateAtom(ATOM_DATA_DOCS_READ, RIGHT_READ, 0, 0, 0);  /* 永久、无限 */
if (perm <= 0) DebugLog("CapCreateAtom failed\n");
if (CapConsume(perm) != 0) DebugLog("consume failed\n");

int q = CapCreateAtom(ATOM_NET_CONNECT, RIGHT_READ, 0, 2, 0);        /* 配额 2 次 */
CapConsume(q); CapConsume(q);
if (CapConsume(q) != ERR_NOENT) DebugLog("quota not enforced\n");

/* 管理面：把决策编码成能力，直接签到目标 subject 的表里 */
int h = CapGrantToSubject(me, ATOM_SYS_SHUTDOWN, RIGHT_ALL, 0, 0);
if (h <= 0) DebugLog("grant failed (need ATOM_SERVICE_MANAGE)\n");

/* 撤权：撤掉该 subject 的该类原子（scope 0 = 任意 scope） */
int n = CapRevokeByAtom(me, ATOM_SYS_SHUTDOWN, 0);
```

### 4.3 IPC 与端口注册表

| # | 调用 | 方向 | 阻塞 |
|---|------|------|------|
| 4 | `IpcSend(port,msg,len)` | 发送 | 无接收者时阻塞；若已有接收者则直接投递 |
| 5 | `IpcRecv(port,buf,&len,&tok)` | 接收 | 无消息时阻塞 |
| 6 | `IpcCall(port,req,req_len,resp,&resp_len)` | 请求-应答 | 阻塞到对方 `IpcReply` |
| 7 | `IpcPortCreate()` | 建端口 | 否 |
| 34 | `IpcReply(token,resp,len)` | 应答 | 否 |
| 56 | `IpcRecvFrom(...)` | 接收 + 发送者身份 | 同 5 |
| 17 | `PortRegister(name,port)` | 名字注册 | 否 |
| 18 | `PortGet(name)` | 名字解析 | 否 |

语义与限制：

* 端口号是 `1..MAX_PORTS(256)`；`port_lookup()` 拒绝 0 与 `> MAX_PORTS`（`kernel/ipc/ipc.c:193-197`）。端口不存在时 `IpcSend/IpcRecv/IpcCall` 返回 `ERR_NOENT`。
* 单条消息上限 `MAX_MSG_SIZE = 4096` 字节；超限返回 `ERR_INVAL`（`ipc.c:393-396`、`514-517`、`574-577`）。
* 接收缓冲区语义：`len` 是**入参/出参**——传入缓冲容量，返回真实消息长度；拷贝量取两者较小值（`ipc.c:455-459`、`237-245`）。
* 消息若是 `IpcCall`，接收方拿到的 `token` 非 0，必须原样传给 `IpcReply`；普通 `IpcSend` 时 `token` 被置 0（`ipc.c:446-449`）。
* `IpcReply` 失败码：`ERR_NOENT`（token 失效/伪造）、`ERR_BUSY`（不是 call 或已回复过）、`ERR_INVAL`（回复超长）（`ipc.c:572-587`）。
* 消息池固定 128 条（`PENDING_POOL_SIZE`，`ipc.c:87`），耗尽返回 `ERR_NOMEM`；进程死亡时 `IpcCleanupProcess()` 销毁其端口、唤醒阻塞者（`ERR_NOENT`）并清掉名字注册，使重启的服务能重新注册同名端口（`ipc.c:354-385`）。
* `IpcRecvFrom` 额外把内核填写的、不可伪造的**发送者 subject** 写出（`syscall.c:472-511`、`ipc.c:210-216`）——权限模型身份层的承重墙（`docs/permission_model.md` §三）。
* 端口注册表容量 64（`PORT_REGISTRY_SIZE`，`ipc.c:88`）：重名注册 `ERR_BUSY`，表满 `ERR_OVERFLOW`，端口不存活 `ERR_NOENT`（`ipc.c:688-716`）。

典型服务端循环（与 `user/services/keyboard/keyboard.c:751-830` 同构）：

```c
int port = IpcPortCreate();
if (port < 0) ThreadExit(1);
if (PortRegister("mysvc", port) != OK) ThreadExit(1);

for (;;) {
    char req[512];
    int  len = (int)sizeof(req);
    int  tok = 0;
    uint64_t caller = 0;
    if (IpcRecvFrom(port, req, &len, &tok, &caller) != OK)
        continue;                 /* ERR_INTERRUPTED: 被 kill 打断 */
    if (tok != 0) {               /* 这是一次 IpcCall，需要应答 */
        const char *resp = "ok";
        IpcReply(tok, resp, 2);
    }
}
```

客户端（同文件可直接编译的等价写法）：`int port = PortGet("mysvc");`（未注册返回 `ERR_NOENT`），随后 `int rlen = sizeof(resp); IpcCall(port, "ping", 4, resp, &rlen);` 阻塞到服务端 `IpcReply`。

### 4.4 内存：映射 / 解映射 / VSpace / 共享池

| # | 调用 | 语义 |
|---|------|------|
| 8 | `map_memory(cap, virt, size, prot)` | 分配物理页并映射到 `virt` |
| 9 | `UnmapMemory(addr, size)` | 解映射并释放物理页 |
| 53 | `vspace_alloc(size, flags)` | 只保留虚拟地址 + 预建页表层级 |
| 67 | `ShmCreate(count, virt)` | 分配连续物理页池并映射进调用者 |
| 68 | `ShmMap(phys, count, subject, virt)` | 把池页只读映射进目标进程 |

**`SYS_MAP_MEMORY`（#8）**要求 `CAP_TYPE_MEM` 能力且带 `RIGHT_WRITE`；`size` 必须非 0、页对齐，`virt` 必须落在用户态；特别地它会拒绝覆盖堆的两个守护页（`[heap_base - PAGE, heap_base)` 与 `[heap_base + HEAP_USER_SIZE, +PAGE)`），让堆上溢/下溢必然踩到未映射页而崩（`syscall.c:592-603`）。**失败返回 0，成功返回 `virt`。**

**`SYS_UNMAP_MEMORY`（#9）**对同一守护页返回 `ERR_INVAL`，其余非法参数返回 `ERR_INVAL`/`ERR_FAULT`（`syscall.c:626-656`）。

**`SYS_VSPACE_ALLOC`（#53）**是 "内核只分配虚拟地址，格式语义交给用户态" 的落地（`kernel/mm/vspace.c:18-22`）：

* `size` 必须非 0 且 4096 对齐，`flags` 目前必须为 0，否则 `ERR_INVAL`；
* 返回的区间不覆盖 ELF 镜像（`0x400000`）、固定测试映射 `[0x10000000, 0x30000000)`、每进程堆区（含守护页）与线程栈区 `[0x90000000, 0x100000000)`；扫描地板为 `VSPACE_FLOOR = 1 GiB`（`vspace.c:72-77`、`373-379`）；
* 只预建 PML4E/PDPTE/PDE，**叶子 PTE 保持 not-present**，之后用 `SYS_MAP_MEMORY` 填充；已被预建的层级算 "已占用"，所以两次分配不会重叠（`vspace.c:359-372`）。

**零拷贝共享池（#67/#68）**：池表固定 8 个（`SHM_MAX_POOLS`），单池上限 1024 页 = 4 MiB（`SHM_MAX_PAGES`，`kernel/mm/shm.c:73-74`）。`ShmCreate` 返回物理基址作为后续 `ShmMap` 的句柄；`ShmMap` 会校验 `(phys_base, count)` 确实落在内核分配的池内（`shm.c:181-186`），并只读、不可执行地映射进目标 subject 的地址空间（`shm.c:194-200`）。调用者死亡时 `ShmCleanupProcess()` 回收池。

```c
/* 对照 user/services/vfs/fs_mem_driver.c:132-147 与 net/main.c:726-733 */
#define POOL_PAGES 256u                       /* 1 MiB */

void *virt = vspace_alloc(POOL_PAGES * PAGE_SIZE, 0);
if (!virt) { DebugLog("vspace_alloc failed\n"); return; }

u64 phys = ShmCreate(POOL_PAGES, virt);       /* 需要 ATOM_SERVICE_MANAGE */
if (phys == 0) { DebugLog("shm_create denied\n"); return; }

u8 *base = (u8 *)virt;                        /* 池内页已可读写 */
/* 导出给客户端（由 vfs_server 代为调用）： */
int r = ShmMap(phys, pages, client_subject, client_virt);
```

### 4.5 线程与调度

| # | 调用 | 关键校验 / 错误 |
|---|------|------------------|
| 10 | `ThreadCreate(entry,arg,priority)` | `entry` 非 0 且 `< USER_PTR_MAX`；`priority <= 31`（CFS 表索引，越界拒绝而不钳制） |
| 11 | `ThreadExit(code)` | 不返回；最后一个线程退出后进程转 `PROC_STATE_ZOMBIE` |
| 12 | `ThreadYield()` | 恒 0 |
| 13 | `ThreadSetAffinity(tid,cpu)` | 未知 tid → `ERR_INVAL`；`cpu = -1` 表示不限 |
| 14 | `ThreadJoin(tid,&exit_code)` | 自己 → `ERR_INVAL`；已有人在 join → `ERR_BUSY`；未知 → `ERR_NOENT` |
| 54 | `ThreadSetCtx(tid,ctx,size)` | `size` 必须等于 `sizeof(thread_ctx_t)`（9×8=72 字节）；目标须同进程，否则 `ERR_NOENT` |

`sys_thread_create()` 会额外把新线程回填 `t->pid = proc->pid` 并递增 `proc->thread_count`，否则新线程的 `process_current()` 会解析到内核进程、所有能力门控都会失败（`syscall.c:777-792`）。

`ThreadSetCtx` 覆盖的是 `context_switch.S` 保存/恢复的那段寄存器区域，字段顺序 `rsp,rbx,rbp,r12..r15,rflags,rip` 与汇编强绑定，`thread_ctx.c:53-55` 用 `_Static_assert` 把布局钉死。

```c
static void worker(void *arg) {
    DebugLog((const char *)arg);
    ThreadExit(0);
}

int tid = ThreadCreate(worker, "worker alive\n", 10);
if (tid < 0) DebugLog("ThreadCreate failed\n");

int code = 0;
if (ThreadJoin(tid, &code) == 0)   /* 阻塞到 worker 退出并回收槽位 */
    DebugLog("joined\n");
```

### 4.6 时间与 RTC

| # | 调用 | 语义 | 错误 |
|---|------|------|------|
| 15 | `GetTime()` | 当前 tick（首个系统调用后 PIT 以 100 Hz 运行，1 tick = 10 ms） | 无 |
| 16 | `Sleep(ticks)` | 相对睡眠 | `ticks == 0` 或 `> 0x7FFFFFFF` → `ERR_INVAL` |
| 42 | `OsGetRtcTime(&t)` | 读 CMOS RTC 墙上时钟 | 指针非法 → `ERR_FAULT` |
| 61 | `OsSetTime(&t)` | 写 CMOS RTC | `ATOM_SYS_SET_TIME` 门控 → `ERR_NOCAP`；范围非法 → `ERR_INVAL` |

`Sleep` 特意拒绝 0 与负数/超大值：用户态 `Sleep(-10)` 会被符号扩展成巨大的 u64，直接读会被解读为 "睡到 tick 回绕"（`syscall.c:871-885`）。

`SYS_SET_TIME` 的顺序是：查 cap 表 → 校验指针 → 拷贝到内核栈 → 范围校验（`year >= 1970`、`month 1..12`、`day 1..31`、`hour <= 23`、`minute <= 59`、`second <= 59`）→ 写 RTC（`syscall.c:911-937`）。`rtc_time_t` 布局：`u16 year; u8 month, day, hour, minute, second;`（`kernel/include/kernel/rtc.h:30-37`）。

```c
rtc_time_t t;
if (OsGetRtcTime(&t) == 0)
    DebugLog("read rtc\n");

/* 需要 ATOM_SYS_SET_TIME；否则 ERR_NOCAP 且不碰 CMOS */
t.year = 2026; t.month = 9; t.day = 19;
t.hour = 12; t.minute = 0; t.second = 0;
int r = OsSetTime(&t);          /* 0 成功 */
```

### 4.7 通知与 IRQ 绑定

| # | 调用 | 语义 | 错误 |
|---|------|------|------|
| 30 | `Notify(tid,mask)` | 把 `mask` 或进目标线程的 pending 位 | 非本进程线程 → `ERR_NOENT`；无当前进程 → `ERR_FAULT` |
| 31 | `WaitNotification(mask)` | 阻塞到匹配位到达；`mask == 0` 为轮询 | 无错误码，返回消费掉的位图 |
| 32 | `BindIrq(cap,irq,mask)` | 把 IRQ 线绑定到当前线程，事件以通知位投递 | `ERR_NOCAP`（cap 类型/线路不匹配）、`ERR_INVAL`（irq > 15）、`ERR_DENIED`（IRQ0/IRQ2 内核保留） |
| 33 | `UnbindIrq(cap,irq)` | 解绑（幂等） | 同门控 |

`SYS_NOTIFY` 的防伪做法：目标线程不存在**或属于别的进程**都返回同一个 `ERR_NOENT`，不泄漏 TID 存在性（`syscall.c:1176-1191`）。内核自己的 `irq.c` 走内部 `Notify()`，不受该限制。

`BindIrq` 要求 IRQ 能力命名了**同一条线**：`cap->obj_id != irq` → `ERR_NOCAP`（`syscall.c:1204-1218`）。IRQ 绑定表是 16 项静态数组，一条线一个属主（`kernel/include/kernel/irq.h:20-21`）。

```c
/* 对照 user/services/keyboard/keyboard.c:743-765 */
#define KBD_IRQ 1
int irq_cap = CapCreateObj(CAP_TYPE_IRQ, RIGHT_READ, KBD_IRQ);
if (irq_cap < 0) ThreadExit(1);
if (BindIrq(irq_cap, KBD_IRQ, 1u) != OK) ThreadExit(1);   /* IRQ1 -> bit0 */

for (;;) {
    unsigned bits = (unsigned)WaitNotification(1u);        /* 阻塞 */
    if (bits & 1u) {
        int sc = IoRead8(0x60);                            /* 需 IO_PORT 能力 */
        (void)sc;
    }
}
```

### 4.8 I/O 端口

| # | 调用 | 需要的权利 |
|---|------|-----------|
| 35 | `IoRead8(port)` | `RIGHT_READ` |
| 36 | `IoWrite8(port,val)` | `RIGHT_WRITE` |
| 69 | `IoRead16(port)` | `RIGHT_READ` |
| 70 | `IoWrite16(port,val)` | `RIGHT_WRITE` |

门控实现在 `ProcHasIoPortCap()`（`syscall.c:1240-1270`）：扫描本进程 cap 表，找 `CAP_TYPE_IO_PORT` 且权利满足、**未过期**（惰性过期规则与 `cap_lookup` 一致）的条目，然后按

```text
obj_id = (count << 16) | base_port
允许条件： base_port <= port < base_port + count
```

判定。真实用例：键盘服务用 `(5 << 16) | 0x60` 覆盖 0x60..0x64（`user/services/keyboard/keyboard.c:802`），网卡驱动用 `(0x20 << 16) | io_base` 覆盖 BAR 的 0x00..0x1F（`user/services/net/main.c:696-697`）。

注意 `syscall.c:1244-1246` 的注释声称 "count == 0 表示只覆盖 base_port"，但代码条件是 `port < base + count`，`count == 0` 时永不成立；仓库内所有调用点都传 `count >= 1`，所以实际行为是 "必须给出正数长度"。

配套的 8 位/16 位变体共享同一门控（`syscall.c:1297-1312` 注释：PCnet 的 RDP/RAP 寄存器需要 16 位访问）。

### 4.9 PCI 枚举与配置空间

| # | 调用 | 语义 | 门控 |
|---|------|------|------|
| 46 | `PciGetCount()` | 懒扫描后返回设备数（首次调用触发扫描，之后返回冻结快照） | 无 |
| 47 | `PciGetDevice(index,&info)` | 拷贝 `pci_device_info_t` | 无 |
| 71 | `PciCfgRead32(index,offset)` | 读 32 位配置空间 dword | `CAP_TYPE_PCI_DEV`(`R\|W`) + `ATOM_SERVICE_MANAGE` |
| 72 | `PciCfgWrite32(index,offset,val)` | 写 dword | 同上 |

扫描范围与规模：bus 0、device 0..31、function 0..7；`vendor == 0xFFFF` 视为空槽；缓存上限 64 项（`PCI_CONFIG_MAX_DEVS`，`kernel/syscall/pci.c:73-81`、`157-186`）。配置访问走经典 0xCF8/0xCFC 端口，**只有内核碰端口**，所以没有 I/O 端口能力的用户进程也能读枚举表（`pci.c:15-23`）。

`SYS_PCI_CFG_*` 的额外硬性条件（`pci.c:284-327`）：

1. 持有 `CAP_TYPE_PCI_DEV` 且 `obj_id == index`、同时具备 `RIGHT_READ|RIGHT_WRITE`；
2. 调用者持有 `ATOM_SERVICE_MANAGE`（原始配置空间访问能改 BAR、关总线主控，不能只靠自铸的 cap 放行）；
3. `index < s_device_count`，`offset` 必须 4 字节对齐且 `<= 0xFC`。

任一不满足分别返回 `ERR_NOCAP`/`ERR_INVAL`。

```c
/* 对照 user/services/net/main.c:683-724 */
int n = PciGetCount();
for (int i = 0; i < n; i++) {
    pci_device_info_t d;
    if (PciGetDevice(i, &d) != 0) continue;
    if (d.vendor_id == 0x1022 && d.device_id == 0x2000) {      /* PCnet */
        int cap = CapCreateObj(CAP_TYPE_PCI_DEV, RIGHT_READ | RIGHT_WRITE, (unsigned)i);
        if (cap < 0) continue;                                  /* 需 ATOM_SERVICE_MANAGE */
        unsigned cmd = (unsigned)PciCfgRead32(i, 0x04);
        PciCfgWrite32(i, 0x04, (cmd & 0xFFFF0000u) | ((cmd | 0x0007u) & 0xFFFFu));
        break;
    }
}
```

### 4.10 块设备（legacy virtio-blk）

| # | 调用 | 参数 | 语义 |
|---|------|------|------|
| 62 | `sys_blk_read(disk,lba,count,buf)` | `disk` = PCI 表索引 | 从 `lba` 读 `count` 个 512 字节扇区到 `buf` |
| 63 | `sys_blk_write(disk,lba,count,buf)` | 同上 | 反向写 |
| 64 | `sys_blk_info(disk,&info)` | `blk_info_t{sectors,sector_size}` | 查容量 |

门控与错误（`kernel/arch/x86_64/virtio_blk.c:574-661`）：

* 需要 `CAP_TYPE_PCI_DEV`，`obj_id == disk`，且同时具有 `RIGHT_READ|RIGHT_WRITE`；否则 `ERR_NOCAP`；
* `count == 0` 或 `lba + count` 超出容量 → `ERR_INVAL`；
* 缓冲区校验失败或设备状态非 `VIRTIO_BLK_S_OK` → `ERR_FAULT`；
* 单次 DMA 操作最多 7 个扇区（内部自动分块），队列单飞：已有操作在飞返回 `ERR_BUSY`；DMA 超时（设备被复位）返回 `ERR_AGAIN`；
* 设备在首次 `SYS_BLK_*` 调用时**惰性初始化**（`BlkEnsureDisk`），传输是轮询完成，不绑 IRQ。

适配器识别：legacy virtio-blk，vendor `0x1AF4`、device `0x1001`（`virtio_blk.c:18-19`）。

```c
blk_info_t info;
if (sys_blk_info(disk_idx, &info) == 0 && info.sector_size == 512) {
    static u8 sector[512];
    if (sys_blk_read(disk_idx, 0, 1, sector) == 0) {
        /* sector 0 已就绪 */
    }
}
```

### 4.11 Framebuffer

| # | 调用 | 门控 | 说明 |
|---|------|------|------|
| 44 | `FbGetInfo(&info)` | `ATOM_SERVICE_MANAGE` | 填 `fb_user_info_t{phys_addr,width,height,pitch,bpp,vga_text}` |
| 45 | `fb_map(virt,size)` | `ATOM_SERVICE_MANAGE` | 以 RW+NX 把 fb 物理页映射进调用者指定地址 |

* 无帧缓冲时返回 `ERR_NOENT`，指针非法 `ERR_FAULT`（`syscall.c:663-683`）。
* `fb_map` 要求 `virt`/`size` 都页对齐、且在用户态；映射长度被钳制在真实 framebuffer 大小（VGA 文本模式 = 1 页；线性模式 = 4 KB 对齐的 `pitch * height`），超出即 `ERR_INVAL`；映射失败会回滚已建立的部分（`syscall.c:693-754`）。
* 内核不再提供任何绘制原语，framebuffer 归用户态 term 服务所有（`kernel/include/kernel/framebuffer.h:20-23`）。

### 4.12 进程管理与进程枚举

| # | 调用 | 语义 |
|---|------|------|
| 19 | `GetFreePages()` | 空闲物理页数（`PmmGetFreeMemory() / PAGE_SIZE`） |
| 20 | `GetPid()` | 当前进程 PID |
| 21 | `ProcessCreate(name,desc,blob,size)` | 描述符式创建进程，返回新 PID |
| 41 | `ProcessWait(pid,&exit_code)` | 阻塞等待并**回收**（reap）目标进程 |
| 51 | `GetHeapBase()` | 本进程 ASLR 堆基址 |
| 52 | `ProcessList(buf,max)` | 枚举进程表 |
| 55 | `GetSubject()` | 本进程内核签发 subject |
| 65 | `ProcInfoBySubject(subject,&ident)` | subject → 内核签发的身份记录（含 128 位 App UUID） |

`SYS_PROCESS_CREATE` 的 ABI（`kernel/syscall/process_desc.c:161-244`）：

```text
a1 = name（用户字符串，最多读 64 字节）
a2 = proc_image_desc_t { u64 entry; u64 seg_count; } 紧接 seg_count 个
     proc_seg_desc_t { vaddr, filesz, memsz, prot, src_offset }（必须连续）
a3 = blob 指针（不透明字节流）     a4 = blob 字节数
```

内核**不解析文件格式**：ELF 解析已经在用户态 `ElfParse()` 完成（`user/lib/libos/syscalls.c:278-299`）。内核逐段校验（页对齐、`memsz >= filesz`、`prot` 合法、段不越界、段间不重叠，段数 `1..16`），建地址空间、映射、按 `src_offset` 拷贝 `filesz` 字节并把 `memsz - filesz` 清零。

**blob 身份种子**：当调用者持有 `ATOM_SERVICE_MANAGE` 时，内核把新进程的 blob 与内嵌服务 ELF 逐字节比对（`blob_size + memcmp`），命中即把 `ATOM_SERVICE_MANAGE` 签进新进程的 cap 表（`process_desc.c:290-330`）。服务名表：`manager, perm, pkg, term, vfs, fs_mem_driver, user, policy, fs_virtio_blk_driver, gui, net, serial, keyboard, shell, wm, device_mgr`。**只信内容不信名字**，否则任意应用可 `BlobGet("perm")` 后复制出字节相同的进程来提权。

`SYS_PROCESS_WAIT` 在成功时返回被回收进程的 PID（不是 0），并释放其地址空间/cap 表/表项；错误：`ERR_NOENT`（不存在或已被回收）、`ERR_INVAL`（等自己，会死锁）、`ERR_BUSY`（已有等待者）、`ERR_FAULT`（退出码指针非法）（`syscall.c:1015-1075`）。

`SYS_PROCESS_LIST` 的 `max_entries` 会被钳到 `MAX_THREADS`（2048）并返回实际写入条数；`proc_info_t` 是固定 84 字节记录 `{i32 pid; u32 state; u32 thread_count; i32 exit_code; u32 main_tid; char name[64];}`，`state` 取 `CREATED=0, READY=1, RUNNING=2, ZOMBIE=3, FINISHED=4`，PID 0（内核）不在列表内（`kernel/include/kernel/proc_info.h:28-35`、`kernel/include/kernel/process.h:29-35`、`kernel/process/process.c:456-479`）。

`ProcInfoBySubject` 对 subject 0、未知 subject、僵尸进程统一返回 `ERR_NOENT`；`proc_ident_t` 布局 `{i32 pid; char name[64]; u64 uuid_hi; u64 uuid_lo;}`（`proc_info.h:48-53`）。

```c
/* 对照 user/services/manager/manager.c:408-445 */
static char blob_buf[524288];
int size = BlobGet("shell", blob_buf, (int)sizeof(blob_buf));
if (size > 0) {
    int pid = ProcessCreate("shell", blob_buf, (unsigned long)size);
    if (pid > 0) {
        int code = 0;
        if (ProcessWait(pid, &code) == pid) {     /* 阻塞 + 回收 */
            DebugLog("service exited\n");
        }
    }
}

/* 枚举与身份查询 */
proc_info_t list[16];
int n = ProcessList(list, 16);
proc_ident_t id;
if (ProcInfoBySubject(GetSubject(), &id) == 0) { /* id.uuid_hi/lo = App UUID */ }
```

### 4.13 信号（POSIX 子集，语义在 Ring 3）

| # | 调用 | 语义 | 门控 |
|---|------|------|------|
| 48 | `SYS_SIGNAL` | 注册 Ring 3 分发器入口（由 C 运行时构造器调用一次） | 地址须在 `[0x1000, USER_PTR_MAX)` |
| 49 | `SYS_KILL` | 给进程挂起信号位；`SIGKILL` 直接强制退出 | 杀自己无门槛，杀别人需 `ATOM_SERVICE_MANAGE` |
| 50 | `SYS_SIGRETURN` | 从 sigframe 恢复被中断上下文 | 无（只由分发器调用） |

* 用户态没有对应的 libos 包装：`user/runtime/signal_user.c:131` 直接 `sys_call(SYS_SIGNAL, __sig_dispatcher, ...)` 注册，`signal_user.c:85` 用 `sys_call(SYS_SIGRETURN, frame_base, ...)` 返回（因此 `user/lib/libos/syscalls.h:245-273` 只声明 `Signal()`/`Kill()`，前者是纯用户态的表替换）。
* 投递是**惰性**的：`SYS_KILL` 只置位 `proc->sig_pending`，真正的投递发生在检查点——系统调用返回用户态（`syscall_entry.S:283-298`）或中断返回用户态（`kernel/include/kernel/signal.h:27-32`）。100 Hz PIT 保证运行中的线程 10 ms 内必过一次检查点。
* `SIGKILL`（9）走 `SignalKillProcess(proc, 128 + SIGKILL)`：给所有线程置 `force_exit`、唤醒阻塞者；`SIGSTOP`（19）是保留的空操作；其余信号（1..63）只置位（`syscall.c:1476-1517`）。
* 参数校验：`pid == 0`、`signum == 0`、`signum >= NSIG(64)` 均 `ERR_INVAL`；目标为僵尸/已结束 → `ERR_NOENT`。
* sigframe 是内核与用户态分发器之间的 ABI：`{u64 gprs[15]; u64 rip; u64 rflags; u64 rsp; u64 signum;}` 共 152 字节，加上 8 字节的 "禁止 return" 槽（`kernel/include/kernel/signal.h:66-97`）。

### 4.14 系统电源与 panic

| # | 调用 | 门控 | 行为 |
|---|------|------|------|
| 37 | `sys_reboot()` | `ATOM_SYS_SHUTDOWN` | `cli` 后向 0x64 写 0xFE 脉冲 8042 复位线；不返回 |
| 39 | `sys_shutdown()` | `ATOM_SYS_SHUTDOWN` | 先写 ACPI PM1a `0x604 = 0x2000`(S5)，失败回退 8042 复位；不返回 |
| 73 | `sys_halt()` | `ATOM_SYS_SHUTDOWN` | `cli` 后在 `hlt` 循环里停住 CPU，**不动复位线**；不返回 |
| 38 | `sys_panic()` | `ATOM_SYS_DEBUG` | 走统一 panic 路径（调试钩子），不返回 |

四者都在**任何副作用之前**查表（`syscall.c:1361-1366`、`1403-1404`、`1438-1442` 与 `sys_halt`）；未授权返回 `ERR_NOCAP`，机器继续运行。`init` 的 P2 自检正是用 "未授权 reboot/halt 返回 `ERR_NOCAP` 而不是复位/停机" 来证明门控位置正确（`user/services/init/main.c` 的 `TestP2RebootUnauthorized` / `TestP2HaltUnauthorized`，P2 Gate 6/6）。

**halt 与 reboot/shutdown 的区别**：`reboot` 脉冲 8042 复位线、`shutdown` 先请求 ACPI S5 再回退复位，两者都会重启机器；`halt` 只把 CPU 停在 `hlt`（`IF=0`，因此没有任何设备能再唤醒它），是 shell `power halt` / `halt` 命令的底层。

授权路径的真实例子：用户服务在 OWNER/ADMIN 登录时用 `CapGrantToSubject(caller, ATOM_SYS_SHUTDOWN, RIGHT_ALL, 0, 0)` 签发，登出时 `CapRevokeByAtom(caller, ATOM_SYS_SHUTDOWN, 0)` 收回（`user/services/user/main.c:260-268`）。

### 4.15 内嵌 blob（SYS_BLOB_GET）

`BlobGet(name, buf, buf_size)` 把内核镜像内嵌的用户态 ELF 按名字取出（`syscall.c:1124-1161`）：

* 名字最多 32 字节（`BLOB_NAME_MAX`，`kernel/include/kernel/blob.h:30`），做有界拷贝；
* 未知名字 `ERR_NOENT`，目标缓冲小于 blob `ERR_OVERFLOW`，指针非法 `ERR_FAULT`；
* 成功返回 blob 字节数，是 `ProcessCreate()` 的典型上游（`manager` 就是这么拉起所有服务的）。

### 4.16 内核互斥量

| # | 调用 | 返回 |
|---|------|------|
| 22 | `MutexCreate()` | handle>=1 / `ERR_NOMEM`（表满，共 256 个，`MAX_MUTEXES`） |
| 23 | `MutexLock(handle)` | `OK` / `ERR_NOENT`（句柄失效；被打断也走这条）/ `ERR_BUSY`（已是属主） |
| 24 | `MutexUnlock(handle)` | `OK` / `ERR_NOENT` / `ERR_DENIED`（非属主） |
| 25 | `MutexDestroy(handle)` | 恒 0；唤醒所有等待者（它们收到 `ERR_NOENT`） |

互斥量是可阻塞、FIFO、非递归的内核对象，解锁时把所有权**直接交接**给下一个等待者（`kernel/include/kernel/mutex.h:15-32`）。线程退出时 `MutexReleaseAll()` 会释放其持有的全部互斥量（单线程最多同时持 16 个，`MAX_HELD_MUTEXES`）。

---

## 五、门控与权限矩阵

### 5.1 设计原则：决策下沉、零 IPC

权限模型的红线写在 `docs/permission_model.md` §四（183-213 行）：**禁止内核在系统调用路径上向用户态 perm-engine 同步查询策略**，因为 syscall handler 常持有自旋锁运行，等待用户态回复会死锁，且延迟不可接受。正确形态是：

```text
授予路径（异步，可阻塞）:
  主体/用户 -> perm-engine 判定 -> cap_grant / cap_create 把"决策结果"
            编码进能力 (atom_id + subject + scope + expiry + quota)

使用路径（同步，不可阻塞）:
  app 调 sys_set_time
    -> syscall handler: CapLookupByAtom(caller_table, caller_subject,
                                         ATOM_SYS_SET_TIME, scope)
    -> 命中且未过期: 放行；否则 ERR_NOCAP (= EPERM)
  无任何 IPC 往返
```

内核侧实现就是 `CapLookupByAtom()`（`kernel/include/kernel/cap.h:210-238`）：按 `(subject, atom, scope)` 纯表扫描，命中即授权；条目惰性过期（过期就地撤销并跳过），配额耗尽的条目的 `type` 已归零，因此 "type != CAP_TYPE_NONE" 本身就是配额存活判定；多个命中时取最小表下标，保证确定性。**能力表就是决策缓存。**

### 5.2 atom 门控矩阵（内核表查找）

| syscall | 需要的 atom | 代码位置 |
|---------|-------------|----------|
| `SYS_CAP_CREATE_ATOM` (57) | `ATOM_CAP_GRANT_SELF` | `syscall.c:300-304` |
| `SYS_CAP_REVOKE_BY_ATOM` (59) | `ATOM_SERVICE_MANAGE`（查**调用者**的表） | `syscall.c:350-353` |
| `SYS_CAP_GRANT_TO_SUBJECT` (60) | `ATOM_SERVICE_MANAGE` | `syscall.c:382-386` |
| `SYS_CAP_HAS_ATOM` (66) | `ATOM_SERVICE_MANAGE` | `syscall.c:416-419` |
| `SYS_SET_TIME` (61) | `ATOM_SYS_SET_TIME` | `syscall.c:916-920` |
| `SYS_REBOOT` (37) / `SYS_SHUTDOWN` (39) | `ATOM_SYS_SHUTDOWN` | `syscall.c:1361-1366` / `1403-1404` |
| `SYS_PANIC` (38) | `ATOM_SYS_DEBUG` | `syscall.c:1441-1442` |
| `SYS_FB_GET_INFO` (44) / `SYS_FB_MAP` (45) | `ATOM_SERVICE_MANAGE` | `syscall.c:671-673` / `699-701` |
| `SYS_SHM_CREATE` (67) / `SYS_SHM_MAP` (68) | `ATOM_SERVICE_MANAGE` | `kernel/mm/shm.c:113-115` / `177-179` |
| `SYS_PCI_CFG_READ` (71) / `SYS_PCI_CFG_WRITE` (72) | `ATOM_SERVICE_MANAGE` **且** `CAP_TYPE_PCI_DEV` | `kernel/syscall/pci.c:289-292` / `311-314` |
| `SYS_KILL` (49)，目标非本进程时 | `ATOM_SERVICE_MANAGE` | `syscall.c:1490-1496` |
| `SYS_CAP_CREATE` (1)，type 为 `PCI_DEV`/`IO_PORT` 时 | `ATOM_SERVICE_MANAGE` | `syscall.c:237-240` |

atom 枚举共 24 个取值：`ATOM_NONE=0`（表示"无 atom 语义"）+ **23 个真实权限原子** `ATOM_SYS_SHUTDOWN=1` 到 `ATOM_CAP_GRANT_SELF=23`，`ATOM_MAX=24` 是上界哨兵，定义在 `kernel/include/kernel/atom.h:30-61`，与 `docs/permission_model.md` §六（261-285 行）一致。目前只有上表中的若干 atom 被内核门控实际使用；其余（相机、麦克风、网络、包管理等）由用户态 perm-engine 负责，内核仅提供 `atom_id` 的承载与查询原语。

### 5.3 cap 类型门控矩阵（句柄/范围查找）

| syscall | cap 类型 | 需要的权利 | 匹配规则 |
|---------|----------|-----------|----------|
| `SYS_MAP_MEMORY` (8) | `CAP_TYPE_MEM` | `RIGHT_WRITE` | 按句柄 |
| `SYS_BIND_IRQ` (32) / `SYS_UNBIND_IRQ` (33) | `CAP_TYPE_IRQ` | `RIGHT_READ` | 句柄 + `obj_id == irq` |
| `SYS_DEBUG_GETCHAR` (29) | `CAP_TYPE_IO_PORT` | `RIGHT_READ` | 覆盖 0x3F8 |
| `SYS_IO_READ8/READ16` (35/69) | `CAP_TYPE_IO_PORT` | `RIGHT_READ` | `base <= port < base+count` |
| `SYS_IO_WRITE8/WRITE16` (36/70) | `CAP_TYPE_IO_PORT` | `RIGHT_WRITE` | 同上 |
| `SYS_BLK_READ/WRITE/INFO` (62/63/64) | `CAP_TYPE_PCI_DEV` | `RIGHT_READ\|RIGHT_WRITE` | `obj_id == disk` |
| `SYS_PCI_CFG_READ/WRITE` (71/72) | `CAP_TYPE_PCI_DEV` | `RIGHT_READ\|RIGHT_WRITE` | `obj_id == index` |
| `SYS_CAP_GRANT` (2) | 任意 | 源能力带 `RIGHT_GRANT` | 按句柄，权限取交集 |

这些扫描都**内联实现了惰性过期**（`syscall.c:1247-1270`、`pci.c:240-256`、`virtio_blk.c:550-568`），所以一个到期的设备能力不会继续放行。

### 5.4 非能力型约束（易被忽略）

| syscall | 约束 |
|---------|------|
| `SYS_NOTIFY` (30) | 目标线程必须属于调用者进程，否则 `ERR_NOENT` |
| `SYS_THREAD_SET_CTX` (54) | 目标 TID 必须属于调用者进程，且 `ctx_size == 72` |
| `SYS_THREAD_JOIN` (14) | 不能 join 自己（`ERR_INVAL`） |
| `SYS_PROCESS_WAIT` (41) | 不能等自己（`ERR_INVAL`） |
| `SYS_CAP_CREATE` (1) | 不得自铸 `CAP_TYPE_DAC_OVERRIDE`（`ERR_DENIED`） |
| `SYS_UNMAP_MEMORY` / `SYS_MAP_MEMORY` | 只能操作**当前进程**的地址空间与其堆守护页 |
| `SYS_PROCESS_CREATE` (21) | 任何人都能创建进程，但只有持 `ATOM_SERVICE_MANAGE` 的调用者才能触发 blob 身份种子 |

### 5.5 门控的自检锚点（可复现证据）

| 锚点 | 位置 | 证明什么 |
|------|------|----------|
| 内核给 init 预置 `ATOM_CAP_GRANT_SELF`/`ATOM_SERVICE_MANAGE`/`ATOM_SYS_DEBUG` | `kernel/kernel_main.c:245-277` | 初始信任根由内核签发 |
| 未授权 reboot → `ERR_NOCAP`，虚拟机不复位 | `user/services/init/main.c:1606-1616` | 门控在副作用之前 |
| 授权 `set_time`：先 `CapCreateAtom(ATOM_SYS_SET_TIME,...)` 再 `OsSetTime()` 返回 0 | `user/services/init/main.c:1583-1602` | 能力即决策 |
| 过期能力 `CapConsume` 返回 `ERR_NOENT` 且不可复活 | `user/services/init/main.c:491-494` | 惰性过期 |
| 配额能力第 3 次消费失败 | `user/services/init/main.c:500-506` | 配额语义 |
| scope 撤销只命中对应 scope | `user/services/init/main.c:529-538` | scope_hash 匹配 |
| 登录用户时按角色签发/收回 `ATOM_SYS_SHUTDOWN` | `user/services/user/main.c:260-268` | 管理面通过 `CapGrantToSubject`/`CapRevokeByAtom` 授予与撤权 |
| `fs_virtio_blk_driver` 用 `CapHasAtom(caller, ATOM_SERVICE_MANAGE)` 控制管理操作 | `user/services/vfs/fs_virtio_blk_driver.c:937` | 管理面查询接口的真实用途 |

---

## 六、关键常量与限制

### 6.1 常量总表

| 常量 | 值 | 含义 / 影响 | 出处 |
|------|----|-------------|------|
| `MAX_THREADS` | 2048 | 线程表、进程表、每进程 cap 表指针数组、用户栈区的规模基准 | `kernel/include/kernel/types.h:116` |
| `USER_STACK_PAGES` | 4（16 KiB） | 每线程用户栈页数；同时决定 `ASLR_STACK_BLOCK` | `types.h:122` |
| `MAX_PORTS` | 256 | IPC 端口表规模；端口号 1..256 | `types.h:123` |
| `MAX_CAPS` | 1024 | 每进程能力槽位数（cap 表约 73 KB，19 页） | `types.h:124`、`cap.h:64-75` |
| `MAX_MSG_SIZE` | 4096 | 单条 IPC 消息上限 | `types.h:125` |
| `MAX_PATH_LEN` | 256 | 路径长度上限（VFS 侧） | `types.h:126` |
| `MAX_HELD_MUTEXES` | 16 | 单线程同时持有的互斥量上限（退出时交接） | `types.h:129` |
| `PAGE_SIZE` | 4096 | 页大小 | `types.h:132` |
| `SYS_COUNT` | 73 | 分发表长度（`SYS_PCI_CFG_WRITE + 1`） | `kernel/include/kernel/syscall.h:34` |
| `USER_PTR_MAX` | `0x0000800000000000` | 用户态地址上界；所有用户指针校验的分界线 | `kernel/include/kernel/vmm.h:30` |
| `HEAP_USER_SIZE` | `0x10000000`（256 MB） | 每进程堆区大小 | `vmm.h:46` |
| `ASLR_HEAP_BASE_MIN/MAX` | `0x70000000` / `0x78000000`（不含） | 堆基址随机范围，64 KB 对齐 | `kernel/include/kernel/rng.h:104-106` |
| `ASLR_STACK_BASE/END` | `0x90000000` / `0x100000000` | 线程栈区；每地址空间一块 `ASLR_STACK_BLOCK` | `rng.h:74-76` |
| `ASLR_STACK_BLOCK` | `MAX_THREADS*USER_STACK_PAGES*PAGE_SIZE` = 32 MB @2048 | 每进程栈区步长 | `rng.h:76` |
| `BLOB_NAME_MAX` | 32 | blob 名字缓冲（含 NUL） | `kernel/include/kernel/blob.h:30` |
| `BLOB_MAX_ENTRIES` | 28 | 内嵌 blob 注册上限 | `blob.h:31-32` |
| `ELF_MAX_LOAD_SEGS` | 8 | libos ELF 解析接受的 PT_LOAD 段上限 | `user/lib/libos/elf_parse.h:41` |
| `PROC_IMAGE_MAX_SEGS` | 16 | 内核接受的段描述符上限 | `kernel/syscall/process_desc.c:68` |
| `SHM_MAX_POOLS` | 8 | 共享池个数 | `kernel/mm/shm.c:73` |
| `SHM_MAX_PAGES` | 1024（4 MiB） | 单池页数上限 | `shm.c:74` |
| `PORT_REGISTRY_SIZE` | 64 | 端口名字注册表容量 | `kernel/ipc/ipc.c:88` |
| `PENDING_POOL_SIZE` | 128 | 挂起消息池大小（耗尽 → `ERR_NOMEM`） | `ipc.c:87` |
| `MAX_MUTEXES` | 256 | 内核互斥量表规模 | `kernel/include/kernel/mutex.h:40` |
| `VSPACE_FLOOR` | `0x40000000`（1 GiB） | `vspace_alloc` 扫描地板 | `kernel/mm/vspace.c:77` |
| `PCI_CONFIG_MAX_DEVS` | 64 | PCI 枚举缓存上限 | `kernel/syscall/pci.c:78` |
| `PCI_MAX_BARS` | 6 | BAR 数组长度 | `kernel/include/kernel/pci.h:34` |
| IRQ 线数 | 16（0..15） | `CAP_TYPE_IRQ` 的 `obj_id` 上界；IRQ0/IRQ2 内核保留 | `syscall.c:210`、`irq.h:31-35` |
| `NSIG` | 64 | 信号编号上界（1..63 有效） | `kernel/include/kernel/signal.h:64` |
| `DEBUG_LOG_MAX` | 512 | 单次 `DebugLog` 拷贝上限 | `syscall.c:104` |
| 调试限速 | 2048 B/tick，桶上限 4096 B | 令牌桶参数 | `syscall.c:105-107` |
| `VIRTIO_BLK_SECTOR_SIZE` | 512 | 扇区大小（`blk_info_t.sector_size`） | `kernel/include/kernel/blk.h:33-36` |
| PIT 频率 | 100 Hz（`1193182/100`） | 首次 `syscall_dispatch()` 时编程 | `syscall.c:1541-1554` |

### 6.2 地址空间布局（用户态视角）

```text
0x0000000000000000
        |  未映射（NULL 陷阱区；所有 syscall 拒绝 addr == 0）
0x0000000000400000   服务 ELF 镜像（ET_EXEC，scripts/user.ld 链接）
0x0000000004000000   ASLR_BOOT_STACK_BASE（init 引导栈保留区）
0x0000000010000000   固定测试映射 [0x10000000, 0x30000000)
0x0000000070000000   堆基址随机区间起点（64 KB 粒度，含基址下方 1 个守护页）
0x0000000078000000   堆基址随机区间终点（不含）；堆区大小固定 256 MB
0x0000000090000000   线程栈区起点（每地址空间一块 32 MB @ MAX_THREADS=2048）
0x0000000100000000   线程栈区终点
        ...
0x00007FFFFFFFFFFF   USER_PTR_MAX = 0x0000800000000000（用户态上界）
0xFFFF800000000000   KERNEL_VIRT_BASE（内核直接映射，用户不可达）
```

依据：`kernel/include/kernel/vmm.h:27-46`、`kernel/include/kernel/rng.h:60-106`、`kernel/mm/vspace.c:72-79`。`SYS_VSPACE_ALLOC` 只会在上述保留区**之外**分配虚拟区间。

---

## 七、排错速查

| 现象 | 最可能的原因 | 核对点 |
|------|--------------|--------|
| 受门控的调用返回 `ERR_NOCAP` | 调用者（或其 subject）没持有对应 atom / 设备 cap | 用 `GetSubject()` 确认身份；管理面服务可用 `CapHasAtom(subject, ATOM_SERVICE_MANAGE)` 查询 |
| 全部能力相关调用都从某线程失败 | 该线程没有回填 PID，`process_current()` 落到内核进程（`cap_table == NULL`） | 只用 `ThreadCreate()` 创建用户线程（内核已补 `t->pid`） |
| 调用返回 `ERR_FAULT` 但指针看着没问题 | 指针跨出了 `USER_PTR_MAX`，或区间内有未映射页；内核按**整段**逐页校验 | 检查长度是否跨越了未映射的守护页/空洞 |
| `map_memory()` "成功" 却没映射上 | 该调用失败也返回 0，与地址 0 无法区分 | 用 `< 0` 以外的判定：把返回值与 0 比较并检查可用性；不要映射到 0 |
| IPC 返回 `ERR_NOENT` | 端口不存在（未创建/已随 owner 死亡销毁），或对方进程已死 | `PortGet()` 重新解析名字；确认服务端仍持有端口 |
| IPC 返回 `ERR_INTERRUPTED` | 阻塞期间被 `SIGKILL` 打断 | 这是设计行为，不是 bug |
| `IpcReply` 返回 `ERR_BUSY` | 同一个 token 回复了两次，或该消息不是 call | token 用后即弃 |
| `Sleep(0)` 返回 `ERR_INVAL` | 有意为之（防止负值被解读为超长睡眠） | 用 `ThreadYield()` 让出 |
| 端口注册返回 `ERR_OVERFLOW` | 名字注册表满（64 项），或老进程的名字没被回收 | 进程退出会由 `IpcCleanupProcess()` 清名 |
| 块设备返回 `ERR_BUSY` | 队列单飞，已有一次 DMA 在飞 | 串行化调用 |
| 调用号 26/27/28/43 返回 `ERR_INVAL` | 这些编号未定义（空洞） | 见 §1.6 |

延伸阅读：`docs/permission_model.md`（atom 门控与决策下沉，§四/§六）、`docs/ops_format.md` §6（内核门控清单）、`docs/kernel_roadmap.md`（P0/P1/P2 与 D4 信号迁移的范围界定）。

> 返回 [文档索引](README.md)
