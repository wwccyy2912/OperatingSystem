# OpSys 网络子系统设计文档（PCnet 驱动 + 用户态 ARP/IPv4/ICMP/UDP/TCP 协议栈）

> 适用版本：OpSys v0.8-dev（git HEAD 105805d）　|　最后更新：2026-09-19
>
> OpSys 的网络能力由 Ring 3 的 net 服务独占实现：它用 PCnet-Fast III（AMD AM79C973）legacy 网卡驱动加自研极简协议栈（ARP/IPv4/ICMP/UDP/TCP），并通过名为 net 的 IPC 端口暴露 16 个 opcode；内核只提供 PCI 配置空间访问（受 CAP_TYPE_PCI_DEV 与 ATOM_SERVICE_MANAGE 双重门控）和 I/O 端口能力，不解释任何网络语义。

---

## 一、设计摘要与现状速查

OpSys 的微内核原则是「内核不做策略」：网卡驱动与整个协议栈都在用户态进程里，内核仅保留三类机制——I/O 端口能力、PCI 配置空间 syscall、连续物理页共享池（DMA 用）。因此网络子系统的全部复杂度（帧收发、ARP、校验和、TCP 状态机）都在 Ring 3，可以被独立重启、审计与替换。

| 能力 | 现状 | 代码位置 |
| --- | --- | --- |
| PCI 枚举与配置空间读写 | 已实现：枚举一次扫描后缓存；CFG 读写双门控（CAP_TYPE_PCI_DEV + ATOM_SERVICE_MANAGE） | `kernel/syscall/pci.c:157-186, 284-327` |
| PCnet 驱动（IO BAR + DMA 环） | 已实现，轮询收包 | `user/services/net/main.c:664-779` |
| 原始以太网帧收发（GET_MAC/SEND/RECV/STATS） | 已实现 | `user/services/net/main.c:485-533` |
| ARP（请求/应答/缓存） | 已实现，缓存 16 项、无老化递减 | `user/services/net/proto.c:98-204` |
| IPv4（头构造 + 首部校验和） | 已实现，无分片、无路由表 | `user/services/net/proto.c:208-243` |
| ICMP echo（应答 + ping 客户端） | 已实现 | `user/services/net/proto.c:248-262, 366-409` |
| UDP（16 个绑定槽 + 16 项全局收队列） | 已实现 | `user/services/net/proto.c:59-76, 269-291, 411-465` |
| TCP | 单连接、仅被动打开（无 connect）、无重传 | `user/services/net/proto.c:467-729` |
| DHCP / DNS / IPv6 / socket 抽象 | 未实现 | — |
| 网络原子门控（ATOM_NET_BIND/CONNECT） | 原子已定义、perm/pkg 已引用，**未接入 net 服务** | `kernel/include/kernel/atom.h:49-50`；`user/services/net/*` 无 atom 调用 |

> 关于「已实现/未实现」的判定方法：全部结论来自对 `user/services/net/`、`user/services/shell/shell.c`、`kernel/syscall/pci.c`、`Makefile` 的源码阅读；仓库内没有任何 net 相关的自动化测试（`scripts/*.py`、`docs/test_report.md` 均无 net 用例），因此本文中带「预期输出」字样的内容是按 ShellPrintf 格式串推导的结果，已逐条标注。

---

## 二、分层架构

### 2.1 总览图

```text
 Ring 3（用户态）
 ┌───────────────────────────────────────────────────────────────────────┐
 │ 客户端：shell 的 net 命令（user/services/shell/shell.c:2485-2704）      │
 │         未来：libnet / socket 兼容层 / GUI 网络面板                     │
 └──────────────┬────────────────────────────────────────────────────────┘
                │ IpcCall(PortGet("net"), net_req_t, len) ⇄ IpcReply(net_resp_t)
                │ 单条消息 ≤ MAX_MSG_SIZE = 4096（kernel/include/kernel/types.h:125）
 ┌──────────────▼────────────────────────────────────────────────────────┐
 │ net 服务进程（user/services/net/main.c 779 行 + proto.c 729 行，单线程）│
 │   NetServerLoop()（main.c:457-661）                                    │
 │     ① NetServiceRx()   轮询 8 个 Rx 描述符 → 32 帧驱动队列             │
 │     ② NetRxPump()+ProtoRx()  逐帧喂给协议栈（ARP / ICMP / UDP / TCP）  │
 │     ③ IpcRecv() 阻塞等请求 → switch(op) → IpcReply()                  │
 │   驱动层 main.c（RAP/RDP 寄存器、DMA 池 0x8400 = 9 页、Rx/Tx 环 8×2048）│
 │         ⇅ NetRxPump() / NetSendRaw()                                  │
 │   协议栈 proto.c（ARP 缓存、IPv4、ICMP、UDP、TCP 单连接状态机）        │
 └───────────────┼────────────────────────────────────────────────────────┘
                 │ I/O 端口读写（CAP_TYPE_IO_PORT，32 个端口）+ 共享物理池
 ┌───────────────▼────────────────────────────────────────────────────────┐
 │ 内核：SYS_IO_*（CAP_TYPE_IO_PORT 门控，kernel/syscall/syscall.c:1247）、│
 │       SYS_PCI_CFG_READ/WRITE（CAP_TYPE_PCI_DEV + ATOM_SERVICE_MANAGE）、│
 │       SYS_SHM_CREATE（ATOM_SERVICE_MANAGE，kernel/mm/shm.c:113）        │
 └───────────────┬────────────────────────────────────────────────────────┘
                 │ PCI 1022:2000，BAR0 = I/O 空间，总线主控 DMA
 ┌───────────────▼────────────────────────────────────────────────────────┐
 │ PCnet 网卡（QEMU -device pcnet）→ slirp 用户态网络 → 宿主网络           │
 └────────────────────────────────────────────────────────────────────────┘
```

### 2.2 各层职责与关键约束

| 层 | 文件 | 职责 | 关键约束 |
| --- | --- | --- | --- |
| 客户端 | `shell.c` | 构造 net_req_t、解析 net_resp_t | 无能力要求，只需 PortGet("net") |
| IPC 信封 | `net.h` | opcode、请求/响应结构体、协议常量 | 两个信封各 1540 字节 |
| 服务 | `main.c:457-661` | 收包、喂协议栈、请求分派 | 单线程：阻塞在 IpcRecv 时不再收包 |
| 驱动 | `main.c:185-453` | CSR/BCR、DMA 环、Tx/Rx、统计 | 轮询收包，不绑定中断 |
| 协议栈 | `proto.c` | ARP/IPv4/ICMP/UDP/TCP | 单线程、无锁、无重传 |
| 内核 | `pci.c`、`syscall.c` | PCI 配置空间、I/O 端口能力、共享物理池 | 只做机制，不看网络语义 |

### 2.3 一次 net ping 10.0.2.2 的完整链路

```text
shell                 net 服务                    驱动/网卡             对端(slirp 网关)
  │ IpcCall(PING,ip)     │                             │                     │
  ├─────────────────────▶│ ProtoPing()                 │                     │
                         │  IpSendRaw()                │                     │
                         │   └ ArpResolve(10.0.2.2)    │                     │
                         │       ArpRequest() ─────────▶ NetSend(60B 广播帧)  │
                         │       （600 × NetRxPumpNow + NetYield ≈ 6 s 自旋） │
                         │       ArpLearn() 写入缓存    │◀── ARP reply ───────┤
                         │   └ 构造 IPv4+ICMP(8+32B) ──▶ NetSend(74B)         │
                         │   RxPumpNow 自旋等 echo reply◀── ICMP echo reply ───┤
                         │   （只认 IPv4 + proto=1 + ICMP type=0）            │
  │◀── resp.ret = 0 ─────┤                             │                     │
  │ "net: reply OK"      │                             │                     │
```

注意：ProtoPing() 等待期间只把「匹配 ICMP type 0」的帧当作结果，其余帧交给 ProtoRx()（`user/services/net/proto.c:390-407`）。图中间一列的 600 次自旋即该函数的等待循环。

---

## 三、硬件与驱动：PCnet-Fast III (AM79C973)

### 3.1 为什么选 legacy 网卡

