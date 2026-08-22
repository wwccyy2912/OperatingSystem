# OpSys 内核开发方向决策文档（性能 · 实用性 · 安全）

> 版本：v1.0（定案稿）
> 日期：2026-08-07
> 状态：已定案，作为内核后续开发的路线图依据
> 关联：docs/requirements.md（§1.1 微内核符合性、§6 性能策略）、docs/vfs_design.md（§8 分阶段计划）
> 本文档为架构评审结论的正式化：综合「架构纯粹性」与「性能现实」两个维度，
> 以 **调用频率（Hot Path）× 上下文切换成本** 为评估框架，确定每个组件在
> Ring 0 / Ring 3 的最终归属与开发顺序。

---

## 〇、摘要（TL;DR）

- **框架**：以「调用频率 × 上下文切换成本」取代纯架构主义。高频原语留 Ring 0，
  冷路径/低频事件移 Ring 3。
- **三个定案**：
  1. **内核 Mutex 保留 Ring 0**（推翻纯理论建议）——用户态锁需 2~4 次 syscall +
     调度器介入，开销 3~5 倍；Fast-Path 化（用户态 spinlock 封装）优先于 Futex。
  2. **ELF 加载器、Framebuffer 绘制、Signal 移 Ring 3**——均为冷路径/低频，
     移出不损性能，且 Framebuffer 已 90% 完成（term 已用户态绘制）。
  3. **移出有前置依赖**：ELF 需先加 `SYS_VSPACE_ALLOC` 原语；Signal 需先加
     `SYS_THREAD_SET_CTX` 原语（用户态无法改他人线程栈帧）。**先加原语、再搬服务**。
- **性能优化推迟到 SMP 阶段**：Futex、批量轮询、零拷贝在单核 QEMU 上收益有限，
  不列为当前里程碑。

---

## 一、决策框架：三个维度

### 1.1 性能维度（本次评审新增）

一切 Ring 归属判断以「**调用频率 × 上下文切换成本**」为第一准则：

| 分类 | 定义 | 例 | 归属倾向 |
|---|---|---|---|
| **Hot Path** | 每次操作都走、频率毫秒级以下 | 锁、malloc、IPC 通道、cap 校验 | Ring 0（避免反复 syscall） |
| **Warm Path** | 中等频率、可接受 1 次 syscall | 串口/键盘中断（毫秒级事件） | Ring 3（IPC 通知开销微秒级，可忽略） |
| **Cold Path** | 一次性或启动时执行 | ELF 加载、framebuffer 初始化 | Ring 3（移出不伤运行时性能） |
| **低频异常流** | 异常控制流、极低频 | 信号（Ctrl+C、崩溃） | Ring 3（用户态模拟足够） |
| **批量搬运** | 大量数据搬移 | 像素绘制、DMA 环收发 | Ring 3（零 syscall + SIMD 可用） |

### 1.2 实用性维度

- **不做绝对化微内核**：业界 L4/seL4 内部同样保留快速路径内核锁与高性能 IPC 优化。
  内核 TCB 多几百行（Mutex）换取 `malloc` 与临界区数倍性能，是商业级 OS 值得的
  工程权衡。
- **每个移出项必须无性能代价或纯减法**：不牺牲可运行性换取架构纯洁。

### 1.3 安全维度

- **TCB 最小化只针对「可安全移出」的项**：移除即减攻面（ELF 解析、信号语义、
  像素绘制），且移除过程中不新增特权接口。
- **新增内核原语（vspace/TCB_WriteRegisters）需最小化**：只暴露机制，
  不做格式语义解释。
- **能力门控纪律不变**：任何新 syscall 必须 `cap_lookup(RIGHT_*)` 校验。

---

## 二、架构现状基线（2026-08-07 代码实测）

### 2.1 体量

| 区域 | 行数 | 说明 |
|---|---|---|
| 内核 `kernel/` | 11,008（.c/.S，不含 include 头；含头 14,037） | 含 arch、mm、sched、ipc、cap、syscall、gfx、process、blob |
| 用户态 `user/` | 17,750（.c/.S；含头 21,793） | 服务 12 个 + libc/libos/libfs/libipc/runtime（含新增 elf_parse、device_mgr） |

