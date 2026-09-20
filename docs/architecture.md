# OpSys 系统架构总览

> 适用版本：OpSys v0.8-dev（git HEAD 105805d）　|　最后更新：2026-09-19
>
> 本文从整体视角描述 OpSys 的分层、引导链、内核子系统、用户态运行时、服务拓扑与端到端调用链，是阅读其它专题文档前的**全局图景**。本文只描述**已经存在于代码中**的事实，并逐项标注出处；设计意图与决策过程见 [kernel_roadmap.md](kernel_roadmap.md)、[permission_model.md](permission_model.md)、[vfs_design.md](vfs_design.md)。

---

## 一、设计目标与总体取舍

OpSys 的目标不是复刻 POSIX，而是用**最小内核 + 全用户态服务**的方式，把"一个能跑起来、能被验证、权限模型自洽"的系统做完整。由此产生几个贯穿全局的取舍：

| 取舍 | 选择 | 代价 | 依据 |
| --- | --- | --- | --- |
| 内核边界 | 只留机制：调度、内存、IPC、能力 | IPC 往返开销；服务间协作需要协议设计 | [microkernel_audit.md](microkernel_audit.md) |
| 驱动归属 | 全部驱动在 Ring 3（串口/键盘/鼠标/磁盘/网卡） | 中断需转发、DMA 需能力门控 | `kernel/ipc/irq.c`、`user/services/*` |
| 身份模型 | 内核签发 `subject_id`，废弃 UID/GID 与自报身份 | 需要账户服务把 subject 绑定到账户 | `kernel/include/kernel/types.h:55-59` |
| 授权判定 | 内核能力表本地查找（决策下沉） | 能力签发必须由权限引擎完成，需跨进程签发 syscall | `SYS_CAP_GRANT_TO_SUBJECT`、[permission_model.md](permission_model.md) §四 |
| 文件系统接口 | 对象句柄 + 书签，而非 fd + 路径 | 客户端库需要重新设计；无 POSIX 兼容层 | [vfs_design.md](vfs_design.md) |
| 启动依赖 | 服务 ELF 以 blob 嵌入内核镜像 | 镜像变大；更新服务需重建内核 | `kernel/blob/blob.c`、Makefile |
| 信号语义 | 内核只做投递机制，语义在 Ring 3 | 需要 `ThreadSetCtx` + 用户态 dispatcher | `kernel/process/signal.c`、`user/runtime/signal_user.c` |
| 多核 | 暂不支持（`MAX_CPUS = 1`） | 单核性能上限；`ThreadSetAffinity` 仅为接口预留 | `kernel/sched/sched.c` |

---

## 二、分层与边界

### 2.1 五层结构

```
┌────────────────────────────────────────────────────────────────────────────┐
│ L5 应用层    .ops 沙盒应用（manifest 声明权限）· demo · fm 文件管理器       │
├────────────────────────────────────────────────────────────────────────────┤
│ L4 服务层    18 个常驻 Ring 3 进程，各自独立地址空间、独立能力表            │
│              init / manager（引导与监管）                                    │
│              serial / keyboard / net / device_mgr（驱动）                    │
│              vfs / fs_mem_driver / fs_virtio_blk_driver（存储）              │
│              perm / user / policy / pkg（系统）                              │
│              term / wm / gui（界面）· shell（前端）· 各 demo/测试桩          │
├────────────────────────────────────────────────────────────────────────────┤
│ L3 客户端库  libipc（IPC）· libos（syscall + ELF 解析）                       │
│              libfs / libpkg / libtui / libwm / libgui（服务客户端）           │
│              libime（输入法）· libc（标准库）                                 │
├────────────────────────────────────────────────────────────────────────────┤
│ L2 C Runtime crt0.S · init/exit(atexit) · malloc · errno · signal_user       │
│              stack_chk（用户态金丝雀）                                        │
├────────────────────────────────────────────────────────────────────────────┤
│ L1 微内核    kernel_main · arch/x86_64 · mm · sched · process · ipc · cap    │
│              syscall · blob · gfx · panic                                     │
└────────────────────────────────────────────────────────────────────────────┘
```

### 2.2 Ring 0 / Ring 3 归属表

| 能力 | 归属 | 理由 |
| --- | --- | --- |
| 页表 / 物理页 / 地址空间 | Ring 0（`mm/`） | 隔离的基础，不可下放 |
| 线程调度与上下文切换 | Ring 0（`sched/`） | 抢占需要特权 |
| IPC 通道与端口 | Ring 0（`ipc/`） | 跨地址空间搬数据的唯一可信通道 |
| 能力表与校验 | Ring 0（`cap/`） | 授权判定的信任根 |
| 中断向量与 IRQ 转发 | Ring 0（`arch/idt.c`、`ipc/irq.c`） | 中断入口必须在内核 |
| ELF 装载 | Ring 0（`mm/elf_boot.c`）+ 用户态解析（`libos/elf_parse.c`） | 内核只按已解析描述符建映射 |
| 帧缓冲绘制 | **Ring 3**（`term`、`gui`、`libgui`） | 内核只提供 `FB_GET_INFO/FB_MAP` |
| 块设备协议（virtio-blk） | **Ring 3** 驱动；内核保留 legacy 通道供早期引导 | `user/services/vfs/fs_virtio_blk_driver.c` |
| 网卡（PCnet） | **Ring 3**（`net` 服务） | 通过 `SYS_PCI_CFG_*` + `SYS_IO_*` 操作硬件 |
| 文件系统语义 | **Ring 3**（`vfs` + FS 驱动） | 内核不认识"文件" |
| 权限策略 | **Ring 3**（`perm`）+ 内核能力缓存 | 策略可变，判定下沉 |
| 账户与口令 | **Ring 3**（`user`） | 内核不存账户状态 |
| 命令策略 | **Ring 3**（`policy`） | 纯用户态偏好/管理策略 |