| 选型理由 | 依据 |
| --- | --- |
| QEMU 兼容性最好的 legacy 网卡，端口 I/O + DMA 环模型简单，无需 MSI-X / 多队列 | `README.md:136`「PCnet-Fast III (AM79C973) 用户态驱动（QEMU 兼容性最好的 legacy 网卡）」 |
| BAR0 是 I/O 空间，可直接复用现成的 CAP_TYPE_IO_PORT 能力模型门控寄存器访问 | `user/services/net/main.c:680`（`s_io_base = dev.bar[0] & 0xFFFFFFF0`） |
| 驱动可以完全跑在用户态：寄存器走 I/O 端口能力，DMA 走共享物理页 | `user/services/net/main.c:691-701, 726-732` |
| 对比 virtio-net 需要 MMIO 映射与更复杂描述符链，与本项目 virtio-blk 的路径不同 | `docs/microkernel_audit.md:21`（PCI 配置留在内核的理由）；`docs/kernel_roadmap.md:127`（MMIO 映射是 virtio 驱动前提） |

驱动在 PCI 上按 **vendor 0x1022 / device 0x2000** 匹配（`user/services/net/main.c:68-69`）。

### 3.2 PCI 发现与配置空间访问（内核侧）

内核只提供四个 syscall（`kernel/include/kernel/syscall_numbers.h:112-113, 188-189`）：

| syscall | 编号 | 参数 | 门控 |
| --- | --- | --- | --- |
| SYS_PCI_GET_COUNT | 46 | 无 | 无（只读缓存快照） |
| SYS_PCI_GET_DEVICE | 47 | (index, 用户态 pci_device_info_t*) | 无（只读快照，写指针经用户范围校验） |
| SYS_PCI_CFG_READ | 71 | (index, offset) → u32 | CAP_TYPE_PCI_DEV (R+W) **且** ATOM_SERVICE_MANAGE |
| SYS_PCI_CFG_WRITE | 72 | (index, offset, val) | 同上 |

枚举在第一次 SYS_PCI_GET_COUNT 时惰性执行一次并缓存（`kernel/syscall/pci.c:157-186`），扫描范围 bus 0、device 0..31、function 0..7，只探测 function 0 存在的槽位（多功能设备启发式），表容量 PCI_CONFIG_MAX_DEVS = 64（`kernel/syscall/pci.c:75-81`）。配置空间访问走经典 0xCF8/0xCFC 端口（`kernel/syscall/pci.c:69-71, 120-125`）——用户进程即使没有任何 I/O 端口能力，也能通过 syscall 读到设备快照。

设备描述结构体（`kernel/include/kernel/pci.h:33-46`）：

```c
typedef struct {
    uint32_t bus;          /* 总线号             */
    uint32_t dev;          /* 设备号             */
    uint32_t func;         /* 功能号             */
    uint16_t vendor_id;    /* 0x1022 (PCnet)     */
    uint16_t device_id;    /* 0x2000 (PCnet)     */
    uint16_t class_code;   /* (base << 8) | sub  */
    uint8_t  prog_if;
    uint8_t  revision_id;
    uint32_t bar[PCI_MAX_BARS]; /* 6 项，0 = 不存在 */
    uint8_t  irq_line;     /* 配置空间 0x3C 低字节 */
} pci_device_info_t;
```

### 3.3 驱动启动序列与它获得的三项权限

main() 的启动步骤（`user/services/net/main.c:664-779`）：

| 步 | 动作 | 代码位置 | 失败表现 |
| --- | --- | --- | --- |
| 1 | 遍历 PCI 枚举找 0x1022:0x2000，取 BAR0 与 IRQ | 670-687 | 打印 net: no PCnet adapter found 后 for(;;) Sleep(10) 常驻 |
| 2 | CapCreateObj(CAP_TYPE_PCI_DEV, R\|W, idx) 与 CapCreateObj(CAP_TYPE_IO_PORT, RIGHT_ALL, (0x20 << 16) \| io_base) | 691, 696-698 | ThreadExit(1) |
| 3 | 从 APROM（IO + 0x00..0x05）读 6 字节 MAC | 704（实现见 216-219） | — |
| 3b | 置 PCI command 的 IO\|MEM\|MASTER（bit0/1/2）以打开总线主控 DMA | 709-724 | ThreadExit(1) |
| 4 | ShmCreate(0x8400/4096 + 1 = 9 页, 0x50000000) 建连续物理 DMA 池 | 726-732 | net: shm_create failed (no ATOM_SERVICE_MANAGE?) |
| 5 | PcnetReset() + PcnetInitRings() + MutexCreate() 建队列/环锁 | 741-745 | — |
| 7 | PcnetStart()（INIT → 等 IDON → STRT） | 749-754 | net: NIC start FAILED / net: INIT timeout |
| 7c | ProtoInit(10.0.2.15, 10.0.2.2) 设置静态地址 | 757-762 | — |
| 8 | IpcPortCreate() + PortRegister("net", port) | 765-774 | ThreadExit(1) |

I/O 端口能力的 obj_id 编码是 (count << 16) | base，覆盖 [base, base+count)（`kernel/syscall/syscall.c:1247-1273`）：驱动申请 0x20 个端口，即 BAR 的 0x00..0x1F，正好覆盖 RDP/RAP/BCR 与 APROM 的 MAC 字节。

**为什么 net 能拿到 ATOM_SERVICE_MANAGE**：SYS_PCI_CFG_* 与 SYS_SHM_CREATE 都要求调用者持有 ATOM_SERVICE_MANAGE（`kernel/syscall/pci.c:266-272, 289-292`；`kernel/mm/shm.c:113`）。该原子由内核在 ProcessCreate 时按 **blob 内容身份** 播种：spawner 必须自己持有该原子，且新进程的 ELF 必须与内核内嵌的服务 ELF 逐字节相同；名单里包含 "net"（`kernel/syscall/process_desc.c:304-330`）。net 由 manager 拉起（`user/services/manager/manager.c:148, 540-541`），而 manager 由 init 拉起，init 在 `kernel/kernel_main.c:261-268` 被直接播种。

### 3.4 DMA 池与环几何

驱动用一个连续物理页池承载所有 DMA 结构（`user/services/net/main.c:30-36, 108-119, 726-736`）：

| 偏移 | 大小 | 内容 |
| --- | --- | --- |
| 0x0000 | 0x100 | InitBlock（28 字节有效，其余填充） |
| 0x0100 | 0x080 | Rx 描述符环 8 × 16 B |
| 0x0200 | 0x080 | Tx 描述符环 8 × 16 B |
| 0x0400 | 0x4000 | Rx 缓冲 8 × 2048 B |
| 0x4400 | 0x4000 | Tx 缓冲 8 × 2048 B |
| 合计 | 0x8400 = 33792 B = 33 KiB | 代码分配 0x8400/4096 + 1 = 9 页 |

> 精度说明：`user/services/net/main.c:36` 与 `:117` 的注释写作「10 pages」，但代码只申请 9 页（`main.c:728`），且最高使用偏移 0x8400 恰好落在第 9 页边界内。以代码为准。

常量：NET_RINGS = 8、NET_BUF_SIZE = 2048、NET_RXQ_DEPTH = 32、NET_MTU = 1514（`user/services/net/net.h:60`）。环长度以 Log2(8) << 4 = 0x30 写进 InitBlock 的 rlen/tlen（`main.c:227-228`）。

### 3.5 描述符与 InitBlock 布局

16 字节描述符（`user/services/net/main.c:121-135`，编译期 _Static_assert(sizeof == 16)）：

```c
typedef struct {
    volatile u32 addr;     /* +0  缓冲物理地址                */
    volatile u16 len;      /* +4  bcnt（Tx = 帧长，Rx = 缓冲大小） */
    volatile u16 status;   /* +6  bit15 OWN + 标志位           */
    volatile u32 mcnt;     /* +8  Rx 收到的帧长                */
    volatile u32 reserved; /* +12                              */
} __attribute__((packed)) pcnet_desc_t;
```

| 标志 / 位 | 值 | 含义 |
| --- | --- | --- |
| DESC_OWN | 0x8000 | 1 = 网卡拥有该描述符（Rx：可填；Tx：待发） |
| DESC_STP | 0x0200 | 帧起始 |
| DESC_ENP | 0x0100 | 帧结束 |
| Rx 错误掩码 | 0x7C00 | ERR\|FRAM\|OFLO\|CRC\|BUFF，任一置位即丢帧并计数 |

InitBlock（SSIZE32 布局，28 字节，`user/services/net/main.c:137-156`）：mode(u16)=0、rlen(u8)=0x30、tlen(u8)=0x30、phys_addr[6]=MAC、reserved(u16)、logical_addr[8]=0（LADRF 全零）、rdra=Rx 环物理地址、tdra=Tx 环物理地址。

关键寄存器常量（`user/services/net/main.c:71-106`）：

