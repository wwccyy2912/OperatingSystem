# OpSys 项目需求规格说明书

> 版本：v1.0
> 日期：2026-07-25（初始规范）/ 2026-07-31（整理归档）
> 状态：实施中

---

## 一、系统总体架构

### 1.1 内核（微内核）

- **位置**：最高特权级（Ring 0），运行在单独的地址空间。
- **职责**：
  - 线程管理与调度（优先级抢占、多核负载均衡）。
  - 物理内存分配与虚拟内存管理（页表、映射）。
  - 能力（Capability）管理：所有资源访问通过能力句柄。
  - 进程间通信（IPC）：同步消息传递，支持共享内存。
  - 中断与异常分发（转发给用户态服务）。
  - 系统调用入口（极少的系统调用，如 `send`、`recv`、`map_memory` 等）。
- **代码量**：预计 1.5~2 万行（C + 汇编）。

> **微内核符合性要求**（架构评审补充）：内核仅保留机制（地址空间、线程、IPC、
> 能力、通知/中断转发），策略与驱动一律用户态实现。设备驱动（串口、显卡、
> 定时器）必须以用户态服务形式存在，内核只提供 IRQ 能力绑定与 MMIO 映射机制。

### 1.2 用户态系统服务

每个服务是一个独立进程，拥有最小必要能力。通过 IPC 与内核及其他服务通信。

| 服务名称 | 功能 | 所需能力 |
|---------|------|----------|
| **服务管理器** | 启动、监控、重启其他服务 | `CAP_SERVICE_MGMT` |
| **设备管理器** | PCI/USB 枚举，驱动加载 | `CAP_PCI`、`CAP_IO` |
| **文件系统服务** | 文件系统实现（FAT32、MFS） | `CAP_BLOCK_IO` |
| **图形服务** | 显示输出、合成、直接扫描输出 | `CAP_GPU`、`CAP_FRAMEBUFFER` |
| **输入服务** | 键盘、鼠标、触摸板事件 | `CAP_INPUT_HW` |
| **音频服务** | 混音、播放、录音 | `CAP_AUDIO_HW` |
| **权限管理器** | 记录应用权限、弹窗询问 | 无特殊能力（仅 IPC） |
| **网络服务** | lwIP 协议栈、socket API | `CAP_NET_CARD` |
| **屏幕捕获服务** | 录屏、截图、权限控制 | `CAP_SCREEN_CAST` |
| **包管理器** | 应用安装、更新、运行时管理 | `CAP_FS`、`CAP_NET` |
| **日志服务** | 收集系统日志 | `CAP_SERIAL` 或 `CAP_FS` |

### 1.3 应用层（沙盒环境）

- 每个应用使用 `.ops` 包格式安装，拥有独立的能力空间。
- 运行时与系统服务交互通过客户端库（如 `libaudio.so`、`libgraphics.so`）。
- 应用默认无法直接访问硬件，必须通过服务代理或用户授权后的直通路径。

---

## 二、模块划分与开发顺序

按依赖关系构建，每部分完成即可测试：

1. **内核基础**：启动、内存管理、线程、简单 IPC。
2. **用户态启动与服务管理器**：加载第一个用户程序（`init`），启动基础服务。
3. **能力系统与沙盒**：实现能力传递、权限管理服务。
4. **基础设备驱动**：串口（调试）、PCI 枚举、AHCI 磁盘。
5. **文件系统服务**：`tmpfs` + `FAT32`，实现 `/` 挂载。
6. **包管理器与运行时**：`.ops` 包安装，运行时共享（squashfs）。
7. **图形与输入**：帧缓冲驱动、合成器、窗口管理器、键盘鼠标。
8. **音频**：HD Audio 驱动，混音服务。
9. **网络**：网卡驱动，lwIP，socket 接口。
10. **屏幕捕获与多媒体**：录屏、硬件编码。
11. **开发者工具与虚拟化**：开发者模式、用户态 VMM。
12. **优化与兼容层**：Linux 系统调用兼容（可选）。

---

## 三、技术选型与依赖

### 3.1 编程语言

- **汇编**（NASM）：启动代码、中断入口、上下文切换。
- **C11**：内核核心模块、部分用户态服务（性能关键）。

> 现状注记：初始规范曾提及 Rust 2021；实际项目以 C11 + NASM 实现。

### 3.2 构建系统

- **Make**：内核与用户态统一构建。
- 自定义脚本：生成镜像（`grub-mkrescue`）、打包 `.ops`（未启动）。

### 3.3 测试与调试