---

## 三、引导链

### 3.1 从加电到 Ring 3

```
① GRUB2（boot/grub.cfg）
     · multiboot2 加载 kernel.elf
     · gfxmode=1024x768x32 + gfxpayload=keep → 提供线性帧缓冲
     · loadfont unicode.pf2（gfxterm 必需）
     · serial --unit=0 --speed=115200（GRUB 自身的串口输出）
                 │
② kernel/arch/x86_64/boot.asm
     · 校验 Multiboot2 magic
     · 建立初始页表（恒等映射）→ 开启 PAE + Long Mode
     · 加载 64 位 GDT → 进入 64 位代码
     · 设置栈 → 调用 KernelMain(mboot_info_phys, kernel_phys_start)
                 │
③ KernelMain()（kernel/kernel_main.c:89）
     Stage 1    SerialInit()                     早期串口控制台
     Stage 1b   RngInit() + StackChkRandomize()  启动期熵源 + 金丝雀随机化
     Stage 2    Multiboot2 信息解析（内存映射、帧缓冲）
     Stage 3    GdtInit() / IdtInit()            GDT、IDT、异常向量
     Stage 4    PmmInit() / VmmInit()            物理页管理 + 4 级页表
     Stage 5    SchedInit() / ThreadInit()       CFS 就绪树 + 睡眠链
     Stage 6    CapInit() / IpcInit() / MutexInit()
     Stage 7    SyscallInit()                    SYSCALL/SYSRET 入口
     Stage 8    ProcessInit() → 创建 PID 1（init），分配 subject_id = 1
     Stage 9    BlobGet("init") → ElfBootLoad → 映射用户镜像 + 随机用户栈
     Stage 10   TSS.RSP0 = syscall 内核栈；#DF/NMI 的 IST 栈
     Stage 11   关闭 PIT、屏蔽全部 IRQ → enter_user_mode()（IRETQ 到 Ring 3）
                 │
④ init（PID 1，用户态）
     · RunTests()            经典 syscall 套件（含 bench/stress）
     · InitProtocol()        注册 "init" 端口，验证多线程
     · BlobGet("manager") → ProcessCreate("manager")
     · P1 → P2 → P2VFS → KBD → P3 → P4 → P5 自检套件（详见 testing_guide.md）
     · 任一套件失败 → BootSelftestFail() 打印横幅后停机
     · 全部通过 → "init: ALL SELFTESTS PASSED" → idle（ThreadYield 循环）
                 │
⑤ manager（用户态监管者）
     · 按 s_services[] 顺序 BlobGet + ProcessCreate 拉起各服务
     · 为可重启服务启动 ServiceMonitor 线程（ProcessWait + 重启 ≤3 次）
     · 最后拉起 shell，此后提示符归 shell 所有
```

### 3.2 为什么服务用 blob 而不是从文件系统加载

引导早期还没有任何文件系统（`vfs` 与 FS 驱动本身也是被拉起的服务），因此**服务镜像必须在没有文件系统的前提下可用**。Makefile 的解法是：

```make
# 每个服务先链接成独立 ELF（用户链接脚本 scripts/user.ld，基址 0x400000）
build/user/services/term.elf: build/user/services/term/term.c.o $(USER_SHARED_OBJ)

# 再用 ld -r -b binary 把 ELF 变成目标文件，并用 objcopy 把符号改名成 <svc>_elf_*
build/term_blob.o: build/user/services/term.elf
	cd build && ld -r -b binary -o term_blob_raw.o user/services/term.elf
	objcopy --redefine-sym _binary_user_services_term_elf_start=term_elf_start ... $@

# 所有 blob 一起链进内核
kernel.elf: $(KERNEL_OBJ) $(SVC_BLOBS)
```

于是 `BlobGet("term")` 就是一次内核内的查表 + 拷贝（`kernel/blob/blob.c`），`ProcessCreate` 拿到 ELF 字节流后由用户态 `libos/elf_parse.c` 解析、内核按描述符建映射。

---

## 四、内核子系统

### 4.1 物理内存管理（PMM）

- 页帧位图 + **next-fit 分配游标**，页级分配/释放（`kernel/mm/pmm.c`）。
- 大块内存通过 `PmmAllocPages()` 连续分配（如能力表本身约 73 KB → 19 页）。
- 内存上限由 Multiboot2 内存映射决定；`SYS_GET_FREE_PAGES` 暴露空闲页数（`free` 命令据此显示）。