| 名称 | 值 | 说明 |
| --- | --- | --- |
| PCNET_RDP | BAR+0x10 | 寄存器数据口（QEMU 按 addr & 0x0f == 0 分派） |
| PCNET_RAP | BAR+0x12 | 寄存器地址口（== 2） |
| PCNET_BCR | BAR+0x16 | BCR 数据口（== 6）；BAR+0x14 为 RESET（读即复位，驱动不使用） |
| BCR20 | 20 | SWSTYLE=2，QEMU 自动编码为 0x0302（16 字节描述符 + SSIZE32） |
| CSR0_INIT / STRT / STOP / TDMD | 0x0001 / 0x0002 / 0x0004 / 0x0008 | 命令位 |
| CSR0_IDON / TINT / RINT / MERR / INTR | 0x0100 / 0x0200 / 0x0400 / 0x0800 / 0x1000 | 状态位（写 1 清零） |
| CSR4 | 4 | 写入 0x0915（QEMU 兼容的默认配置值，`main.c:212`） |

> 驱动明确面向 QEMU 的 PCnet 模型而非真实 AMD 硬件：注释指出 QEMU 用 (addr & 0x0f) 分派寄存器偏移，而 AMD 硬件是 RDP=0x10 / RAP=0x14（`user/services/net/main.c:71-76`）；CSR0 位布局也按 QEMU 实现（`main.c:85-89`）。

### 3.6 初始化：Reset → 环 → INIT → STRT

```text
PcnetReset()        CSR0 = STOP；BCR20 = 2（SWSTYLE=2 / SSIZE32）；CSR4 = 0x0915
PcnetInitRings()    池清零；写 InitBlock；8 个 Rx 描述符 addr/BCNT/OWN=1 交给网卡
PcnetStart()        CSR1/CSR2 = InitBlock 物理地址低/高 16 位
                    CSR0 = INIT  →  自旋至多 1,000,000 次等 IDON
                       ├ MERR 置位 → 返回 -7 (ERR_FAULT)，打印 csr0
                       └ 超时       → 返回 -7，打印 "INIT timeout"
                    CSR0 = STRT  → Rx/Tx 上线（不写 INEA，不绑定中断）
```

Rx 描述符的 BCNT 写作 (4096 - 2048) | 0xF000 = 0xF800：低 12 位是缓冲计数，高 4 位的 ONES 半字节是 QEMU CHECK_RMD 校验所要求的（`user/services/net/main.c:233-244`）。Tx 描述符同理写作 (4096 - len) | 0xF000（`main.c:378`）。

### 3.7 发送路径

NetSend()（`user/services/net/main.c:331-405`）：

1. 参数校验：!data || len == 0 || len > NET_MTU → 返回 -2（ERR_INVAL）。
2. 取 s_lock 互斥锁（保护 Tx 环 + Rx 队列）。
3. 读 CSR74（XMTRC）算出下一个槽位：slot = (NET_RINGS - xmtrc) % NET_RINGS。注释解释了为何不能用「第一个空闲槽」——QEMU 的发送器严格按 XMTRC 顺序消费描述符，选错槽会让第二次发送失步、TDMD 永不触发（`main.c:337-342`）。
4. 自旋等待该槽的 OWN 位清零（上限 1,000,000 次）；超时则复位该描述符（status=0、len=(4096-2048)|0xF000、mcnt=0）、s_stat_err++、返回 -7。
5. 拷贝帧到 Tx 缓冲；**长度 < 60 时补零到 60**（IEEE 802.3 最小帧，否则真实硬件/交换机会按 runt 丢弃，`main.c:368-373`）。
6. 写 addr / BCNT / status = OWN|STP|ENP，写 CSR0 = TDMD 触发发送。
7. 自旋等 OWN 被网卡清零以确认真正发出；超时打印 net: TX timeout len=... c0=... tstat=...、复位描述符并计数错误；成功则 s_stat_tx++ 并返回 0。

### 3.8 接收路径：轮询而非中断

NetServiceRx()（`user/services/net/main.c:408-453`）：

1. 遍历 8 个 Rx 描述符，跳过仍为 OWN=1 的（网卡未填）。
2. 只接受 **单描述符完整帧**：STP|ENP 都置位且错误掩码 0x7C00 为 0；否则 s_stat_err++ 并回收描述符。
3. mcnt 为 0 或 > NET_MTU → 计数错误并回收。
4. 在锁内把帧拷进 32 槽的驱动队列（s_rxq[32][1514]）；队列满则丢弃并 s_stat_err++；成功则 s_stat_rx++。
5. 把描述符 status = DESC_OWN 交还网卡。
6. 每轮结束写 CSR0 = RINT 清中断标志。

**为什么不用中断**：QEMU 的 pcnet INTx 路径不可靠，且 8259 是电平触发——一个锁存的 RINT 会让共享中断线持续拉高并反复打断 CPU（`user/services/net/main.c:22-25, 274-278, 444-452, 459-461`）。驱动因此**从不调用 BindIrq**（全文件无此调用），PCI 读到的 irq_line 只用于打印（`main.c:159, 679, 688`）。代价是：网卡收包不会唤醒服务，只有在服务主循环或阻塞型 op 的自旋里才会排空环。

### 3.9 MAC 地址读取

PcnetReadMac() 从 BAR 偏移 0x00..0x05 逐字节读 6 字节（`user/services/net/main.c:215-219`），启动时打印一次，并通过 NetGetMac() 提供给协议栈（`main.c:294-296`），NET_OP_GET_MAC 再把它交给客户端。

### 3.10 统计计数

三个 u32 计数器（`user/services/net/main.c:177-179`），由 NET_OP_STATS 以 u32[3] 返回（`main.c:519-533`）：

| 计数器 | 递增点 |
| --- | --- |
| s_stat_rx | 帧成功进入 32 槽驱动队列（`main.c:438`） |
| s_stat_tx | 帧被网卡确认发出、OWN 清零（`main.c:400`） |
| s_stat_err | Tx 槽卡死 / Tx 超时（`main.c:358, 394`）、Rx 描述符不完整或带错误位（`:419`）、mcnt 非法（`:425`）、Rx 队列满（`:440`） |

---

## 四、net 服务 IPC 协议

### 4.1 端口注册与调用模型

服务启动时 IpcPortCreate() + PortRegister(NET_PORT_NAME, port)（`user/services/net/main.c:765-774`）；端口名 "net" 定义于 `user/services/net/net.h:38`，客户端用 PortGet("net") 解析（shell 见 `user/services/shell/shell.c:2490`）。

调用语义是**同步 RPC**：客户端 IpcCall 阻塞到服务 IpcReply。内核侧的门控只有两点——端口存在、请求长度 ≤ MAX_MSG_SIZE（`kernel/ipc/ipc.c:507-517`）；名称解析是纯查表（`kernel/ipc/ipc.c:675-686`）。**任何 Ring 3 进程只要知道端口号就能调用 net 的全部 opcode，包括发送任意以太网帧**（见第八节）。

### 4.2 opcode 全表

16 个 opcode（`user/services/net/net.h:41-57`，分派于 `user/services/net/main.c:485-652`）：

| opcode | 值 | 请求 data 布局 | 实现入口 |
| --- | --- | --- | --- |
| NET_OP_GET_MAC | 1 | 无 | `main.c:485-489` |
| NET_OP_SEND | 2 | 原始以太网帧 frame[len] | `main.c:490-499` |
| NET_OP_RECV | 3 | 无（len 字段被忽略） | `main.c:500-518` |
| NET_OP_STATS | 4 | 无 | `main.c:519-533` |
| NET_OP_SET_IP | 5 | ip[4] + gw[4] | `main.c:535-542` |
| NET_OP_IP_SEND | 6 | ip[4] + proto(1B) + 载荷 | `main.c:543-552` |
| NET_OP_PING | 7 | ip[4] | `main.c:553-560` |
| NET_OP_UDP_BIND | 8 | 端口 2 字节（大端） | `main.c:561-567` |
| NET_OP_UDP_SENDTO | 9 | 目的 ip[4] + sport + dport（各 2B 大端）+ 数据 | `main.c:575-586` |
| NET_OP_UDP_RECV | 10 | 无 | `main.c:587-608` |
| NET_OP_UDP_UNBIND | 11 | 端口 2 字节 | `main.c:568-574` |
| NET_OP_TCP_LISTEN | 12 | 端口 2 字节 | `main.c:609-615` |
| NET_OP_TCP_ACCEPT | 13 | 无 | `main.c:616-633` |
| NET_OP_TCP_SEND | 14 | 数据（超过 1400 字节会被截断） | `main.c:634-638` |
| NET_OP_TCP_RECV | 15 | 无 | `main.c:639-649` |
| NET_OP_TCP_CLOSE | 16 | 无 | `main.c:650-652` |