> 行数口径：`find kernel -name '*.[chS]' | xargs wc -l`。2026-08-16 实测：
> `vspace.c`(374)、`thread_ctx.c`(99)、`pci.c`(247)、`process_desc.c`(270)、`elf_boot.c`(177)；
> ELF 解析攻击面已移出内核（`elf.c` 218 行 → `elf_boot.c` 177 行仅保留 init 加载器 + 用户态
> `user/lib/libos/elf_parse.c` 承担全部解析）。行数回升源于计划内的新原语增量，非 TCB 扩张。

### 2.2 已符合微内核的项（✅ 不动）

| 项 | 代码证据 |
|---|---|
| 设备驱动在 Ring 3 | serial.c / keyboard.c：ring-3 进程 + `io_read8/8` + IRQ 绑定 + notify |
| IRQ 纯转发 | `kernel/ipc/irq.c`（90 行）：静态 16 线表，ISR → `notify()`，不解码语义 |
| 文件系统用户态 | vfs_server + fs_mem_driver 双进程，IPC 挂载握手 |
| 服务管理器用户态 | manager.c 用 SYS_PROCESS_CREATE 拉起全部服务 |
| 能力门控完整 | map_memory/io/irq 全部 `cap_lookup(RIGHT_*)` |
| ASLR | 堆基址随机、栈 canary 随机 |
| 调度/内存内核 | PMM/VMM/sched 在 Ring 0（必需特权） |

### 2.3 偏离项（本次评审处理对象）

| 项 | 现状 | 行数 | 分类 |
|---|---|---|---|
| 内核 Mutex | `kernel/ipc/mutex.c` FIFO 阻塞锁 + 持有跟踪 | 269 | **Hot Path → 留 Ring 0** |
| ELF 加载器 | `kernel/mm/elf.c` + `sys_process_create` 内 elf_load | 218 | **Cold Path → 移 Ring 3** |
| Framebuffer 绘制 | `kernel/gfx/framebuffer.c` fb_fill/fb_puts/fb_printf | 543 | **Cold Path → 移 Ring 3** |
| POSIX 信号 | `SYS_SIGNAL/KILL/SIGRETURN` 内核实现 | — | **低频 → 移 Ring 3** |
| 运行时串口日志 | 14 个内核文件用 `serial_printf` | — | 保留 panic 裸写，常规走用户态 |

---

## 三、Ring 0 / Ring 3 归属决定（最终版）

| 组件 | 最终归属 | 核心理由（性能视角） | 状态 |
|---|---|---|---|
| 调度器 / PMM / VMM | **Ring 0** | 必须特权指令，无替代 | ✅ 定案（现状） |
| 中断转发（IRQ Notify） | **Ring 0**（极简） | 查表转发，纳秒级 | ✅ 定案（现状） |
| 内核 Mutex / 同步原语 | **Ring 0**（Fast-Path） | 高频，用户态模拟 → 多次 syscall | ✅ 定案（本次确认保留） |
| IPC 底层通道 | **Ring 0** | 数据搬运机制 | ✅ 定案（现状） |
| 能力表（Cap）校验 | **Ring 0** | 安全检查必经之路 | ✅ 定案（现状） |
| ELF 加载解析 | **Ring 3** | 冷路径，仅启动时运行 | ✅ 定案（**需先加 SYS_VSPACE_ALLOC**） |
| Framebuffer 绘图代码 | **Ring 3** | 批量内存搬运，零 syscall + SIMD | ✅ 定案（term 已接管，内核仅删代码） |
| 串口/键盘/普通驱动 | **Ring 3** | 低频中断，IPC 开销可忽略 | ✅ 定案（现状） |
| POSIX 信号处理 | **Ring 3**（库） | 低频异常流 | ✅ 定案（**需先加 SYS_THREAD_SET_CTX**） |
| 运行时串口日志 | **Ring 3**；panic 路径保留内核裸写 | 调试时 panic 裸写不经格式化 | ✅ 定案 |
| Futex 化 Mutex | Ring 0（**推迟**） | 单核收益有限，SMP 阶段再做 | ⏸ 推迟到 SMP |
| MMIO 设备映射（SYS_MAP_MEMORY 增强） | Ring 0（**推迟**） | virtio 阶段才需要 | ⏸ Phase 1 配套 |