### 4.2 虚拟内存与地址空间

- 4 级页表（PML4 → PDPT → PD → PT），按需建表；`VmmMap()` / `VmmUnmap()` 提供页级映射（`kernel/mm/vmm.c`）。
- 每个进程持有一个 `addr_space_t`（PML4 物理帧 + 若干元数据）。
- `VspaceAlloc()`（`kernel/mm/vspace.c`，对应 `SYS_VSPACE_ALLOC`）在用户态申请的连续虚拟区间上**预建页表层级**，随后用 `map_memory` 把物理页挂上去——主要给零拷贝读路径（共享页池）和信号栈使用。
- **W^X 强制**：用户链接脚本 `scripts/user.ld` 用显式 `PHDRS` 把 `.text`（R+X）、`.rodata`（R）、`.data/.bss`（R+W）拆成三个 `PT_LOAD`，内核按段 `p_flags` 建映射，杜绝"小程序的段被 ld 合并成 RWX"。
- **共享页池**（`kernel/mm/shm.c`）：`SYS_SHM_CREATE` 分配连续物理页池并映射进调用者（受 `ATOM_SERVICE_MANAGE` 门控），`SYS_SHM_MAP` 把池**只读**映射进客户端地址空间，实现零拷贝文件读。
- **ASLR**（`kernel/arch/x86_64/rng.c` + `kernel/include/kernel/rng.h`）：内核 PRNG 在启动期由硬件熵播种，随后派生三处随机化——
  - 用户堆基址：`[0x70000000, 0x78000000)` 内 64 KB 对齐；
  - 用户线程栈基址：`[0x90000000, 0x100000000)` 内按 `ASLR_STACK_BLOCK` 对齐；
  - 栈保护金丝雀：`__stack_chk_guard` 启动时随机化。

### 4.3 调度器（CFS + 红黑树）

`kernel/sched/sched.c`：

| 结构 | 作用 |
| --- | --- |
| `s_ready_tree` | 以 `vruntime` 为键的**红黑树**；最左节点即下一个运行线程 |
| `s_current[0]` | 当前运行线程（`MAX_CPUS = 1`） |
| `s_sleep_list` | 按绝对唤醒 tick 升序的睡眠链表；每 tick 只看表头 |
| `s_fpu_state[MAX_THREADS][512]` | 每线程 512 字节 `fxsave/fxrstor` 区（16 字节对齐） |

- 权重模型：`vruntime += delta * (NICE_0_LOAD / weight)`，`NICE_0_LOAD = 1024`；优先级越高 → 权重越大 → vruntime 增长越慢 → 分到更多 CPU。步长下限钳到 1。
- **两条进入路径**：PIT IRQ0 → `SchedTick()`（推进时间基 + 唤醒到期睡眠者 + 抢占）；yield/阻塞 → `SchedReschedule()`（**不推进时钟**，让 tick 速率跟随 PIT 而非系统负载）。
- 上下文切换在 `IF=0` 下原子完成：`FpuSwitch()` → `SchedSetKernelGs()`（写 GS base）→ `context_switch()`（含更新 TSS.RSP0 为下一个线程的内核栈顶）。
- Idle（tid 0）永不进就绪树，只在当前线程 BLOCKED/FINISHED 时兜底。

### 4.4 线程与进程

| 项 | 说明 |
| --- | --- |
| `thread_t` | TID/PID、状态、优先级、被保存的寄存器组、内核栈、`syscall_save_rsp`、IPC 阻塞端口、持有的内核互斥锁、通知位图、CFS 的 `vruntime` 与红黑树节点、`force_exit`、`wake_tick`、用户栈物理页 |
| `process_t` | PID、状态、地址空间、能力表、主线程 TID、线程数、名字、退出码与等待者、**堆基址（ASLR）**、**`subject_id`**、**App UUID（128 位）**、信号状态（pending 位图 + Ring 3 dispatcher 入口） |
| 线程状态 | READY / RUNNING / BLOCKED / ZOMBIE / FINISHED |
| 进程状态 | CREATED / READY / RUNNING / ZOMBIE / FINISHED |
| 标识符 | PID 与 subject_id **永不复用**（回收的是表槽位） |
| 线程栈 | 每线程 4 页（`USER_STACK_PAGES = 4`，16 KiB），映射在 ASLR 栈块内：`block_base + tid * 4 * PAGE_SIZE` |
| 进程回收 | `ProcessReap()` 会清理端口、注销端口名、唤醒被阻塞的对端（`IpcCleanupProcess`） |

**用户态 ELF 解析分工**：`SYS_PROCESS_CREATE` 接收的是**已解析的进程镜像描述符**（`kernel/include/kernel/proc_image.h`），解析动作由用户态 `libos/elf_parse.c` 完成。好处是内核不需要一个完整的 ELF 解析器，且将来切换 PIE 只需改用户态解析器（`kernel/include/kernel/rng.h` 里 `ASLR_ELF_BASE_*` 已为 PIE 预留区间）。

### 4.5 IPC