响应的 data 布局：

| opcode | 响应 data | resp->len |
| --- | --- | --- |
| GET_MAC | mac[6] | 6 |
| SEND / SET_IP / IP_SEND / PING / *_BIND / *_UNBIND / TCP_LISTEN / TCP_SEND / TCP_CLOSE | 无 | 0 |
| RECV | 原始帧（≤ 1514 B） | 帧长 n |
| STATS | u32[3] = {rx, tx, err} | 12 |
| UDP_RECV | 源 ip[4] + sport + dport + 载荷 | n + 8 |
| TCP_ACCEPT | peer ip[4] + peerport（2B 大端） | 6 |
| TCP_RECV | 数据（≤ 1500 B）；对端 FIN 且队列空时 ret=0、len=0 表示 EOF | n |

### 4.3 请求/响应信封与 4096 字节上限

```c
typedef struct {
    u32 op;
    u32 len;                    /* 载荷字节数 */
    u8  data[NET_MTU + 16];     /* 1530 */
} net_req_t;

typedef struct {
    i32 ret;
    u32 len;                    /* RECV: 实际拷贝字节数 */
    u8  data[NET_MTU + 16];     /* 1530 */
} net_resp_t;

_Static_assert(sizeof(net_req_t)  <= 4096, "net_req_t too big");
_Static_assert(sizeof(net_resp_t) <= 4096, "net_resp_t too big");
```

（`user/services/net/net.h:79-88`）

尺寸关系（编译 net.h 实测）：

| 量 | 值 | 说明 |
| --- | --- | --- |
| NET_MTU | 1514 | 14 字节以太网头 + 1500 载荷，不含 FCS（`net.h:40`） |
| data[] 元素数 | 1530 | NET_MTU + 16，留了 16 字节余量 |
| sizeof(net_req_t) = sizeof(net_resp_t) | **1540** | 4 + 4 + 1530 = 1538，按 4 字节对齐补到 1540 |
| data 偏移 | 8 | 内核与服务按「8 + payload」计算消息长度 |
| 内核消息上限 MAX_MSG_SIZE | 4096 | `kernel/include/kernel/types.h:125` |
| 最大单帧消息 | 8 + 1514 = 1522 | 仍留有约 2.5 KiB 余量；上限提升到 4096 时 MTU 才有继续增长的空间 |

服务端使用两个 4096 字节静态缓冲（`user/services/net/main.c:181-183`），回复长度按 IpcReply(token, resp, 8 + resp->len) 计算（`main.c:658-659`）。shell 侧则声明 net_req_t / net_resp_t 局部变量，并把请求长度写成 8 + payload（例如 `shell.c:2502, 2546, 2642`）。

### 4.4 服务主循环

```c
static void NetServerLoop(int port) {
    for (;;) {
        NetServiceRx();                       /* ① 排空 NIC 环 → 驱动队列 */
        { u8 frame[1600]; u32 flen;
          while (NetRxPump(frame, &flen))     /* ② 队列 → 协议栈 */
              ProtoRx(frame, flen); }
        int msg_len = (int)sizeof(s_req);     /* 4096 */
        int ret = IpcRecv(port, s_req, &msg_len, &token);   /* ③ 阻塞 */
        if (ret < 0) { printf("net: ipc_recv failed (%d)\n", ret); ThreadExit(1); }
        net_req_t  *req  = (net_req_t *)s_req;
        net_resp_t *resp = (net_resp_t *)s_resp;
        memset(resp, 0, sizeof(*resp));
        resp->ret = -2;                       /* 默认 ERR_INVAL */
        switch (req->op) { /* 16 个 case，见 4.2 表 */ }
        (void)IpcReply(token, resp, (int)(8 + resp->len));
    }
}
```

（`user/services/net/main.c:457-661`，上为其结构等价缩写）

要点：

- **默认错误码是 -2**：任何未识别的 opcode 或长度不足的请求都会原样返回 -2。
- 请求长度会被逐 op 校验（例如 SEND 要求 req->len > 0 且 ≤ NET_MTU 且 ≤ msg_len - 8，`main.c:492-498`），防止客户端谎报长度导致越界拷贝。
- 阻塞型 op（PING / TCP_ACCEPT / TCP_RECV）内部自行调用 NetRxPumpNow() 把 NIC 环排到驱动队列（`main.c:322-324` → `main.c:408-453`），并以 NetYield()（Sleep(1) = 10 ms，PIT 100 Hz，`kernel/kernel_main.c:125`）让出 CPU。由于没有后台收包线程，**服务阻塞在 IpcRecv 期间到达的帧只能靠 NIC 自带的 8 个 Rx 描述符缓冲**，超过 8 帧即丢。

### 4.5 错误码语义

错误码取值与内核一致（`user/lib/libos/syscalls.h:40-50`）：

| 值 | 名称 | 在网络路径中的含义 |
| --- | --- | --- |
| 0 | OK | 成功 |
| -1 | ERR_NOMEM | UDP 绑定表 16 槽已满（`proto.c:421`） |
| -2 | ERR_INVAL | 长度非法、端口 < 16、TCP 状态不允许该操作、未知 opcode |
| -4 | ERR_NOENT | UDP_UNBIND 的端口未绑定（`proto.c:427`） |
| -6 | ERR_AGAIN | RECV / UDP_RECV 无待收数据（`main.c:513`、`proto.c:451`） |
| -7 | ERR_FAULT | ARP 无应答、ping 超时、TCP accept/recv 超时、Tx 环卡死 |
| -8 | ERR_OVERFLOW | IPv4/UDP 载荷超过 1500（无分片能力，`proto.c:216, 437`） |

---

## 五、协议栈逐层实现

协议栈全部在 `user/services/net/proto.c`（729 行），只被 net 服务线程调用，因此**内部无锁**（文件头注释 `proto.c:18-22`）。

### 5.1 以太网层

ProtoRx(frame, len)（`user/services/net/proto.c:305-358`）的过滤与分派顺序：

1. len < 14 → 丢弃。
2. 目的 MAC 必须是本机 MAC，**或** frame[0] == 0xFF（只检查首字节，不是完整广播地址比较）。
3. 解析 EtherType：0x0806 → ARP（要求载长 ≥ 28）；0x0800 → IPv4；其余丢弃。
4. IPv4 要求：版本 = 4、iplen ≤ plen、**首部校验和校验结果为 0**（`proto.c:330-337`）。
5. 按 ip[9] 分派 ICMP / UDP / TCP。

发送侧由 IpSendRaw() 统一封装 14 字节以太网头 + IP 头（`proto.c:208-243`）。

### 5.2 ARP

| 项 | 实现 |
| --- | --- |
| 缓存 | 16 项数组 s_arp[16]，字段 {valid, ip[4], mac[6], ttl}（`proto.c:45-53`） |
| 请求 | ArpRequest() 构造 60 字节帧：目的 MAC 全 0xFF 广播、op=1、sender = 本机 MAC/IP、target IP 待解析（`proto.c:126-141`） |
| 应答/学习 | ArpHandle() 先学习发送方（IP 在第 14..17 字节、MAC 在第 8..13 字节），若 op==1 且 target IP 是自己则回 op=2 应答（`proto.c:144-171`） |
| 解析 | ArpResolve()：缓存命中直接返回；否则发请求并自旋 **600 次 × 10 ms ≈ 6 s**，每轮 NetRxPumpNow() 并排空队列，匹配「EtherType=0x0806 且 op=2 且 sender IP 命中」的帧，其余帧转交 ProtoRx()；超时返回 -7（`proto.c:176-204`） |
| 老化 | ttl 仅在 ArpLearn 写入 ARP_TTL_TICKS = 1000（约 10 s），**代码中没有任何递减点**（`proto.c:102, 121`），因此条目实际永不过期，只会被新条目覆盖（表满时覆盖第 0 项，`proto.c:116-117`） |

### 5.3 IPv4

IpSendRaw()（`user/services/net/proto.c:208-243`）：

- 先 ArpResolve(dst)；失败直接返回错误码。
- 总长 IP_HDR_LEN + len > 1500 → -8（**无分片**）。
- 头字段：0x45（v4、20 字节头）、总长、id=0、flags/frag=0、**TTL=64**、协议号、源/目的地址。
- 首部校验和：先清零 [10..11]，用 IpChecksum()（16 位反码和，`proto.c:80-92`）计算后填入。
- 以太网目的 MAC 用 ARP 结果，源 MAC 是本机 MAC，EtherType = 0x0800。