---

## 四、开发方向：三个维度 × 阶段路线图

### 4.1 性能方向（Ring 0 优化，不搬移）

| 阶段 | 工作 | 说明 | 验证 |
|---|---|---|---|
| **P0** | Mutex Fast-Path：用户态 spinlock 封装 | `libos` 提供 `user_spinlock`（原子 CAS，无竞争 0 syscall），竞争时 `thread_yield()` | ✅ 完成：`user/lib/libos/spinlock.h`（CAS+yield），`malloc` 无竞争 0 syscall |
| **P3** | Futex 化内核 Mutex | 无竞争纯原子 + 竞争排队；**SMP 阶段实施** | ⏸ 推迟到 SMP（单核收益有限） |
| **P3** | IPC 多活动调用 | 当前 `s_active_call` 每端口单活动调用，多客户端排队 | ✅ 已完成（v0.2/v0.3：`s_reply_wait` 每端口链表 + 代际令牌 + `ipc_abort_wait`；manager monitor 线程与 shell 并发调 serial 验证通过） |
| **P3** | 零拷贝读路径 | `CAP_TYPE_MEM` 映射文件页，绕开 4096 上限（vfs_design §8.4） | ✅ 已完成（2026-08-22）：内核 `SYS_SHM_CREATE`/`SYS_SHM_MAP`（管理原子门控 + 池表校验 + 只读映射）；fs_mem_driver 共享页池存储（bump 分配，增长迁移堆回退）；vfs `VFS_OP_READ_MAP`（授权重查 + 导出）；libfs `fs_read_map`；P5 回归（映射内容 == chunked 读） |
| **P1 配套** | SYS_MAP_MEMORY 支持设备 MMIO | virtio-blk/net DMA 环前提 | virtio 驱动 |

### 4.2 实用性方向（可运行、可验证优先）

| 阶段 | 工作 | 前置依赖 | 说明 |
|---|---|---|---|
| **P0** | Framebuffer 内核绘制删除 | 无 | ✅ 完成：kernel_main.c 启动画面删除（fb 交给 term）；framebuffer.c 543→170 行（init+query） |
| **P0** | 统一内核 panic 路径 | 无 | ✅ 完成：新增 `kernel/panic.c`（`panic(fmt,...)`：cli → 横幅+原因 → halt）；4 处裸 `cli;hlt` 终止点收敛（idt 内核态异常、stack_chk canary、thread idle 栈失败、FINISHED 兜底）；gdb 破 canary 实测触发 |
| **P1** | 新增 `SYS_VSPACE_ALLOC` | 无 | ✅ 完成：`kernel/mm/vspace.c`（383 行），分配 + 映射 + 错误路径（`ERR_INVAL`），`init` 测试验证通过 |
| **P1** | ELF 加载器移 Ring 3 | SYS_VSPACE_ALLOC | ✅ 完成：`kernel/mm/elf.c` → `elf_boot.c`（仅保留 init 加载器）；解析移入 `user/lib/libos/elf_parse.c`，`sys_process_create` 改描述符式（`proc_image_desc_t`） |
| **P2** | 新增 `SYS_THREAD_SET_CTX` | 无 | ✅ 完成：`kernel/sched/thread_ctx.c` + `thread_ctx.h`（101 行），seL4 `TCB_WriteRegisters` 等价物；`_Static_assert` 锁定 thread_ctx_t 与 context_switch.S 帧布局 |
| **P2** | Signal 移 Ring 3 | SYS_THREAD_SET_CTX | ✅ 完成：`user/runtime/signal_user.c`（dispatcher + 用户态 handler 表）；内核 `signal.c` 仅留投递机制；`sigrestore.S` 删除；`SYS_SIGNAL` 改注册 dispatcher；hello 信号自测 + 49 项回归通过 |
| **P2** | 常规串口日志走用户态 serial 服务 | 无 | ✅ 完成：内核保留 `serial_puts` 仅 panic/early-boot |