| 原语 | 语义 |
| --- | --- |
| `SYS_IPC_SEND` | 向端口发送（端口无接收者则阻塞） |
| `SYS_IPC_RECV` | 从端口接收（阻塞），返回 `tok`（若该消息是一次 call） |
| `SYS_IPC_CALL` | 同步调用：发送请求并阻塞等待应答 |
| `SYS_IPC_REPLY` | 携带 `tok` 应答，唤醒对应调用者 |
| `SYS_IPC_RECV_FROM` | 同 `RECV`，并额外获得**内核填充的发送方 subject** |

关键数据结构（`kernel/include/kernel/ipc.h`）：

```c
typedef struct {
    port_t dest_port; port_t src_port;
    u32    msg_type;  u32    msg_len;
    u64    caps[4];   u32    cap_count;   /* 能力转移字段（预留） */
} ipc_header_t;

typedef struct {
    port_t    port_id;
    tid_t     owner_tid;  pid_t owner_pid;
    thread_t *wait_queue;                 /* 阻塞等待该端口的线程 */
    bool      in_use;
} port_entry_t;
```

- **端口表** `MAX_PORTS = 256`；消息体 `MAX_MSG_SIZE = 4096` 字节。
- **应答等待链**：每个端口的应答等待是以 token 标识的链，token 带**代际（generation）**，因此旧 token 不会误唤醒新调用者；这解除了早期"每端口只能有一个在途调用"的限制（`user/services/manager/manager.c` 头部注释）。
- **端口名注册表**：`SYS_PORT_REGISTER` / `SYS_PORT_GET` 提供全局名字服务，服务启动时注册自己（如 `"term"`、`"vfs"`），客户端按名解析端口号。
- **能力不随消息传递**：`ipc_header_t` 中的 `caps[]` 字段是预留设计；当前版本的能力签发走 `SYS_CAP_GRANT_TO_SUBJECT`（权限引擎定向写入目标进程的能力表）。
- **超长报文**：超过 `MAX_MSG_SIZE` 的 `IpcCall` 返回 `ERR_INVAL`（P4 自检覆盖）。

### 4.6 能力系统

`kernel/cap/cap.c` + `kernel/include/kernel/cap.h`：

```c
typedef struct {
    cap_t      handle;   cap_type_t type;   rights_t rights;
    u64        obj_id;   u64 obj_ptr;       u32 ref_count;
    u16          atom_id;      /* ATOM_* 语义索引；0 = 无语义 */
    subject_id_t subject;      /* 持有者 */
    u64          expiry_ticks; /* 0 = 永久；绝对 tick */
    u32          quota;        /* 0 = 无限；>0 = 剩余次数 */
    u64          scope_hash;   /* 0 = 不限制 */
} cap_entry_t;
```

- **类型**：THREAD / PORT / MEM / IRQ / IO_PORT / **PCI_DEV** / SERVICE / KERNEL / DAC_OVERRIDE。
- **权限位**：`RIGHT_READ / RIGHT_WRITE / RIGHT_EXEC / RIGHT_GRANT`。
- **表**：每进程一张，`MAX_CAPS = 1024`，动态分配（约 73 KB = 19 页），槽位带**代际计数器**防止旧句柄复活。
- **校验**：`CapLookup()` 逐项检查 rights → atom → 惰性过期（过期即就地吊销并视作不存在）→ quota → scope。
- **吊销**：`SYS_CAP_REVOKE_BY_ATOM(subject, atom, scope_hash)` 跨**全部**进程能力表批量吊销，是"按语义撤销授权"的基础（`perm` 的 REVOKE 即用它）。
- **跨进程签发**：`SYS_CAP_GRANT_TO_SUBJECT(subject, atom, rights, expiry, quota)` 按 subject 解析目标进程并把能力写进它的表——权限引擎的"决策 → 能力"编码动作；配套只读查询 `SYS_CAP_HAS_ATOM`（受 `ATOM_SERVICE_MANAGE` 门控）供管理面使用。

### 4.7 中断、异常与通知

- **IDT**（`kernel/arch/x86_64/idt.c`）：异常向量 + 硬件 IRQ；`#DF`（double fault）与 NMI 使用独立 **IST 栈**，避免栈已损坏时二次崩溃。
- **PIC/PIT**：IRQ 屏蔽/解除、100 Hz 定时；Ring 3 切换前屏蔽全部 IRQ，由第一次系统调用重新打开（`EnableSchedulerOnce()`）。
- **IRQ 转发**（`kernel/ipc/irq.c`）：`SYS_BIND_IRQ(cap, irq, mask)` 把某个 IRQ 绑定到某个线程的**通知位（notification bit）**上；中断到来时内核只置位并唤醒等待者，**不做任何设备处理**——设备逻辑全在用户态驱动线程（`serial`、`keyboard` 都采用"服务线程 + IRQ 线程"结构）。
- **异步通知**（`kernel/ipc/notify.c`）：`SYS_NOTIFY` / `SYS_WAIT_NOTIFICATION`，seL4 风格的位掩码信号。
- **内核互斥锁**（`kernel/ipc/mutex.c`）：`SYS_MUTEX_CREATE/LOCK/UNLOCK/DESTROY`；线程持有的锁记录在 TCB 里（`held_mutexes`），线程退出时做**锁移交**，避免死锁。
- **异常与 panic**（`kernel/panic.c`）：打印寄存器现场与栈回溯；`SYS_PANIC` 是 shell `panic` 命令使用的测试钩子。