局限（均以代码为准）：

| 项 | 现状 |
| --- | --- |
| 路由 | 无路由表；s_gw（默认网关）只在 ProtoInit 里被保存，**全文件无其他引用**（`proto.c:41, 297`），即每个目的地址都直接做 ARP |
| 分片/重组 | 不支持（`proto.c:214-216`） |
| IP 选项 | 不支持（固定 20 字节头） |
| 接收侧校验 | 只检查 version、总长、首部校验和（`proto.c:329-337`） |

### 5.4 ICMP

| 方向 | 实现 |
| --- | --- |
| 应答（服务端角色） | IcmpEchoReply()：收到 type=8 的 echo request 时复制整个 ICMP 报文，改 type=0、重算校验和，再走 IpSendRaw() 发回（`proto.c:248-262`；分派见 `proto.c:344-348`） |
| 请求（客户端角色） | ProtoPing()：构造 8 + 32 字节报文（type=8，id 由静态计数器 0x1234 起递增，seq=1，载荷 0..31 递增），发完后自旋 **600 次 × 10 ms ≈ 6 s**，把「IPv4 + proto=1 + ICMP type=0」的任意应答视为成功（`proto.c:366-409`） |
| 匹配严格度 | 只判 type=0，不比对 id / seq / 源地址；因此同一时刻任何 echo reply 都会让等待提前返回成功 |
| 其他 ICMP | 不处理（type 0 在 ProtoRx 中被丢弃，无差错 / 不可达 / 重定向处理） |

### 5.5 UDP

| 项 | 实现 |
| --- | --- |
| 绑定表 | s_udp[16]，仅 {in_use, port}；端口必须 ≥ 16，重复绑定返回 -2，表满返回 -1（`proto.c:59-65, 411-422`） |
| 解绑 | ProtoUdpUnbind()，未绑定返回 -4（`proto.c:424-430`） |
| 发送 | ProtoUdpSendto() 构造 8 字节头（sport / dport 大端、长度、**校验和 0**——RFC 768 对 IPv4 允许），总长 > 1500 返回 -8，随后 IpSendRaw()（`proto.c:432-447`） |
| 接收 | ProtoRx 中只有 dport 已绑定才入队；未绑定端口的数据报**静默丢弃**（不回 ICMP 端口不可达，`proto.c:349-355`） |
| 收队列 | 全局 16 项环形队列 s_udp_rxq[16]，每项含 src[4] / sport / dport / len / data[1500]；满则丢弃（`proto.c:67-76, 280-291`） |
| 出队 | ProtoUdpRecv() **不按端口过滤**：返回队首的任何数据报（含其 dport 字段）。空队列返回 -6（`proto.c:449-465`） |

> 设计要点：UDP 收队列是**服务级全局队列**而非 per-socket 队列，绑定只决定「哪些 dport 的数据报会被保留」。多客户端同时绑定不同端口时，先调用 UDP_RECV 的客户端可能取走别人的数据报（可依赖返回的 dport 自行分拣）。

### 5.6 TCP：单连接状态机

状态与变量（`user/services/net/proto.c:467-499`）：

| 状态 | 值 | 含义 |
| --- | --- | --- |
| TCP_STATE_LISTEN | 0 | 已 LISTEN，等 SYN |
| TCP_STATE_SYN_SENT | 1 | 已回 SYN-ACK，等对端 ACK |
| TCP_STATE_ESTAB | 2 | 已建立 |
| TCP_STATE_CLOSE_WAIT | 3 | 对端已 FIN，本端仍可发 |
| TCP_STATE_CLOSED | 4 | 空闲（初值） |

全局仅一组连接变量：s_tcp_lport、s_tcp_peer[4]、s_tcp_peer_port、s_tcp_iss、s_tcp_snd_nxt、s_tcp_rcv_nxt，外加 8 项收队列 s_tcp_rxq[8]（每项 data[1500]）。**没有连接表，也没有主动打开（connect）能力。**

报文构造 TcpSendSeg(flags, seq, ack, data, len)（`proto.c:526-556`）：数据偏移 0x50（20 字节头，无选项）、标志位、**固定窗口 4096**（seg[14]=0x10、seg[15]=0x00）、紧急指针 0，校验和按 RFC 793 伪首部（源 IP、目的 IP、0、协议号 6、TCP 长度）计算（`proto.c:502-523`）。

三次握手（被动打开，`proto.c:576-621`）：

```text
对端                      net 服务 (ProtoTcpAccept 自旋，6000 × 10ms ≈ 60 s)
 │  SYN(seq=x)            │  dport == s_tcp_lport 且 SYN 且非 ACK
 ├───────────────────────▶│  peer = 源地址 / 源端口；rcv_nxt = x+1
 │                        │  iss = GetTime()（为 0 则退化为 0x1234）；snd_nxt = iss+1
 │  ◀── SYN|ACK(seq=iss, ack=x+1) ──┤  state = SYN_SENT
 │  ACK(ack=iss+1)        │  ACK 且非 SYN → state = ESTAB
 ├───────────────────────▶│  snd_nxt = ack; rcv_nxt = seq
 │                        │  返回 0，回填 peer[4] + peerport
```

（`proto.c:593-607` 处理 SYN 并回 SYN-ACK；`proto.c:638-653` 处理 SYN_SENT 下的纯 ACK 与同时打开。）

数据收发（`proto.c:625-677, 679-689, 692-729`）：

| 方向 | 规则 |
| --- | --- |
| 接收数据 | 仅在 ESTAB / CLOSE_WAIT 且 seq == s_tcp_rcv_nxt 且队列未满时入队，然后 rcv_nxt += dlen 并立即回 ACK。乱序 / 重复段**直接丢弃且不回 ACK**，等对端重传 |
| 接收 FIN | rcv_nxt = seq + 1，状态转 CLOSE_WAIT，回 ACK（`proto.c:670-674`） |
| 发送数据 | ProtoTcpSend()：状态必须为 ESTAB / CLOSE_WAIT，len > 1400 时截断到 1400，打 ACK\|PSH；仅在发送成功时推进 snd_nxt（`proto.c:679-689`） |
| 交付客户端 | ProtoTcpRecv() 自旋 600 × 10 ms；有数据立即返回长度；若已 CLOSE_WAIT 且队列空则返回 **0（EOF）**；否则返回 -7（`proto.c:692-729`） |
| 关闭 | ProtoTcpClose() 只把状态置 CLOSED 并清队列（`proto.c:568-573`）——**不发送 FIN/RST**，属于本地硬关闭 |

标志位定义（`user/services/net/net.h:90-95`）：TCP_FIN 0x01、TCP_SYN 0x02、TCP_RST 0x04、TCP_PSH 0x08、TCP_ACK 0x10。其中 **TCP_RST 在全仓库内只被定义、从未被使用**（既不发送也不判定），因此非法段的处理方式是「静默忽略」。

---

## 六、Shell 集成：net 命令

### 6.1 子命令表

CmdNet（`user/services/shell/shell.c:2485-2704`）注册为命令 net（`shell.c:3771`，描述串 "PCnet NIC: net mac|arp|recv|stats"）。它先 PortGet("net")，失败则打印 net: 'net' port unavailable (-4) 并返回（`shell.c:2490-2494`）。

| 子命令 | 参数 | 使用的 opcode | 行为 |
| --- | --- | --- | --- |
| net mac | — | 1 GET_MAC | 打印 6 字节 MAC（%x，无前导零） |
| net arp | — | 1 + 2 + 3 | 取本机 MAC → 构造 42 字节 ARP who-has 10.0.2.2 → 轮询 RECV 至多 300 × 10 ms ≈ 3 s，命中「EtherType 0x0806 + op=2 + spa=10.0.2.2」才算成功 |
| net recv | — | 3 RECV | 打印帧长并把**前 64 字节**按 16 字节一行输出十六进制；空队列打印 net: no packet pending |
| net stats | — | 4 STATS | 打印 rx / tx / err 三个计数 |
| net ping <ip> | 点分十进制 | 7 PING | 自校验 4 段 0..255 地址后发 ping，成功打印 net: reply OK，失败打印 net: no reply (ret=%d) |
| net tcp <port> | 端口 16..65535 | 12 → 13 → 15 → 14 → 16 | 服务端回显测试：监听 → 接受 → 收一段 → 原样回发 → 关闭 |