### 4.3 安全方向（TCB 削减 + 原语最小化）

| 阶段 | 工作 | 说明 | 量化目标 |
|---|---|---|---|
| **P0** | 删内核 Framebuffer 绘制 | 减攻面（像素/字体渲染逻辑出内核） | ✅ 完成：内核 -373 行（framebuffer.c 字库+绘制函数、kernel_main.c 启动画面） |
| **P1** | ELF 解析出内核 | 恶意 ELF 的解析攻击面移出 TCB | ✅ 完成：`elf.c` → `elf_boot.c`（init-only 加载器，181 行）；全部解析逻辑移入用户态 `elf_parse.c` |
| **P2** | Signal 语义出内核 | 信号处理逻辑用户态化 | ✅ 完成：语义在 `user/runtime/signal_user.c`（handler 表/SIG_IGN/SIG_DFL 策略/默认动作），内核仅保留 checkpoint 投递 + SIGKILL 强制退出 |
| **P1 配套** | MMIO 能力化 | 设备内存映射走 `CAP_TYPE_MEM`/`CAP_TYPE_PCI_DEV` 门控，不开放裸映射 | ✅ 完成：新增 PCI 枚举 syscall（`SYS_PCI_GET_COUNT`/`SYS_PCI_GET_DEVICE`），`device_mgr` 服务（Ring 3）经 IPC 提供 PCI 目录 |
| **长期** | 崩溃恢复策略 | vfs -ESTALE 重开（vfs_design §十之一）、服务重启语义 | 服务级容错 |

**TCB 目标**：当前内核 8,644 行 → 削减约 700~800 行（P0~P2 合计），
 保留 Mutex ~300 行为 Fast-Path 原语（有意的工程权衡，非残留）。
**P0 实测**：8,644 → **7,792 行（-852 行）**，已低于 7,900 预算线。
**P0 后实测**（含 panic 统一）：**7,824 行**（panic.c +40，内联 hlt 终止代码移除 -8）。
 统一故障路径为有意保留（可观测性/可维护性），不计入削减。

---

## 五、性能预算与验证方法

### 5.1 基准（QEMU，沿用串口 + screendump + 解码脚本流程）

| 指标 | 当前基线 | 目标 |
|---|---|---|
| malloc 单次 syscall 次数 | 2（lock+unlock） | ✅ 0（无竞争 fast-path，P0 达成） |
| IPC 单次 call 延迟 | — | ✅ 微秒级（100k 往返实测：100,000 calls / 11,755 ticks） |
| vfs 写 4KiB 块 | 已验收（4032 分块） | 不变 |
| 内核行数 | 8,644 | ✅ 11,008（P1/P2 原语增量：vspace/thread_ctx/pci/process_desc/elf_boot 落地；ELF 解析攻击面已移出） |
| 线程容量 | MAX_THREADS=256 | ✅ 1024（`types.h` 提升；1000 线程压力测试通过） |

### 5.2 回归保障

- 每次 Ring 归属变更后：`make iso` 干净构建 + 现有验收（31 项经典内核测试；总回归
  49 项 = 经典 31 + P1 10 + P2 Gate 3 + P2V 4 + KBD 1）全量回归。
- 用户态化组件必须保持对外行为不变（ELF 加载结果、信号语义、启动画面时序）。

---

## 六、已定案决策记录