### 4.8 信号：机制在内核、语义在用户态

这是 Ring 0/3 切分的一个典型样本（`kernel/include/kernel/process.h` 注释、`kernel_roadmap.md` D4/P2）：

- **内核只做**：维护进程级 `sig_pending` 位图；投递时把被打断的用户上下文快照成一个 **sigframe** 压到用户栈，并把 `RIP` 改到用户态注册的 `sig_dispatcher`、`RDI` 指向 sigframe；`SYS_SIGRETURN` 恢复现场。
- **用户态负责**：handler 表（`user/runtime/signal_user.c`）、`SIG_IGN`/`SIG_DFL` 语义、默认动作（终止/忽略）、跳转到用户 handler 并返回。
- `signal()` 本身**不是系统调用**——它只在运行时的用户内存里换一个槽位；只有"注册 dispatcher"这一次用 `SYS_SIGNAL`。

### 4.9 其它内核设施

| 设施 | 位置 | 说明 |
| --- | --- | --- |
| Blob 表 | `kernel/blob/blob.c` | `SYS_BLOB_GET(name, buf, size)` 从嵌入镜像里取出服务 ELF |
| Framebuffer | `kernel/gfx/framebuffer.c` | `SYS_FB_GET_INFO`（几何/格式/是否为 VGA 文本）与 `SYS_FB_MAP`（映射物理帧缓冲），**不做绘制** |
| PCI | `kernel/syscall/pci.c` | 开机枚举 PCI 设备（`SYS_PCI_GET_COUNT/GET_DEVICE`）+ 配置空间读写（`SYS_PCI_CFG_READ/WRITE`，受 `CAP_TYPE_PCI_DEV` 门控） |
| RTC | `kernel/arch/x86_64/rtc.c` | `SYS_GET_RTC_TIME` 读、`SYS_SET_TIME` 写（写受 `ATOM_SYS_SET_TIME` 门控） |
| virtio-blk | `kernel/arch/x86_64/virtio_blk.c` | 内核内 legacy 块设备通道（早期引导可用），正常路径由 Ring 3 驱动使用 |
| 栈保护 | `kernel/arch/x86_64/stack_chk.c` | 内核金丝雀；`-mstack-protector-guard=global` 是因为 freestanding 内核没有 TLS |
| PRNG | `kernel/arch/x86_64/rng.c` | 启动期播种 + `RngMix` 混合熵；ASLR 与 App UUID 都取自它 |

---

## 五、用户态运行时

`user/runtime/` 提供每个用户进程都会链接的最小运行时：

```
_start (crt0.S)
  ├─ 取堆基址（SYS_GET_HEAP_BASE，惰性）
  ├─ 清 .bss
  ├─ 注册信号 dispatcher（SYS_SIGNAL）
  ├─ 遍历 .init_array  ← 全局构造函数
  ├─ main(argc, argv)
  ├─ 遍历 .fini_array  ← 全局析构（倒序）
  └─ exit(code) → 跑 atexit 链 → SYS_THREAD_EXIT
```

| 组件 | 文件 | 要点 |
| --- | --- | --- |
| 启动代码 | `crt0.S` | 唯一汇编入口；调用 `__libc_init` 与 `main` |
| 全局构造/析构 | `init.c` / `exit.c` | `.init_array` / `.fini_array` / `atexit` 链 |
| 堆 | `malloc.c` | 大小分桶 + 空闲块快速判定（O(1) 快路径）+ **就地 realloc 扩展**；堆基址来自内核 ASLR；带守卫页 |
| 错误码 | `errno.c` | 系统调用负返回值 → `errno` |
| 信号 | `signal_user.c` | Ring 3 dispatcher、handler 表、sigframe 解析 |
| 栈保护 | `stack_chk.c` | 用户态 `__stack_chk_fail` |

> 详细设计（含 malloc 数据结构与分配算法）见 [runtime_design.md](runtime_design.md) 与 [runtime_quick_ref.md](runtime_quick_ref.md)。

---

## 六、服务拓扑

```
                          init (PID 1, subject 1)
                                   │ BlobGet + ProcessCreate
                                   ▼
                              manager ──────────────┐ 监管线程（ProcessWait + 重启）
                                   │                 │
      ┌────────────┬───────────────┼─────────────┬───┴────────┬─────────────┐
      ▼            ▼               ▼             ▼            ▼             ▼
   serial       term          keyboard        vfs         perm          gui
   (COM1)    (framebuffer   (IRQ1 + PS/2   (命名空间)   (权限引擎)   (像素合成器)
              + perm.ui)     键盘/鼠标)         │
                                   ┌───────────┴───────────┐
                                   ▼                       ▼
                            fs_mem_driver        fs_virtio_blk_driver
                            (RAM 卷，MOUNT       (磁盘卷，MOUNT
                             握手注册)            握手注册)
      ┌────────────┬───────────────┬─────────────┬────────────┐
      ▼            ▼               ▼             ▼            ▼
  device_mgr     pkg            user         policy         net
  (PCI 枚举)  (.ops 管理)     (账户/登录)  (命令策略)   (PCnet + 协议栈)
                                   │
                                   ▼
                                 shell  ← 最后由 manager 拉起，接管提示符
                                   │
                                   ├── exec <blob> → 拉起 demo / 应用进程
                                   └── gui → 拉起 gui_demo（像素桌面）
```