### 6.2 使用示例与预期输出

> 下列输出是**按 ShellPrintf 格式串推导**的（仓库内无 net 自动化测试）。前提是 QEMU 已挂载 PCnet 网卡（见第七节）；否则 net 端口不存在，任何子命令都只会得到 net: 'net' port unavailable (-4)。

```text
OpSys> net mac
net: MAC 52:54:0:12:34:56
```

（QEMU -device pcnet 的默认 MAC 为 52:54:00:12:34:56，本机 QEMU 10.2.2 用 info qtree 实测；shell 用 %x 输出，因此 0x00 显示为 0。）

```text
OpSys> net arp
net: ARP who-has 10.0.2.2 sent, waiting reply...
net: RX 60 bytes dst 52:54:0:12:34:56 type 806
net: ARP reply! sender=52:55:0:2:2
```

第一行来自 `shell.c:2550`；第二行的目的 MAC 是本机（`shell.c:2560-2561`）；第三行的 sender 是 slirp 网关的虚拟 MAC（`shell.c:2566-2567`）。若 3 s 内没有合规应答，打印 net: no reply within timeout。

```text
OpSys> net ping 10.0.2.2
net: ping 10.0.2.2 ...
net: reply OK
OpSys> net stats
net: rx=12 tx=9 err=0
```

net tcp 的完整用法（需要宿主侧主动连入，见 7.4）：

```bash
# 宿主机另一个终端（QEMU 需带 hostfwd=tcp::8080-:8080）
$ nc 127.0.0.1 8080
hello
```

```text
OpSys> net tcp 8080
net: tcp listening on 8080 ...
net: tcp accepted 10.0.2.2:xxxxx
net: tcp recv 5 bytes: hello
```

（三行分别来自 `shell.c:2673, 2680-2682, 2686-2688`；随后 shell 用 TCP_SEND 回发同一段数据并 TCP_CLOSE（`shell.c:2689-2697`）——宿主侧的 nc 会看到回显。）

### 6.3 与文档/实现不一致处（以代码为准）

| 项 | 事实 |
| --- | --- |
| 无 ip 子命令 | 虽然有 NET_OP_SET_IP（opcode 5），shell 并未暴露；静态地址只能由服务自身在启动时设置（`main.c:757-762`） |
| 无 udp 子命令 | 4 个 UDP opcode 全部没有命令行前端（`net.h:49-51` 定义、`main.c:561-608` 实现，shell 未调用） |
| usage 串三处不一致 | 无参数时打印 Usage: net mac \| arp \| recv \| stats（`shell.c:2487`）；兜底分支打印 Usage: net mac \| arp \| ping <ip> \| tcp <ip> <port> <msg> \| recv \| stats（`shell.c:2702`）；注册描述串又只有 mac/arp/recv/stats（`shell.c:3771`）。实际 tcp 只接受一个端口参数，不接 ip/msg |
| net 不在救援名单 | 策略服务不可用时的强制放行列表（`shell.c:315-318`）不含 net；但 policy 的种子表中也没有 net 规则（`user/services/policy/main.c:83-111`），默认判为 POLICY_UNSET（放行），见第八节 |

---

## 七、QEMU 网络配置

### 7.1 现状：仓库默认不挂网卡

| 入口 | 实际参数 | 是否含网卡 |
| --- | --- | --- |
| make run | -cdrom build/opsos.iso -m 256M -serial stdio -d int,cpu_reset,guest_errors -drive file=disk.img,... -device virtio-blk-pci,drive=vd,disable-modern=on | **无**（`Makefile:326-333`） |
| make debug | 同上加 -nographic -serial mon:stdio -s -S | **无**（`Makefile:335-345`） |
| scripts/run.sh | -cdrom -m -nographic -serial mon:stdio -d int,cpu_reset,guest_errors | **无**（`scripts/run.sh:84-90`） |

全仓库没有任何 -netdev / -device pcnet 字样（对 Makefile、scripts/、docs/、README.md 的检索结果为空）。也就是说：**net 服务默认把「本配置无网卡」当作正常运行场景处理**。

### 7.2 无网卡时会发生什么

1. net 服务被 manager 正常拉起（`user/services/manager/manager.c:540-541`），启动时打印 net: starting PCnet-Fast III driver。
2. PCI 枚举里没有 0x1022:0x2000，于是打印 net: no PCnet adapter found，进入 for (;;) Sleep(10) 常驻（`user/services/net/main.c:683-687`）。
3. 端口 "net" **不会**被注册（注册发生在第 8 步，`main.c:765-774`），因此 shell 的 net 命令统一返回 net: 'net' port unavailable (-4)。
4. 不影响其余服务：net 的 restartable 标志为 0（`manager.c:148`），manager 不为它建监控线程。

### 7.3 推荐参数（含本机实测）

在 make run 的基础上追加一行即可：

```bash
qemu-system-x86_64 \
    -cdrom build/opsos.iso \
    -m 256M \
    -serial stdio \
    -d int,cpu_reset,guest_errors \
    -netdev user,id=n0 \
    -device pcnet,netdev=n0 \
    -drive file=disk.img,if=none,id=vd,cache=writethrough \
    -device virtio-blk-pci,drive=vd,disable-modern=on
```

需要 TCP 入向测试时给 netdev 加 hostfwd（把宿主的 8080 转发到 guest 的 8080）：

```bash
    -netdev user,id=n0,hostfwd=tcp::8080-:8080
```

本机验证（QEMU 10.2.2，Fedora 44）：

```text
$ qemu-system-x86_64 -M pc -m 64 -display none -S -monitor stdio \
      -netdev user,id=n0 -device pcnet,netdev=n0
(qemu) info pci
  Bus  0, device   3, function 0:
    Ethernet controller: PCI device 1022:2000
      BAR0: I/O (not mapped)
```

即：-device pcnet 暴露的正是驱动所需的 **1022:2000**，且 BAR0 为 I/O 空间——与 `user/services/net/main.c:68-69, 680` 的假设完全一致；默认 MAC 为 52:54:00:12:34:56，可直接用于核对 net mac 的输出。

> 说明：驱动注释指向 QEMU 10.2 的行为细节（总线主控开关、INTx 电平触发问题，`main.c:22-25, 709-717`），本文的 QEMU 参数也只在本机 QEMU 10.2.2 上核对过设备名与 PCI ID；更早版本设备名同为 pcnet，但未逐一实测。

### 7.4 slirp 用户态网络的可用性与限制

net 服务启动时把地址硬编码为 slirp 的默认网段（`user/services/net/main.c:757-761`）：

```c
static const u8 ip[4] = {10, 0, 2, 15};   /* guest */
static const u8 gw[4] = {10, 0, 2, 2};    /* slirp 网关 */
ProtoInit(ip, gw);                         /* 打印 "net: stack up 10.0.2.15 gw 10.0.2.2" */
```

net arp 也因此固定探测 10.0.2.2（`user/services/shell/shell.c:2538-2540`）。gw 只被保存、不参与选路（见 5.3）。

| 能力 | 现状 | 依据 |
| --- | --- | --- |
| guest 主动 TCP/UDP 外连 | 可用（slirp 提供 NAT） | QEMU user-mode networking 的既有语义；本仓库未做自动化验证 |
| 外部主机主动连入 guest | 默认不可达，需要 hostfwd 端口转发 | 同上；shell 的 TCP 用例注释明确依赖 hostfwd（`user/services/shell/shell.c:2649-2653`） |
| ICMP echo 对外转发 | **取决于宿主配置**（slirp 需要宿主允许非特权 ICMP 才能代发 echo） | QEMU 文档提交「doc: slirp supports ICMP echo if enabled in Linux」<https://patchwork.ozlabs.org/project/qemu-devel/patch/1405692553-564-5-git-send-email-mjt@msgid.tls.msk.ru/> |
| ping 10.0.2.2（虚拟网关） | 通常可用（slirp 自身即网关）；**本文未实测** | 属 QEMU 行为约定，仓库内无证据 |
| DHCP | 未实现（无 DHCP 客户端，地址硬编码） | 全仓库检索无 dhcp 相关代码 |
| DNS | 未实现（无解析器、无域名接口，所有 API 只接受 4 字节 IP） | `user/services/net/proto.h:30-56` 全部形参都是 const u8 ip[4] |
| IPv6 | 未实现（只处理 version = 4，`proto.c:330-331`） | — |
| 混杂模式 / 广播 | 只接收目的 MAC 为本机或首字节 0xFF 的帧（`proto.c:309-317`） | — |
| 本机环境 | /proc/sys/net/ipv4/ping_group_range = 0 2147483647（允许任意用户组使用 ping socket） | 本机实测，仅影响宿主侧 ICMP 转发能力 |