| 编号 | 决策 | 结论 | 理由 | 状态 |
|---|---|---|---|---|
| D1 | 内核 Mutex 归属 | **保留 Ring 0**，Fast-Path 化 | 用户态锁 3~5 倍开销；malloc 高频 | ✅ 定案（推翻纯理论建议） |
| D2 | ELF 加载器归属 | **移 Ring 3** | 冷路径零性能损失；减 TCB | ✅ 定案（前置 SYS_VSPACE_ALLOC） |
| D3 | Framebuffer 绘制归属 | **移 Ring 3** | 批量搬运零 syscall + SIMD；term 已接管 | ✅ 定案（内核仅删代码） |
| D4 | POSIX 信号归属 | **移 Ring 3**（库） | 低频异常流 | ✅ 定案（前置 SYS_THREAD_SET_CTX） |
| D5 | 设备驱动归属 | **Ring 3**；virtio 用共享内存+批量轮询 | 低频中断 IPC 可忽略；高吞吐用轮询 | ✅ 定案（现状 + Phase 1 折衷） |
| D6 | Futex 化 Mutex | **推迟到 SMP** | 单核 QEMU 收益有限 | ⏸ 推迟 |
| D7 | MMIO 映射增强 | **Phase 1 配套** | virtio 驱动前置依赖 | ⏸ 推迟 |

---

## 七、开放问题（后续评审）

1. **多核模型**：单核 → SMP 的调度/锁/缓存一致性设计（Futex、irq 绑定表
   `s_irq_bindings` 均需重审）。
2. **IPC 并发模型**：`s_active_call` 单活动调用 → 多活动调用池 ✅ 已完成（v0.2/v0.3，`s_reply_wait` 链表 + 代际令牌；`ipc_abort_wait` 处理强杀）。残留：无。
3. **崩溃恢复**：内核侧已落地（2026-08-21：`process_reap` → `ipc_cleanup_process`
   销毁死亡进程端口、唤醒阻塞对端为 ERR_NOENT、释放注册名；`irq_cleanup_process`
   立即释放 IRQ 线；回归新增 crashpeer 对端死亡测试）。**服务侧** -ESTALE 重开与
   manager 全服务重启策略待续（vfs 句柄失效语义）。
4. **用户态 loader 的归属进程**：manager 内嵌 vs 独立 loader 服务（Phase 1 编码前定）。

## 七之二、生产加固记录（2026-08-21，投产前硬化）