**依赖方向**：所有服务都依赖 `serial`（日志）与 `term`（界面）；`vfs` 依赖 `perm`（授权）；`shell` 依赖 `vfs`、`perm`、`user`、`pkg`、`policy`、`net`、`keyboard`、`term`。因此 manager 的启动顺序即为**拓扑序**：`serial → term → keyboard → … → vfs → fs_* → perm → … → shell`。

**重启策略**（`user/services/manager/manager.c` 的 `restartable` 字段）：

| 可重启 | 服务 | 原因 |
| --- | --- | --- |
| ✓（实际启动监控线程） | `perm` `pkg` `device_mgr` `shell` `user` | 状态可从零重建；`StartServiceMonitors()` 的 `s_restartable[]` 只列了这 5 个 |
| ✗（表中标记但未监控） | `wm` `gui` `policy` | `s_services[].restartable = 1`，但尚未注册监控线程（标记与实现不一致，待收口） |
| ✗（引导期演示） | `flaky` | 重启演示在**引导阶段**完成（重启到 `MAX_RESTARTS` 后置 `FAILED`），此后不再监控 |
| ✗ | `serial` `term` `keyboard` | 持有硬件/IRQ/帧缓冲，重启需重绑 IRQ 与重初始化设备 |
| ✗ | `vfs` `fs_mem_driver` `fs_virtio_blk_driver` `net` | 持有命名空间/挂载状态或 PCI 设备；需要 `-ESTALE` 重打开语义 |
| ✗ | `init` `manager` | 引导骨架 |

---

## 七、端到端调用链

### 7.1 写一个文件：`tee /Volumes/Disk/hello.txt hi`

```
shell 进程
  │ ① CmdTee() 解析参数 → libfs
  ▼
libfs FsOpenItem(url, WRITE)
  │ ② IpcCall(port_of("vfs"), {op: VFS_OP_OPEN_ITEM, url, flags, access})
  ▼
vfs 服务
  │ ③ 解析 URL → 定位卷（"Disk" → 由 fs_virtio_blk_driver 挂载的卷）
  │ ④ IpcCall(port_of("perm"), {op: PERM_OP_CHECK, subject, atom, scope})
  ▼
perm 权限引擎
  │ ⑤ 查角色/规则/已有授权：
  │      · OWNER 角色命中规则 → 直接 ALLOW
  │      · 默认拒绝 → 创建 PENDING query
  │ ⑥ IpcCall(port_of("perm.ui"), {op: PERM_OP_UI_SHOW, 提示文本})
  ▼
term 服务
  │ ⑦ 渲染 Powerbox 面板（"Allow? (y/n)"），抢占键盘焦点
  ▼
用户输入 y
  │ ⑧ shell 执行 perm_answer <id> y → IpcCall(port_of("perm"), PERM_OP_ANSWER)
  ▼
perm 权限引擎
  │ ⑨ 记录授权 → SYS_CAP_GRANT_TO_SUBJECT(subject, ATOM_DATA_*, rights, expiry, quota)
  │    （能力被直接写进 shell 进程的内核能力表）
  ▼
vfs 服务
  │ ⑩ 返回 FileHandle（服务端对象表里的一条记录）给 shell
  ▼
shell → libfs FsWrite(handle, offset, buf, len)
  │ ⑪ IpcCall(vfs, {op: VFS_OP_WRITE, handle, ...})
  ▼
vfs → IpcCall("vfs.fs.disk" 驱动) → fs_virtio_blk_driver
  │ ⑫ 驱动组织 I/O → SYS_IO_*/SYS_PCI_CFG_*（受 CAP_TYPE_PCI_DEV 门控）
  │    或 SYS_BLK_READ/WRITE（内核 legacy 块通道）
  ▼
QEMU virtio-blk → disk.img（持久化）
```

**失败路径**：`perm` 返回拒绝 → `vfs` 把 `-105 (EACCES)` 返回给 shell → `tee` 打印 `FAILED (-105) EACCES`。注意 Powerbox 面板会**抢占键盘**，被阻塞的命令需在用户应答后**重新执行**（这也是自动化脚本采用"输入 → 等面板 → y → 重新输入"循环的原因）。

### 7.2 一个窗口从创建到绘制（像素 GUI）