- **QEMU**（x86_64）：模拟目标硬件，支持 GDB 调试。
- **VirtualBox**：真机近似环境验证（含 BIOS 差异）。
- **集成测试**：内核自带 syscall 自测（24 项：20 项既有 + vspace_alloc / thread_set_ctx / 1000 线程 / 100k IPC），QEMU 串口驱动验证。

### 3.4 第三方库（用户态，规划）

- **lwIP**：轻量级 TCP/IP 协议栈。
- **FatFs**：FAT32 文件系统（移植到文件系统服务）。
- **TinyUSB**（可选）：USB 驱动栈。
- **Mesa**（可选）：用户态 OpenGL/Vulkan 驱动（用于直通路径）。
- **libpng / libjpeg**：图像解码。

---

## 四、关键接口定义

### 4.1 内核系统调用（约 12 个）

```c
// 能力管理
cap_t cap_create(void);
int cap_grant(cap_t cap, pid_t target, rights_t rights);
int cap_revoke(cap_t cap);

// IPC
int send(port_t port, const void *msg, size_t len);
int recv(port_t port, void *buf, size_t *len);
int call(port_t port, const void *req, size_t req_len, void *resp, size_t *resp_len);

// 内存
void *map_memory(cap_t mem_cap, size_t offset, size_t size, int prot);
int unmap_memory(void *addr, size_t size);

// 线程与调度
pid_t thread_create(void (*entry)(void *), void *arg);
void thread_exit(int code);
void thread_yield(void);
int thread_set_affinity(pid_t tid, int cpu);

// 特殊
int debug_log(const char *str);
```

### 4.2 用户态服务 IPC 协议示例（基于 `call`）

**图形服务**：
- 请求：`CreateSurface { width, height, format }` → 响应：`surface_id`
- 请求：`Present { surface_id, x, y, wait_vsync }`

**文件系统服务**：
- 请求：`Open { path, flags }` → 响应：`fd`（能力句柄）
- 对 fd 可调用 `Read { fd, offset, size }` 等。

所有协议使用简单结构体 + 序列化（便于跨语言）。

### 4.3 能力类型

- `CAP_THREAD`：控制线程（暂停、恢复、查询）。
- `CAP_PORT`：IPC 端口（发送/接收）。
- `CAP_MEM`：内存区域（映射、共享）。
- `CAP_IRQ`：中断号（绑定到线程）。
- `CAP_IO_PORT`：I/O 端口范围（`in`/`out`）。
- `CAP_PCI_DEV`：PCI 设备配置空间访问。
- 用户态能力（扩展）：由服务自己定义，内核不解释。

---

## 五、安全模型实现要点

### 5.1 最小能力初始化

- `init` 进程启动时只有 `CAP_PORT`（与内核通信的端口）和 `CAP_SELF`。
- `init` 通过服务管理器启动其他服务，**显式授予**所需能力（例如授予音频服务 `CAP_AUDIO_HW`）。

### 5.2 沙盒应用启动

```c
cap_t app_caps = cap_create();  // 空能力表
for each perm in manifest.permissions {
    if (perm == "network") cap_grant(network_cap, app_caps, READ|WRITE);
    // 其他权限...
}
pid_t pid = thread_create(app_entry, app_caps);
```

### 5.3 权限管理器策略

- 存储路径：`/etc/perm-store/<app-id>.json`
- 弹窗通过 `perm-manager` 调用图形服务的对话框 surface。
- 用户选择后，`perm-manager` 向内核请求授予临时或永久能力。

> **架构评审补充**：安全模型以 capability 为准，**不在内核维护 UID/GID 凭据
> 策略**（凭据检查属于用户态安全策略层）。

---

## 六、性能优化策略（关键路径）

### 6.1 IPC 优化

- 小消息（≤64 字节）通过寄存器传递，不经过共享内存。
- 大消息使用共享内存 + 一次能力传递（避免拷贝）。
- 内核提供快速路径（绕过部分检查）。

### 6.2 图形直通

- 应用请求 `CAP_GPU_DIRECT` 后，内核将 GPU 的 MMIO 和 VRAM 映射到其地址空间。
- 应用直接提交命令缓冲区，无需经过图形服务。

### 6.3 实时音频

- 音频服务运行实时线程。
- 应用可授予 `CAP_AUDIO_REALTIME`，直接写 DMA 缓冲区（独享设备）。

### 6.4 CPU 亲和性与隔离

- 内核支持将指定核心从调度器中隔离（`isolcpus` 风格）。
- 实时应用绑定到隔离核心，不受干扰。

---