| 项 | 内容 | 验证 |
|---|---|---|
| FPU/SSE 状态保存恢复 | `fpu_switch` 从空操作恢复为急切 fxsave/fxrstor；每线程槽位初始化 x86 默认值；`fpu_used` 死字段移除 | 新增 FPU 双线程交错 SSE2 回归测试（32/32 起） |
| SYS_PANIC 门控 | 任意 Ring 3 进程可一键崩溃内核（DoS）→ 门控 `ATOM_SYS_DEBUG`（init 种子） | P2 门控套件 3→5 项 |
| 管理原子种子提权链 | 应用 blob_get("perm")+process_create 逐字节副本可获 `ATOM_SERVICE_MANAGE`（完全管理接管）→ 种子加调用者门控（仅持管理原子的 spawner 可种），manager 加入种子表 | 回归全绿 |
| sys_kill 门控 | 任意进程可 SIGKILL 关键服务 → 自杀恒允许、杀他人须 `ATOM_SERVICE_MANAGE` | R2.6 kill 错误路径保持 |
| sys_notify 门控 | 可向外部线程注入虚假通知 → 目标限本进程线程 | P2 notify 测试 |
| sys_debug_getchar 门控 | 任意进程可窃取/消耗控制台输入 → 仅 COM1 IO 能力持有者可读 | P2 debug_getchar 测试 |
| sys_fb_get_info/map 门控 | 任意进程可涂改屏幕 → 门控 `ATOM_SERVICE_MANAGE`（term 持有） | term 渲染正常 |
| IPC 对端死亡 | 服务崩溃时阻塞的 ipc_call 调用方永久挂死、端口注册名泄漏、IRQ 线不释放 → `process_reap` 统一清理（`ipc_cleanup_process`/`irq_cleanup_process`） | 新增 crashpeer 对端死亡回归测试（33/33 起） |
| 服务自动重启 | manager 重启策略从 flaky-only 扩展为 perm/pkg/device_mgr/shell（每服务一 monitor 线程，MAX_RESTARTS=3）；配合端口/IRQ 清理可干净重启 | 新增 P3 Crash Recovery 回归（kill pkg → 自动重启 → 端口恢复） |
| force_exit 槽位残留 | `alloc_thread` 漏清 `force_exit`：SIGKILL 线程槽位回收后，新线程在首个检查点被静默杀死（code 0）→ 补清零 | P3 测试一次重启成功（原为两次） |
| blob 注册 fail-fast | BLOB_MAX_ENTRIES 静默溢出致服务缺失（sbox_demo_noperm 注册失败被吞）→ 上限 22 + 注册失败 panic | 全量 smoke 恢复 |
| v0.4 图形最小闭环 | `window_demo` 服务：经 term（显示所有者）渲染 3 窗口 + 标题栏焦点标记 + 状态栏，键盘 1/2/3 切换焦点、q 退出（输入走 keyboard 焦点路由）——窗口概念在现有安全架构内的闭环；真实合成器/窗口注册表待续 | `scripts/verify_window_demo.py`（VGA 解码验证窗口渲染 + 焦点切换） |
| 资源耗尽优雅降级 | 新增 P4 套件：IPC 超长消息 → ERR_INVAL（边界）；线程表耗尽（1024）→ ERR_NOMEM 且 join 全部释放后可恢复（优雅降级，不崩溃） | init 回归 54→56 项 |
| 零拷贝读路径（P3 完成） | 内核 `SYS_SHM_CREATE`/`SYS_SHM_MAP`（67/68，管理原子门控 + 池表防任意物理映射 + 只读非执行映射）；fs_mem_driver 共享池存储（System blob 池化 + Users 增长迁移堆回退）；vfs `READ_MAP` 授权重查；`fs_read_map` | P5 回归 1/1（映射 == chunked 读）；smoke 62/62 |

---

## 八、执行顺序总结

```
P0（✅ 已完成 2026-08-07，QEMU 回归通过：fallocate NOSPC@32MiB err -101、
    stat Users 32768 KB used、shell 事后可交互）
  ├─ Mutex Fast-Path：libos 用户态 spinlock 封装（spinlock.h，malloc 无竞争 0 syscall）
  ├─ Framebuffer：删 kernel_main.c 启动画面绘制，framebuffer.c 543→170 行
  └─ 统一 panic：kernel/panic.c 收敛 4 处裸 hlt 终止点；gdb 破 canary 实测 panic halt

P1（✅ 已完成 2026-08-09，QEMU 回归通过：24/24 测试全过）
  ├─ SYS_VSPACE_ALLOC：kernel/mm/vspace.c，分配 + 映射 + 错误路径
  ├─ ELF 加载器用户态化：elf.c → elf_boot.c（init-only）；解析移入 user/lib/libos/elf_parse.c；
  │    sys_process_create 改描述符式（proc_image_desc_t）
  ├─ PCI 枚举 syscall + device_mgr 服务（Ring 3）：SYS_PCI_GET_COUNT/GET_DEVICE
  └─ 常规串口日志用户态化

P2（✅ 已完成 2026-08-21，QEMU 回归通过：49/49 全过 + hello 信号自测）
  ├─ SYS_THREAD_SET_CTX（thread_ctx.c，seL4 TCB_WriteRegisters 等价物）✅
  ├─ MAX_THREADS 256→1024 + 1000 线程 / 100k IPC 压力测试 ✅
  └─ Signal 用户态化（signal_user.c：dispatcher + 用户态 handler 表；
       内核 signal.c 仅留投递机制；sigrestore.S 删除）✅

P3（SMP/远期）
  ├─ Futex 化 Mutex、IPC 多活动调用、零拷贝读
  ├─ 快照/CoW/书签签名（vfs_design §8 Phase 3）
  └─ 多核调度与隔离（requirements.md §6.4）
```