```
应用（如 gui_demo）
  │ GuiActivate()   → GUI_OP_ACTIVATE：合成器接管帧缓冲，term 文本屏让位
  │ GuiCreate(...)  → GUI_OP_CREATE：gui 服务分配离屏窗口缓冲（w×h×32bpp）
  │ GuiFill/GuiText → GUI_OP_FILL / GUI_OP_TEXT：客户端在窗口缓冲上绘制 → 服务端标记脏区 → 合成
  │ GuiPoll()       → GUI_OP_POLL：取出本窗口的事件（KEY / MOUSEMOVE / BUTTON / WHEEL / CLOSE）
  ▼
gui 服务
  ├─ Z 序：焦点窗口置顶；点击命中测试（含重叠窗口与标题栏穿透处理）
  ├─ 脏区合成：只把变化的矩形从窗口缓冲 blit 到帧缓冲
  └─ 焦点路由：键盘事件发给焦点窗口；鼠标事件发给命中窗口
  ▼
帧缓冲（GRUB 提供的线性 RGB，32bpp ARGB 或 24bpp BGR）
```

输入侧：PS/2 中断 → IRQ 转发通知 → `keyboard` 服务读端口 → 事件经 `keyboard` 端口交给持有键盘焦点的消费者（`term` 或 `gui`）→ `gui` 再按窗口 owner 分发。

---

## 八、内存布局

### 8.1 内核虚拟地址空间（`kernel/arch/x86_64/linker.ld`）

| 符号 | 含义 |
| --- | --- |
| `__kernel_phys_start` | 内核物理加载基址（0x100000） |
| `__kernel_virt_base` | 内核虚拟基址 |
| `__kernel_stack` / `__kernel_stack_top` | 引导栈 |
| `__kernel_image_end` / `_kernel_end` | 镜像结束（PMM 据此避开内核占用） |

### 8.2 用户进程虚拟地址空间

| 区域 | 范围 | 大小/对齐 | 说明 |
| --- | --- | --- | --- |
| 程序镜像 | `0x00400000` 起 | 段对齐 4 KiB | `ET_EXEC`，三段 W^X |
| init 引导栈 | `[0x04000000, 0x10000000)` | 页对齐随机 | 仅 PID 1 初始栈 |
| PIE 保留区 | `[0x40000000, 0x70000000)` | 页对齐随机 | 未来 `ET_DYN` 用（当前未启用） |
| 用户堆 | `[0x70000000, 0x78000000)` 起 +256 MB | 64 KiB 随机 | 首尾守卫页 |
| 线程栈区 | `[0x90000000, 0x100000000)` | `ASLR_STACK_BLOCK` 对齐 | 每线程 4 页（16 KiB），按 TID 排布 |
| 内核映射 | 高半区 | — | 用户不可访问 |

### 8.3 内核资源上限

| 常量 | 值 | 含义 |
| --- | --- | --- |
| `MAX_THREADS` | 2048 | 线程表 / 进程表 / 能力表指针数组 / ASLR 栈块大小 |
| `MAX_PORTS` | 256 | IPC 端口表 |
| `MAX_CAPS` | 1024 | 每进程能力表槽位 |
| `MAX_MSG_SIZE` | 4096 | IPC 单条消息字节上限 |
| `MAX_PATH_LEN` | 256 | 路径串上限 |
| `USER_STACK_PAGES` | 4 | 每线程用户栈页数（16 KiB） |
| `MAX_HELD_MUTEXES` | 16 | 单线程可同时持有的内核锁数（退出移交用） |
| `PAGE_SIZE` | 4096 | 页大小 |

---

## 九、构建与部署模型

```
源码                     编译                       链接                      打包
kernel/**.c/.S  ──► build/kernel/**.o ─┐
                                        ├─► kernel.elf（含全部 blob）─► build/isodir/boot/kernel.elf
user/**.c/.S    ──► build/user/**.o    ─┘                                     │
   │                       │                                                  ▼
   │            （每个服务单独链接，排除其它服务的 main）          grub2-mkrescue
   ▼                       ▼                                                  │
共享对象：runtime+crt0+libc+libos+libipc+lib*    build/user/services/<svc>.elf▼
                                                    │                    build/opsos.iso
                                                    ▼
                              ld -r -b binary + objcopy 改名 ─► build/<svc>_blob.o
```

要点：

- **共享对象 + 独占入口**：`USER_SHARED_OBJ = 所有用户 .o 减去各服务的入口 .o`，这样每个服务链接时都带着完整的 libc/runtime，但只有一个 `main`。
- **用户编译选项**（Makefile `USER_CFLAGS`）：`-mcmodel=large`（用户地址空间可能超过 ±2GB 模型）、`-mno-red-zone`、`-ffreestanding -nostdinc -nostdlib`、`-fstack-protector-strong -mstack-protector-guard=global`、`-ffunction-sections -fdata-sections`。
- **用户链接选项**（`USER_LDFLAGS`）：`-T scripts/user.ld`、`--gc-sections`（死代码消除）、`-z separate-code`、`-z max-page-size=0x1000`。
- **新增服务共需改五处**：Makefile 的 `USER_C`、`USER_SVC_ENTRY_OBJ`、`SVC_NAMES`、`SVC_LINK_RULE`，**外加 `kernel/blob/blob.c` 的 blob 注册表**（`BLOB_REG` + `extern`，受 `BLOB_MAX_ENTRIES = 28` 限制，漏改会在开机时 panic）；详见 [developer_guide.md](developer_guide.md) 任务手册 A。
- **格式化排除**：`fs_mem_driver.c`（冻结）、`term/font.h` 与 `kernel/panic_font.h`（生成的字模数据）。