### 7.5 调试建议

| 目标 | 做法 |
| --- | --- |
| 确认驱动是否找到网卡 | 看串口日志 net: PCnet at PCI[i] ...、net: IO base 0x..., IRQ ...、net: MAC ...、net: PCI command=0x... (bus master ON)（`main.c:676-723`） |
| 确认 DMA 池可用 | net: DMA pool phys=0x... va=0x50000000；失败会打印 shm_create failed (no ATOM_SERVICE_MANAGE?)（`main.c:730`） |
| 判断收发是否真的工作 | net stats 的 rx / tx / err 计数；发送自检失败会有 net: TX timeout len=... 日志；PCI 布局可用 QEMU monitor 的 info pci / info qtree 对照 7.3 |
| 抓原始帧 | net recv（只显示前 64 字节）；更完整的观测需要客户端自行解析（见 4.2 的 RECV 响应） |
| 观察中断与异常 | -d int,cpu_reset,guest_errors（`Makefile:331`） |

---

## 八、安全与权限

### 8.1 三层门控现状

| 层 | 门控对象 | 现状 | 依据 |
| --- | --- | --- | --- |
| 硬件寄存器 | SYS_IO_READ8/WRITE8/READ16/WRITE16 | **已接入**：CAP_TYPE_IO_PORT 覆盖 [base, base+count)，驱动申请 (0x20 << 16) \| io_base | `kernel/syscall/syscall.c:1247-1273`；`user/services/net/main.c:696-698` |
| PCI 配置空间 | SYS_PCI_CFG_READ/WRITE | **已接入**：CAP_TYPE_PCI_DEV（obj_id = 枚举索引，R+W）**且** ATOM_SERVICE_MANAGE | `kernel/syscall/pci.c:240-256, 266-272, 284-300, 307-327` |
| DMA 内存 | SYS_SHM_CREATE | **已接入**：ATOM_SERVICE_MANAGE（把物理页暴露给设备属特权操作） | `kernel/mm/shm.c:113` |
| 服务身份 | blob 内容身份播种 | **已接入**：spawner 必须持 ATOM_SERVICE_MANAGE，且新进程 ELF 必须与内嵌 ELF 逐字节相同，"net" 在名单内 | `kernel/syscall/process_desc.c:304-330` |
| IPC 端口 | PortGet / IpcCall | **未接入**：只校验端口存在与长度 ≤ 4096，无能力检查 | `kernel/ipc/ipc.c:507-517, 675-686` |
| 网络原子 | ATOM_NET_BIND / ATOM_NET_CONNECT | **未接入** net 服务（详见 8.2） | `user/services/net/*` 无任何 atom / CapHasAtom 调用 |
| 命令层 | shell 命令策略 | **部分接入**：shell 启动时向 policy 服务查角色判决，DENY 则拒执行；但 policy 种子表无 net 规则，默认放行，且 net 不在救援名单 | `user/services/shell/shell.c:249-311, 1414-1416, 315-318`；`user/services/policy/main.c:83-111` |

### 8.2 ATOM_NET_CONNECT / ATOM_NET_BIND 的接入现状

原子本身已在权限模型中就位，但**没有任何一条网络路径在工作时检查它们**：

| 位置 | 用途 | 是否门控网络操作 |
| --- | --- | --- |
| `kernel/include/kernel/atom.h:49-52` | 定义 ATOM_NET_BIND、ATOM_NET_CONNECT、ATOM_NET_WIFI_SCAN、ATOM_NET_WIFI_SET | 否（仅枚举） |
| `user/services/init/main.c:500, 517, 529-545` | init 的 P0 自测：用 ATOM_NET_CONNECT / ATOM_NET_BIND 做能力创建与吊销（CapRevokeByAtom）回归 | 否 |
| `user/services/perm/perm-manager.c:1355, 1364, 1377` | 角色 × 原子决策表：OWNER / ADMIN 对 ATOM_NET_CONNECT 判 ALLOW，GUEST 判 DENY | 否（决策引擎，不是网络调用点） |
| `user/services/pkg/pkg_manager.c:130-131` | .ops 清单里的权限名 net.bind / net.connect 映射到原子 | 否（签发阶段） |
| `docs/ops_format.md:81-82` | 文档化 net.bind / net.connect 权限键 | 否 |
| `user/services/net/**` | — | **无任何引用**（atom、CapHasAtom、CapLookupByAtom 检索为空） |

结论（明确标注）：**网络操作当前没有原子门控**。任何能解析到 "net" 端口的 Ring 3 进程都可以：读 MAC、发送任意以太网帧（NET_OP_SEND，可伪造源 MAC）、收发任意 IP 载荷（NET_OP_IP_SEND）、绑定任意 ≥ 16 的 UDP 端口、监听任意 ≥ 16 的 TCP 端口，以及取走全局 UDP 收队列中的数据报。这属于「能力层尚未接线」的已知缺口，而非设计意图。

### 8.3 攻击面与加固建议

| 攻击面 | 现状 | 建议 |
| --- | --- | --- |
| 任意外发原始帧 | NET_OP_SEND 无门控，客户端可绕过协议栈构造任意帧 | 用 ATOM_NET_CONNECT 门控；或对原始帧 opcode 引入独立的 CAP_TYPE_SERVICE 端口能力 |
| 监听 / 绑定任意端口 | TCP_LISTEN / UDP_BIND 只校验端口 ≥ 16 | 用 ATOM_NET_BIND 门控（< 1024 需额外授权，见 `docs/permission_model.md:100`） |
| 全局 UDP 收队列 | 不按端口分流，存在跨客户端窃听 | 队列按绑定槽分片，或在 IpcRecvFrom 的 sender subject 上做归属校验（内核已提供该身份，`user/lib/libos/syscalls.h:406-409`） |
| 静态身份 | 地址硬编码 10.0.2.15 | 引入 SET_IP 的权限校验与 DHCP 客户端 |
| 无差错报文 | UDP 未绑定端口静默丢弃；非法 TCP 段静默忽略 | 补 ICMP 端口不可达与 RST，注意限速以免成为放大源 |
| 无速率限制 | 帧收发无配额 | 结合 cap_entry_t.quota（`kernel/include/kernel/cap.h:64-76`）做配额门控 |

---

> **v0.9 更新**：本节写于 TCP 只有被动打开（LISTEN/ACCEPT）的阶段。现在 
> `NET_OP_TCP_CONNECT`(17) 与 `ProtoTcpConnect()` 已经落地：主动打开复用同一个 
> `TCP_STATE_SYN_SENT` 状态机，本地端口取 49152..65535 临时端口段，等待 SYN|ACK 
> 约 6 秒（600 × `NetYield()`，一个 tick = 10 ms），识别对端 RST 作为被拒，失败或超时 
> 都会把状态复位为 CLOSED 并清空 TCP 收队列；新增 `NET_OP_GET_IP`(18) 与 
> `ProtoGetIp()` 用于回读静态地址。shell 的 `http` 命令就建立在这条路径上 
> （`scripts/verify_tools.py` 用宿主机上的 Python HTTP 服务做了端到端往返验证）。 
> 下面“无主动打开”的描述只对当时成立。

## 九、已知限制