## 七、目录结构

```
os-project/
├── kernel/               # 内核代码
│   ├── arch/x86_64/      # 平台相关（汇编、启动）
│   ├── include/          # 内核头文件
│   ├── mm/               # 内存管理
│   ├── sched/            # 调度器
│   ├── ipc/              # 消息传递
│   ├── cap/              # 能力系统
│   └── syscall/          # 系统调用入口
├── user/                 # 用户态代码
│   ├── lib/              # 客户端库（libc, libipc...）
│   ├── services/         # 系统服务（每个子目录一个服务）
│   │   ├── init/         # 第一个用户程序
│   │   ├── shell/        # 命令行
│   │   └── ...           # 其余服务
│   └── runtime/          # 运行时映像构建脚本
├── boot/                 # 引导程序（GRUB 配置、多引导头）
├── build/                # 构建输出（自动生成）
├── scripts/              # 辅助脚本（测试、打包、镜像生成）
└── docs/                 # 设计与规范文档
```

---

## 八、测试策略

### 8.1 单元测试（宿主环境）

- 每个内核模块编写宿主测试，使用 `gcc` 直接运行。

### 8.2 集成测试（QEMU）

- 启动 QEMU，通过串口发送命令，捕获输出，判断是否符合预期。
- 内核 syscall 自测套件（当前 24 项）：IPC、内存映射、线程、能力、vspace、thread_ctx、压力测试。

### 8.3 压力测试

- 创建 1000 个线程，循环 yield，观察调度是否崩溃。
- 大量 IPC 消息（10 万次），测量延迟分布。

### 8.4 安全测试

- 尝试从应用层发送恶意消息（非法能力、格式错误），内核应返回错误而不是崩溃。
- 尝试能力提升攻击（如重复释放、UAF），依赖手动审计。

---

## 九、文档要求

- **代码注释**：每个公共接口函数必须有 Doxygen 风格的注释。
- **设计文档**：`docs/` 下包含：
  - `kernel.md`：内核接口与实现细节。
  - `ipc_protocols.md`：各服务之间的消息格式。
  - `security_model.md`：能力系统与沙盒策略。
- **用户手册**：如何构建、安装、使用包管理器。

---

## 十、里程碑与交付物

| 版本 | 内容 | 状态 |
|------|------|------|
| **v0.1** | 内核 + 用户态 init + 串口日志 + 简单 IPC | ✅ 已完成 |
| **v0.2** | 能力系统 + 沙盒应用（`hello.ops`） | ✅ 已完成 |
| **v0.3** | 文件系统 + 包管理器（可安装/运行应用） | ✅ 已完成（Phase A 验收 6/6 通过，2026-08-16） |
| **v0.4** | 图形 + 窗口管理器（显示桌面） | 🔄 最小闭环已完成（window_demo：经 term 渲染 3 窗口 + 键盘焦点切换，2026-08-21）；完整合成器/真实窗口注册表待续 |
| **v0.5** | 音频 + 网络 | ⏳ 规划 |
| **v0.6** | 完整多媒体 + 开发者模式 | ⏳ 规划 |
| **v1.0** | 稳定版，支持多核、性能优化、文档齐全 | ⏳ 规划 |

---

## 十一、代码编写规范（摘要）

### 通用

- 缩进：4 空格；行宽 ≤ 100 字符；UTF-8 + LF。
- 文件头注释：版权、简要描述。函数注释：功能、参数、返回值、注意事项。

### C 语言

- 头文件保护：`#ifndef KERNEL_XXX_H` / `#define` / `#endif`。
- 全局函数：`模块名_动词_名词`（如 `pmm_alloc_page`）；静态函数：`_前缀`。
- 全局变量 `g_` 前缀；静态全局 `s_` 前缀；局部小写下划线。
- 类型 `_t` 后缀；常量/宏全大写（`PAGE_SIZE`）。
- K&R 括号风格。

### 汇编（NASM）

- 全局标号 `_` 前缀（`_start`）；局部标号 `.` 前缀（`.loop`）。
- Intel 语法；System V AMD64 ABI（RDI/RSI/RDX/RCX/R8/R9）。
- Callee-saved 寄存器（RBX、RBP、R12-R15）必须恢复。

### 错误处理

- C：返回错误码（负数），通过指针参数返回结果。
- 汇编：错误码放入 `eax`/`rax`。

### 日志与调试

- 内核关键路径使用 `KERN_LOG_*` 宏或串口调试输出。
- boot.asm 早期调试通过 COM1 输出单字符（A/C/D 断点标记）。