---

## 十、安全设计汇总

| 机制 | 位置 | 作用 |
| --- | --- | --- |
| 内核签发 subject | `process_t.subject_id` | 身份不可伪造；IPC 交付时由内核填充 |
| 内核签发 App UUID | `process_t.app_uuid_hi/lo` | 应用身份（取代可伪造的自报 `app_id_hash`） |
| 能力门控 | `CapLookup()` | 敏感 syscall 的硬边界 |
| 决策下沉 | `syscall.c` 内纯本地查表 | 无 IPC、无 TOCTOU、无策略服务单点 |
| 能力生命周期 | atom / expiry / quota / scope | 授权可过期、可限量、可限定作用域 |
| 按语义吊销 | `SYS_CAP_REVOKE_BY_ATOM` | 跨进程批量撤销 |
| 能力抹位 | `perm` 的 rights 交集 | 句柄权限只减不增 |
| 书签 | `VFS_OP_CREATE_BOOKMARK` | 客户端不需要路径知识 |
| 管理面门控 | `ATOM_SERVICE_MANAGE` | 只有管理面能改角色 / 查他人能力 / 建共享池 |
| 账户二次校验 | `user` 服务的 STOP/VERIFY | 退出保护：确认框 + whoami + 密码 |
| W^X | `scripts/user.ld` PHDRS | 段级不可写且可执行 |
| 栈保护 | 内核 `stack_chk.c` / 用户态 `stack_chk.c` | 溢出检测（金丝雀随机化） |
| ASLR | `rng.c` | 堆/栈/金丝雀随机化 |
| 用户栈守卫页 | 堆与栈区域边界 | 越界立即 #PF 而非静默踩内存 |
| ELF 段重叠校验 | `libos/elf_parse.c` + 内核装载 | 拒绝畸形镜像 |
| 沙盒 manifest | `pkg` + `ops_format.md` | 应用权限由声明决定，无法自授 |

**已知的、明确标注的局限**：

- 口令存储为 FNV-1a-64 + 加盐的完整性校验，**不是**生产级口令哈希（`user/services/user/user.h` 已注明）；
- 无 SMP，因此不存在跨核安全问题，也谈不上跨核隔离；`SYS_PANIC` 是留给 shell 的测试钩子；
- **许可头与目录 LICENSE 不一致**：177 个源文件的 SPDX 头统一为 `GPL-3.0-or-later`，而 `user/lib/`、`user/runtime/` 目录放的是 LGPLv3 文本；
- **服务重启标记与实现不一致**：`policy`/`wm`/`gui` 在服务表里标了 `restartable`，但没有进 `s_restartable[]` 监控名单；
- **网络未接入原子门控**：`ATOM_NET_BIND`/`ATOM_NET_CONNECT` 已在枚举中定义，但 `user/services/net/` 目前没有任何 atom 校验，任何 Ring 3 进程都能调用 net 服务的全部 opcode（见 [net_design.md](net_design.md) §八）。

---

## 十一、限制与演进方向

| 方向 | 现状 | 参考 |
| --- | --- | --- |
| SMP 多核 | `MAX_CPUS = 1`，`ThreadSetAffinity` 仅接口 | [kernel_roadmap.md](kernel_roadmap.md) §4.1 |
| 全量 PIE | 地址区间已预留，解析器已移到用户态 | `kernel/include/kernel/rng.h` |
| 服务热重启 | 仅无状态服务可重启 | `manager.c` |
| 挂起/超时检测 | 未实现（只有退出检测） | `manager.c` 头部注释 |
| TCP 完善 | 单连接，无重传/拥塞控制 | [net_design.md](net_design.md) |
| POSIX 兼容层 | 无 `fork`/`execve`/mmap 文件映射 | [faq.md](faq.md) |
| 审计与遥测 | `PERM_OP_AUDIT` 等接口已预留 | `user/services/perm/perm.h` |

---

## 十二、延伸阅读

| 主题 | 文档 |
| --- | --- |
| 为什么这样切分 Ring 0/3 | [kernel_roadmap.md](kernel_roadmap.md)、[microkernel_audit.md](microkernel_audit.md) |
| 权限与身份模型 | [permission_model.md](permission_model.md) |
| 权限引擎的协议与判定细节 | [permission_reference.md](permission_reference.md) |
| 文件系统对象模型 | [vfs_design.md](vfs_design.md) |
| 系统调用逐项参考 | [syscall_reference.md](syscall_reference.md) |
| 服务与 IPC 协议 | [service_reference.md](service_reference.md) |
| 运行时与内存分配 | [runtime_design.md](runtime_design.md) |
| 界面层 | [tui_design.md](tui_design.md)、[gui_design.md](gui_design.md) |
| 网络 | [net_design.md](net_design.md) |
| 国际化 | [i18n_design.md](i18n_design.md) |
| 构建、运行与调试 | [getting_started.md](getting_started.md) |
| 改代码 | [developer_guide.md](developer_guide.md) |
| 测试 | [testing_guide.md](testing_guide.md)、[test_report.md](test_report.md) |

> 返回 [文档索引](README.md)