| 类别 | 限制 | 依据 |
| --- | --- | --- |
| 驱动 | 收包靠轮询，无中断；服务阻塞在 IpcRecv 时只能靠 8 个 Rx 描述符缓冲，超过即丢 | `main.c:22-25, 455-470` |
| 驱动 | Tx/Rx 超时是 100 万次忙等，占用 CPU 不让出 | `main.c:349-363, 383-397` |
| 驱动 | 无多播过滤（LADRF 全零）、无混杂模式、无 VLAN；池页数注释与实现不一致（注释 10 页 / 实际 9 页） | `main.c:36, 117, 140, 226-231` 对比 `:728` |
| IPC | 单线程服务：请求处理与协议栈收包串行，无异步 / 事件通知机制 | `main.c:457-661` |
| IPC | 端口无能力门控（见 8.1 / 8.2） | `kernel/ipc/ipc.c:507-517` |
| IPC | NET_OP_RECV 的 len / max 字段在实现中被忽略（`net.h:44` 注释与代码不符） | `net.h:43-46` 对比 `main.c:500-518` |
| ARP | TTL 写入后从不递减，条目永不过期；表满时覆盖第 0 项而非 LRU | `proto.c:100-123` |
| ARP | 解析失败要等约 6 s 才返回，期间同步阻塞客户端 | `proto.c:184-203` |
| IPv4 | 无分片 / 重组、无路由表、网关字段未使用、无 IP 选项、无 TTL 递减处理 | `proto.c:41, 214-216, 297` |
| ICMP | 只回 echo；ping 只判 type=0（不校验 id / seq / 源），任何 echo reply 都会让等待提前成功 | `proto.c:344-348, 396-402` |
| UDP | 绑定表 16 项、收队列 16 项且全局共享；不校验校验和、无拥塞 / 流控、无 ICMP 差错 | `proto.c:59-76, 349-355, 444` |
| TCP | 单连接；无主动打开（无 connect）；无重传 / 超时重发、无拥塞控制、无窗口管理（固定通告 4096）、无 MSS 选项、无 RST、无 TIME_WAIT、无乱序处理、收包不校验 TCP 校验和 | `proto.c:467-729` |
| TCP | ProtoTcpClose() 不发 FIN，属本地硬关闭；对端需靠自身超时感知 | `proto.c:568-573` |
| TCP | ProtoTcpAccept 的窗口是 6000 × 10 ms ≈ 60 s，而 `proto.h:62` 注释写「约 6 s」 | `proto.c:580` 对比 `proto.h:62` |
| 配置 | 无 DHCP、无 DNS、无 IPv6、无 socket 抽象；地址只能由编译期常量或 SET_IP 硬设 | 全仓库检索 |
| 工具链 | shell 未暴露 ip / udp 子命令；usage 串三处不一致；无自动化测试 | `shell.c:2485-2704, 3771`；`scripts/*.py`、`docs/test_report.md` 无 net 用例 |
| 文档 | `proto.h:42` 写 ping 超时约 1.5 s，代码为约 6 s | `proto.h:42` 对比 `proto.c:390` |

---

## 十、后续路线

按「先补正确性、再补能力、最后补抽象」的顺序推进；每项都给出当前代码里的落点。

### 10.1 第一阶段：把单连接做扎实

1. **收包解耦**：NetServiceRx() 移入独立线程（队列锁已就绪，`main.c:745`），或用 SYS_BIND_IRQ 绑定 IRQ 并按 CSR0 的 RINT 位收包（位定义见 `main.c:81-99`）；前置条件是解决 8259 电平触发风暴（`main.c:22-25`）。
2. **TCP 重传与超时**：为 s_tcp_snd_nxt 增加未确认段副本与 RTO 定时器（可复用 GetTime()，`proto.c:598`），并引入 TCP_RST（`net.h:95` 定义未用）拒绝非法段。
3. **乱序与去重**：`proto.c:659` 只接受 seq == rcv_nxt，可先补「重复段丢弃 + 乱序缓存」。
4. **关闭语义**：ProtoTcpClose() 改为先发 FIN 再进入 FIN_WAIT。

### 10.2 第二阶段：多连接与队列正确性

1. **连接表**：单组 s_tcp_* 变量（`proto.c:490-499`）改为 tcp_conn_t 数组并按四元组查表；TCP_ACCEPT 返回连接句柄，SEND / RECV / CLOSE 带句柄（扩展 `net.h` 时注意维持 4096 上限）。
2. **UDP 队列按端口分片**：全局 s_udp_rxq 改为 per-bind 队列，消除跨客户端窃听（配合 IpcRecvFrom 的 sender subject，`user/lib/libos/syscalls.h:406-409`）。
3. **ARP 老化**：主循环按 tick 递减 ttl（当前无递减点，`proto.c:102, 121`），表满时按 TTL 淘汰。
4. **主动打开**：新增 NET_OP_TCP_CONNECT 走 SYN_SENT（`proto.c:476-480` 已预留该状态），使 slirp 下可「guest 主动外连」而不依赖 hostfwd。

### 10.3 第三阶段：网络配置与命名

1. **DHCP 客户端**（RFC 2131 最小子集）：UDP 68 → 255.255.255.255，复用 ProtoUdpSendto / ProtoRx。
2. **DNS 解析**（A 记录，UDP 53 发往 slirp 的 10.0.2.3）：新增 NET_OP_RESOLVE，保持 API 的 const u8 ip[4] 形态。
3. **路由与网关**：让 s_gw 参与选路（当前仅保存，`proto.c:41, 297`），支持「非直连目的 → 网关 MAC」。
4. **ICMP 差错与分片**：补端口不可达（生成侧限速）与接收侧重组；发送侧继续不分片（`proto.c:214-216` 的 -8 保留为显式错误）。

### 10.4 第四阶段：抽象与安全

1. **原子门控接线**：ATOM_NET_BIND（bind / listen）与 ATOM_NET_CONNECT（connect / 原始帧）在服务入口检查；身份取 IpcRecvFrom 的 sender subject，配合 CapHasAtom（`user/lib/libos/syscalls.h:397-404`，net 已持 ATOM_SERVICE_MANAGE）。
2. **libnet 客户端库**：把 shell 手写的请求组装（`shell.c:2495-2700`）收敛为 `user/lib/libnet`（net_ping / net_udp_sendto 等）。
3. **socket 兼容层**：在 libnet 之上实现 socket / bind / sendto / recvfrom / accept 子集，作为可选库而非内核接口（对比 `docs/requirements.md:40, 65` 的 lwIP 规划）。
4. **零拷贝与统计**：借鉴 vfs 的共享物理页模式，减少「DMA 缓冲 → 驱动队列 → IPC 缓冲」三次拷贝（`main.c:432-438, 500-518`）；并在 rx / tx / err 之外补按协议、按错误的细分计数（`main.c:177-179`）。

---

## 十一、附录

### A. 源码索引

| 文件 | 行数 | 本文涉及的要点 |
| --- | --- | --- |
| `user/services/net/net.h` | 99 | opcode 1..16、信封结构体、NET_MTU、以太网 / IP / 协议常量、TCP 标志位 |
| `user/services/net/proto.h` | 65 | 协议栈对外 API（含 TCP 五个函数及其注释） |
| `user/services/net/proto.c` | 729 | ARP / IPv4 / ICMP / UDP / TCP 全部实现 |
| `user/services/net/main.c` | 779 | PCnet 驱动、DMA 环、纯轮询 Rx、IPC 服务循环、启动序列 |
| `user/services/shell/shell.c` | 4260 | CmdNet（2485-2704）、命令注册（3771）、策略过滤（249-311） |
| `kernel/syscall/pci.c` | 368 | PCI 扫描、配置空间读写与双门控 |
| `kernel/include/kernel/pci.h` | 56 | pci_device_info_t、PCI_MAX_BARS |
| `kernel/include/kernel/atom.h` | 63 | 网络原子定义 |
| `kernel/ipc/ipc.c` | — | IpcCall 长度校验（507-517）、端口名注册 / 解析（675-716） |
| `user/services/manager/manager.c` | — | 服务表（148）、SpawnService（402-426）、net 启动（540-541） |
| `Makefile` | 374 | net 源文件（146-147）、net.elf 链接规则（289-292）、run（326-333）、debug（335-345） |
| `scripts/run.sh` | — | QEMU 参数（84-90，无网卡） |

### B. 常量速查（全部来自 user/services/net/net.h）

| 常量 | 值 | 含义 |
| --- | --- | --- |
| NET_PORT_NAME / NET_MTU | "net" / 1514 | 端口注册名；MTU = 14（以太网头）+ 1500（载荷），不含 FCS |
| ETH_TYPE_IPV4 / ETH_TYPE_ARP | 0x0800 / 0x0806 | EtherType |
| IP_PROTO_ICMP / TCP / UDP | 1 / 6 / 17 | IP 协议号 |
| IP_HDR_LEN / UDP_HDR_LEN / TCP_HDR_LEN | 20 / 8 / 20 | 头长度 |
| TCP_FIN / SYN / RST / PSH / ACK | 0x01 / 0x02 / 0x04 / 0x08 / 0x10 | TCP 标志位 |

驱动与协议栈侧关键常量：NET_RINGS = 8、NET_BUF_SIZE = 2048、NET_RXQ_DEPTH = 32、NET_POOL_SIZE = 0x8400（`main.c:109-119`）；ARP_CACHE_MAX = 16、ARP_TTL_TICKS = 1000、UDP_SOCK_MAX = 16、UDP_RXQ_MAX = 16、TCP_RXQ_MAX = 8（`proto.c:45-76, 482`）。

---

> 返回 [文档索引](README.md)
