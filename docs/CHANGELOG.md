# OpSys 版本沿革（CHANGELOG）

> 适用版本：OpSys v1.0-dev（HEAD 105805d + 工作区）　|　最后更新：2026-09-20
>
> 本文按时间顺序记录 OpSys 的开发演进，内容依据仓库的 72 次提交（`git log`）整理，并按主题归并为阶段。**版本号命名存在历史混用**（例如"v1.3"实为 shell/TUI 功能迭代，落在 v0.5 与 v0.6 之间），本文保留提交中的原始标签以便追溯。

---

## 总览

| 阶段 | 时间 | 主题 | 关键产物 |
| --- | --- | --- | --- |
| 奠基 | 2026-08-11 | 权限模型 P0、仓库初始化 | 项目骨架 |
| v0.1 | 2026-08-15 | 内核 + init + 串口 + IPC 打通 | 可引导 ISO + shell |
| Phase 0–2 收尾 | 2026-08-17 | runtime_demo 接入、命名 UNIX 化、多组件许可 | 许可体系、VFS/term 加固 |
| 生产加固 | 2026-08-22 | 零拷贝读、崩溃恢复、v0.4 窗口化基础 | 审计修复批次 |
| v0.4 | 2026-08-22 | 完整窗口管理器（wm + libwm + wm_demo） | 文本桌面 |
| v0.5 | 2026-08-23 | 账户服务、退出保护、环境变量、命令策略、TUI 交互库 | 多用户 + 策略三层 |
| v1.3（功能标签） | 2026-08-24 | Shell 历史/补全、fm 文件管理器、TUI 组件库 | 交互式 shell |
| v0.6.x | 2026-08-25 | 架构优化：性能、安全（W^X/栈保护）、UX、去冗余 | 0 新警告的清洁构建 |
| v0.7.x | 2026-08-26 | SYSCALL 快速路径、DMA 重试、金丝雀自检、滚动历史、磁盘工具 | 性能与可靠性 |
| GUI 线 | 2026-08-27～29 | 像素渲染库、合成器、鼠标、脏区、事件隔离、CJK | 像素桌面 |
| 网络线 | 2026-08-29 | PCnet 驱动、ARP/IPv4/ICMP/UDP/TCP | 可用的最小协议栈 |
| 国际化 | 2026-08-29 | TUI/GUI UTF-8、CJK 字模、拼音 IME | 全链路中文支持 |
| 收口 | 2026-09-01 | 函数名 PascalCase 统一、GPLv3 许可头、头部结构图注释 | 代码风格与许可合规 |

---

## 一、奠基与 v0.1（2026-08-11 ～ 08-15）

| 日期 | 提交 | 内容 |
| --- | --- | --- |
| 08-11 | `5b3ba1d` | Initial commit |
| 08-11 | `35305f0` | **用户权限模型 P0** —— 权限模型的第一版落地（身份/角色/原子权限的地基） |
| 08-11 | `adab618` 等 | 仓库清理（构建产物、IDE 配置、临时文件移出版本库） |
| 08-15 | `90a737d` | **v0.1 预发布版本-0** |
| 08-15 | `613bcb8` | **v0.1-2** |
| 08-15 | 多次 `README.md` 更新 · 删除 `kernel.elf` · 删除 `AGENTS.md` | 文档与产物治理 |

**这一阶段确立了整个项目的形态**：x86_64 Long Mode + Multiboot2/GRUB2 引导的内核骨架，Ring 0 只保留调度、内存、IPC、能力；init 作为 PID 1 在用户态完成自检后由 manager 拉起全部服务；服务的 ELF 以 blob 形式嵌入内核镜像，使得"没有文件系统也能启动系统"。

---

## 二、VFS/Runtime 收尾与许可体系（2026-08-17）

| 提交 | 内容 |
| --- | --- |
| `409a748` | **runtime_demo 接入** + debug_log 预算修复 + **VFS/term 安全加固** —— 把 C Runtime 的演示程序接进构建，同时收紧 VFS 与终端的输入校验 |
| `6918a97` | **shell 命令重命名为标准 UNIX 等价名**（`bm_create` → `bm create`、`perm_answer` → `perm answer` 等），下划线形式保留为别名 |
| `3b273ba` | **许可重构为多组件模型**：kernel/服务核 GPLv3、libc/Runtime/扩展服务 LGPLv3、docs CC BY 4.0 |

---

## 三、生产加固与 v0.4 窗口化（2026-08-22）

| 提交 | 内容 |
| --- | --- |
| `728d78b` | **production hardening**：审计问题修复、**零拷贝读路径**（`ShmCreate`/`ShmMap`）、**崩溃恢复**（manager 的 `ProcessWait` 监控 + 自动重启）、**v0.4 窗口化**基础 |
| `40aafc7` | **用户账户服务 + 退出保护 + TUI 交互库 + shell 命令完善** —— `user` 服务（登录/改密/建号）、`stop` 的三重保护、`libtui` 的掩码输入行与确认框 |
| `3a99475` | **v0.4：完整窗口管理器** —— `wm` 服务（窗口注册表 + 合成器 + 焦点路由）、`libwm` 客户端库、`wm_demo` 三窗口桌面 |
| `3d401f4` `8b90192` `be76ca5` `61ead9c` | README 的"特别鸣谢"章节与排版 |

---

## 四、v0.5：账户、策略与环境（2026-08-23）

| 提交 | 内容 |
| --- | --- |
| `34dfd06` | **环境变量**（`export`/`unset`/`env`，libc `getenv/setenv`）+ **命令策略三层架构**（新增 `policy` 服务：Capability → Policy DB → Shell 覆盖） |
| `f258a74` | **动态内存优化**：malloc 大小分桶 + PMM next-fit；**修复 `useradd` 角色漏洞** |
| `3f74d78` | **账户锁定/解锁** + 5 次失败自动锁定策略 |
| `57ac8ba` | **启动/自检流程优化**：**selftest fail-fast**（失败不再被静默忽略）+ 结果汇总 |
| `052a965` | Shell 命令全量检查，修复 `stop` 错误分支的 em-dash 输出问题 |
| `1c34e20` | **命令策略运行时热更新**（`policy_set` / `policy_dump`） |
| `9647350` | 修复 `kill`/`users`/`whoami`；新增 `cd`/`pwd`/`shutdown`；UNIX 风格命令别名 |

---

## 五、交互式 Shell 与 TUI 组件（2026-08-24，提交标签 "v1.3"）

| 提交 | 内容 |
| --- | --- |
| `37568c3` | **TUI 组件库 `tui_menu`** + **`fm` 文件管理器** + 命令交互 TUI 化（`kill` 进程选择器等） |
| `518c129` | **Shell 命令历史 + Tab 补全** + `fm` 重命名/复制 + `mv` 的 TUI 目录选择 |

---

## 六、v0.6.x：架构优化系列（2026-08-25）

一条"性能 / 安全 / UX / 可读性 / 去冗余"的连续迭代线：

| 版本 | 提交 | 内容 |
| --- | --- | --- |
| v0.6 | `0c6285e` | 架构优化总批次 |
| v0.6.1 | `6c95ed1` | **死代码消除**（`--gc-sections`）+ **W^X 段布局**（`user.ld` 显式 PHDRS）+ 拷贝热路径优化 |
| v0.6.2 | `ca4cfd3` | **用户态栈保护** + **ELF 段重叠校验** + malloc `bin_index` 越界修复 |
| v0.6.3 | `ef5a92e` | 死字段清理（`time_slice` / `cred` / `persona_id`）+ `ipc_strcmp` 去重 |
| v0.6.4 | `a45f9b1` | **bash 风格 cwd 提示符** `opsys:<cwd>$` + 死宏清理 |
| v0.6.5 | `28b86dc` | malloc `block_is_free` **O(1) 快速路径** + 映射范围守卫（修复自引入崩溃） |
| v0.6.6 | `499e534` | **启动画面** + TUI 滚动指示符 + 段选择子去重 |
| v0.6.7 | `3ef722e` | vfs 句柄死字段移除 + `fallocate` 错误标签修正 |
| v0.6.8 | `e97a30b` | 热路径串口噪音清除 |
| v0.6.9 | `5100647` | shell 魔法数字命名化 |

---

## 七、v0.7.x：性能、可靠性与运维（2026-08-26）

| 版本 | 提交 | 内容 |
| --- | --- | --- |
| v0.7 | `58e2c52` | **Track 1：SYSCALL 指令快速路径** —— 用 `SYSCALL/SYSRET` 取代 `int 0x80`，并处理"CPU 不切栈"带来的用户 RSP 保存问题 |
| v0.7 | `e964826` | Track 2/3/4：**DMA 超时重试**（virtio-blk 可靠性）、**canary 自检**（`canarytest` 服务）、**term 滚动历史** |
| v0.7.1 | `3c46ee5` | 启动交互修复 + **`disk` 工具** + `ls` 默认当前目录 |
| v0.7.1 | `54b4020` | 启动时不再向用户弹出 init 测试的权限窗口 |
| v0.7.1 | `4a4ff6b` | `cd ..` / `fm` 残留修复 / 历史命令可编辑 / **`exec` 支持磁盘文件** |

---

## 八、GUI 线：从像素库到像素桌面（2026-08-27 ～ 08-29）

| 阶段 | 提交 | 内容 |
| --- | --- | --- |
| P1+P2 | `7732671` | **`libgui` 像素渲染库**（点/矩形/线/边框/blit/文本，32bpp 与 24bpp 双格式）+ **PS/2 鼠标驱动** |
| P3+P4 | `812e7ca` | **像素合成器服务 `gui`**（每窗口离屏缓冲 + Z 序 + 事件队列）+ **`gui_demo` 桌面** |
| 优化 | `0f0a328` | **脏区合成** —— 只重绘变化区域以提升刷新率 |
| 交互 | `6bab0cb` | 窗口边框重叠修复 + **标题栏拖动** |
| 交互 | `fd3a0f0` | **键盘输入跟随焦点窗口** |
| 修复 | `6970f45` | 重叠窗口下层穿透修复 |
| 修复 | `f0c1261` | 悬停下层标题栏穿透修复 |
| 阶段3 | `3ee600c` | **国际化**：CJK 渲染 + `RESIZE` + 隐式 grab + 滚轮 |
| 隔离 | `5a9f416` | **事件按窗口 owner 隔离**（修复跨窗口事件串扰） |
| 控件 | `8bb979c` | **标题栏按钮**（关闭 / 最大化） |
| 收尾 | `6ff36da` | 标题栏按钮焦点门控、最大化缓冲重分配、**最小化任务栏**、**末窗自动退出** |

---

## 九、网络线：从网卡驱动到 TCP（2026-08-29）

| 阶段 | 提交 | 内容 |
| --- | --- | --- |
| 驱动 | `c227d56` | **PCnet-Fast III (AM79C973) 网卡驱动** + PCI 配置空间访问（`SYS_PCI_CFG_READ/WRITE`，受 `CAP_TYPE_PCI_DEV` 门控） |
| 阶段4 | `472b715` | **协议栈**：ARP 缓存 + IPv4 + ICMP + UDP |
| 阶段5 | `683ab56` | **TCP 单连接状态机** + shell `net tcp` 测试 |
| 修复 | `108f690` | TUI / GUI / 网络阶段 1 的高危 bug 修复 |

---

## 十、国际化（2026-08-29）

| 提交 | 内容 |
| --- | --- |
| `9f1171b` | **TUI 阶段 2**：ANSI 转义序列 + 颜色 cell + **UTF-8/CJK 渲染** |
| `ed9f082` | **全系统 UTF-8 支持**：输入（**拼音 IME**）、编辑、渲染与存储 |

---

## 十一、收口：命名、许可与文档注释（2026-09-01）

| 提交 | 内容 |
| --- | --- |
| `4877fcd` | **kernel**：微内核化审计 + **函数名统一 PascalCase** + GPLv3 许可头 + 头部结构图注释 |
| `aa2b97b` | **user**：函数名统一 PascalCase + GPLv3 许可头 + 库与服务头部结构图注释 |
| `105805d` | **build**：Makefile / 链接脚本 / shell 脚本补齐 GPLv3 许可头，恢复文件许可头补齐 |

自此次提交起，全部 C 源码统一采用"SPDX 许可标识 + 结构（Structure）/ 工作原理（How it works）/ 目的（Purpose）/ 注意事项（Caveats）"四段式文件头，函数命名统一为 PascalCase，libc 标准函数名保持不变。

---

## 十三、v0.9-dev：工具链与服务端能力（2026-09-20，工作区）

这一批是 **HEAD `105805d` 之后未提交的工作区改动**，主题是「把系统用起来」：补齐磁盘、开关机、网络三类工具，并把 libc 与 C Runtime 从"够用"推到"完整"。全部改动经 `make iso`（0 警告）与 QEMU 启动自检（8 套件全绿）验证，新增的 `scripts/verify_tools.py` 在 QEMU 里跑通 23 项工具检查。

### 13.1 磁盘工具系列

| 层 | 改动 |
| --- | --- |
| 内核 | 无（复用既有 `SYS_BLK_*` 与 `CAP_TYPE_PCI_DEV` 门控） |
| VFS 协议 | 新增 `VFS_OP_SYNC`（`vfs_req_sync_t`/`vfs_resp_sync_t`）：vfs_server 遍历已挂载卷，逐卷转发 `DRV_OP_SYNC`，统计成功/失败 |
| FS 驱动 | 新增 `DRV_OP_SYNC`（刷盘）、`DRV_OP_CTRL_CHECK`（**只读**一致性扫描）、`DRV_OP_CTRL_INFO`（卷详情）、`DRV_OP_CTRL_RAW_READ`（原始扇区读，`DRV_RAW_MAX = 1024`）；virtio 与 mem 两个驱动各自实现 |
| 管理员代理 | `user` 服务新增 `USER_OP_DISK_SYNC\|CHECK\|INFO\|RAW_READ`，与既有的 mount/unmount/format/fill 走同一条 OWNER/ADMIN 校验路径 |
| 客户端 | `libfs` 新增 `FsSync()` |
| Shell | `disk` 从"list/mount/unmount/format/fill"扩展为 `list/info/sync/check/read/mount/unmount/format/fill`；新增 `df`、`du`、`cp`、`touch`、`head`、`hexdump`、`tree`、`wc`（`cmd_disk.c` + `cmd_fs.c`） |

### 13.2 开关机工具

- 内核新增 **`SYS_HALT`（73）**：`cli` 后在 `hlt` 循环停住 CPU、**不动复位线**，与 `SYS_REBOOT`（复位）/`SYS_SHUTDOWN`（ACPI S5）区分；同样以 `ATOM_SYS_SHUTDOWN` 门控，`SYS_COUNT` 随之变为 74。init 的 P2 门控套件新增 `halt unauthorized -> ERR_NOCAP` 用例（P2 Gate 6/6）。
- Shell 新增 `power [status|sync|off|reboot|halt]`（含 `-f`/`-s`/`-n`）与别名 `poweroff`/`halt`/`restart`：**先刷盘再断电**，把"关机"从"直接掉电"变成有序操作。
- **manager 新增控制端口 `manager`**：`svc_req_t`/`svc_resp_t` 协议提供 `SVC_OP_LIST/STATUS/START/STOP/RESTART`，以"调用方是否持 `ATOM_SERVICE_MANAGE`"为门控；`STOP` 通过 `stop_requested` 抑制自动重启，并保护 `manager`/`init`。
- Shell 新增 `svc` 命令族，经 `user` 服务管理员代理（`USER_OP_SVC_*`）访问控制端口。

### 13.3 网络工具

- **TCP 主动连接**：`NET_OP_TCP_CONNECT`（17）落地，`ProtoTcpConnect()` 复用既有 `SYN_SENT` 状态机，含临时端口分配、RST 拒连识别、6 秒超时与失败复位；新增 `NET_OP_GET_IP`（18）与 `ProtoGetIp()`。
- Shell 新增 `ip`（查看/设置静态地址）、`netstat`、`udp`（bind/send/recv）、`dns`（手工构造 A 查询）、`http`（HTTP/1.0 GET）。
- `make run` / `make debug` 默认附带 **PCnet 网卡**（`-netdev user,id=n0 -device pcnet,netdev=n0`），网络开箱可用。

### 13.4 libc 与 C Runtime 完善

| 组件 | 新增/修复 |
| --- | --- |
| `libc/stdio_file.c` | `FILE` 缓冲层加固；新增 `sscanf`/`vsscanf`/`fscanf`/`perror`；后端负错误码到 `errno` 的正确映射；`ungetc` 对控制台可用；`r+` 写前重定位 |
| `libc/string.c` | `strnlen` `strndup` `strsep` `strlcpy` `strlcat` `memmem` `strcoll` `strxfrm` `strerror_r` |
| `libc/stdlib.c` | `div`/`ldiv`/`lldiv`、`strtod`/`strtof`/`strtold`/`atof`（含 hex float、inf/nan、ERANGE）、`mbstowcs`/`wcstombs` |
| `libc/stdio.c` | `fmt == NULL` 保护；结尾孤立 `%` 不再写入 NUL 字节；`getchar` 把 `ERR_NOCAP` 归一化为 EOF+`EACCES` |
| `runtime/malloc.c` | `MallocStats`/`MallocCheck`/`MallocUsableSize` 诊断三件套；`aligned_alloc`/`posix_memalign`（**唯一强定义**，与 `free`/`realloc` 兼容）；修复三处既有堆缺陷：size-class bin 会发出小于请求的块、就地 realloc 丢弃小余量导致块链空洞、`asize` 回绕 |
| `runtime/exit.c` | `atexit` 容量 32、满时返回非 0；新增 `__cxa_atexit`/`__cxa_finalize`；退出顺序 atexit(LIFO) → `__cxa_finalize` → `_fini` → `_exit`，带重入保护 |
| `runtime/init.c`/`errno.c`/`signal_user.c` | 初始化顺序与一次性保护；明确 errno 为进程全局（无 TLS）；`Signal()` 的 `SIG_ERR`/`EINVAL`/不可捕获语义、dispatcher 重入合并与重投 |
| `libfs/stdio_vfs.c` | **新增**：把 libc 的 `FILE` 后端接到 libfs（`.init_array` 构造函数注册），于是 `fopen("/Volumes/Disk/x","r")` 在所有链接了共享对象的程序里直接可用 |

### 13.5 验证

| 验证 | 结果 |
| --- | --- |
| `make iso -j4` | 通过，0 警告 / 0 错误 |
| QEMU 启动自检 | `init: ALL SELFTESTS PASSED`：经典 34/34、P2 Gate 6/6、KBD 1/1、P3 1/1、P4 2/2、P5 1/1 |
| `scripts/verify_tools.py`（新增） | 23/23 通过：磁盘 5 项、文件工具 10 项、电源/服务 4 项、网络 4 项，含主机侧 HTTP/UDP 往返与 `power halt` 落到内核的验证 |
| 宿主机单元测试（子代理各自在 /tmp 内） | libc 187 项断言、网络状态机 32 项、堆 20000 步随机 churn × 6 种子，全部通过 |

### 13.6 已知缺口（如实记录）

- `disk info/check/read` 只覆盖 virtio-blk 卷（`Disk`）；内存卷（`System`）会明确报告"没有控制面"而不是伪造数据。
- `ATOM_NET_BIND`/`ATOM_NET_CONNECT` 仍未接入 `net` 服务：任何 Ring 3 进程都能调用它的全部 opcode（见 [net_design.md](net_design.md) §八）。
- `power off` 只做「刷盘 → 断电」，不会逐个停止服务；完整的关机编排仍是未来工作。
- `svc` 的监控名单仍是 `perm/pkg/device_mgr/shell/user` 五个；`wm`/`gui`/`policy` 在服务表里标了可重启但未注册监控线程。

---

## 十四、v1.0-dev：用户态权限模型与 VFS 的补完（2026-09-20，工作区）

主题：把 [permission_model.md](permission_model.md) §十 路线图里的 **P3（上下文/频率/路径约束）** 与
**P4（持久化/审计）** 从"协议占位"做成"决策路径真正使用"，同时把 VFS 的对象模型与命名空间补齐。
全部改动都在用户态，**没有新增任何系统调用**。

### 14.1 权限引擎（perm）

| 能力 | 落地内容 |
| --- | --- |
| 授权生命周期 | `perm_grant_t` 增加 `expiry_ticks`（惰性过期 + `PERM_EV_EXPIRE` 审计）、`scope_hash`（作用域匹配，不匹配则不算命中并继续走角色链）、`source`（POWERBOX/DIRECT/POLICY） |
| 限时授权 | `PERM_OP_ANSWER` 支持 `ttl_ticks` 与 `scope_hash`，Powerbox 可以发"限时/限作用域"的授权 |
| 上下文感知 | 后台主体的默认拒绝**不再创建询问、不再推送面板**；未登记主体仍按前台处理（P1/P2 语义不变） |
| 频率阈值 | 每次判定计数；滚动窗口（1000 tick）内拒绝达 8 次即隔离 3000 tick，隔离期一律拒绝且不弹窗，可手动解除 |
| 审计 | 事件码覆盖 CHECK/ POWERBOX/ ANSWER/ GRANT/ REVOKE/ ROLE_SET/ CONTEXT/ QUARANTINE/ EXPIRE/ POLICY_SAVE/ POLICY_LOAD；支持按主体/裁决/时间/条数过滤 |
| 策略持久化 | 快照升级 v2（携带 expiry/scope/source，3728 B ≤ 上限），兼容读 v1；SAVE 可跳过并回收过期授权；LOAD 全有或全无 + 热更新 |
| 抹位修正 | **角色链现在逐位求值**：请求 READ\|WRITE 会得到 READ\|WRITE（此前会因为只看"主 atom"而只拿到 WRITE，READ 被静默丢弃） |

### 14.2 VFS

| 能力 | 落地内容 |
| --- | --- |
| fstat by handle | `VFS_OP_STAT_HANDLE` + libfs `FsStatHandle()`；stdio 后端的 `size()` 因此真正可用 |
| 截断 | `VFS_OP_TRUNCATE` + libfs `FsTruncate()`（缩小释放尾部、扩大零填充；mem 与 virtio 两个驱动都实现） |
| 书签生命周期 | 过期在 RESOLVE 时**强制**返回 `VFS_ERR_STALE`；新增 `VFS_OP_REFRESH_BOOKMARK` + libfs `FsRefreshBookmark()` |
| 命名空间 | MOVE 环检测、保留名/超长名/内嵌分隔符校验、删除后句柄与枚举器统一 `VFS_ERR_STALE`、内存卷条目与单文件配额 |
| 读权限收紧 | 读取要求句柄确实带 READ 位（写-only 句柄不能读回内容） |

### 14.3 网络原子门控

`net` 服务在分派前检查调用者能力：出站操作要 `ATOM_NET_CONNECT`，绑定/监听/接收要 `ATOM_NET_BIND`，
诊断类（MAC/地址/统计）保持开放。`user` 服务在 OWNER/ADMIN 登录时随关机能力一并签发这两个原子 —— 补齐了
[net_design.md](net_design.md) §八 记录的"已定义但无人使用"缺口。

### 14.4 shell 与工具

`perm` 命令族扩展为 `answer|query|revoke|audit|ctx|freq|save|load`，并新增 `permguard`
（隔离管理）。管理性操作要求 OWNER/ADMIN（经 `user` 服务核验），只读查询不设门槛。
策略文件默认 `/Volumes/Disk/perm.policy`，审计导出默认 `/Volumes/Disk/perm.audit`。

### 14.5 验证

| 验证 | 结果 |
| --- | --- |
| `make iso -j4` | 通过，0 警告 / 0 错误 |
| QEMU 启动自检 | **9 个套件全绿**：经典 34/34、**P6 10/10（新增）**、P1 10/10、P2 Gate 6/6、P2V 4/4、KBD 1/1、P3 1/1、P4 2/2、P5 1/1 → `init: ALL SELFTESTS PASSED` |
| 新增 P6 套件 | 授权 TTL 过期、作用域匹配、后台拒绝不弹窗、8 次拒绝隔离 + 手动解除、审计事件完整、策略 save→load（含"角色与规则未被破坏"断言）、fstat by handle、截断缩/扩零填充、书签过期 + 续期、移动环检测与保留名 |
| `scripts/verify_tools.py` | 32 项通过 / 0 失败 / 2 项环境性跳过，新增 `perm audit\|ctx\|freq\|save\|load\|permguard` 六项检查 |

### 14.6 过程中被测试逼出来的真实缺陷

| 缺陷 | 影响 | 处置 |
| --- | --- | --- |
| 抹位只看"主 atom" | 请求 READ\|WRITE 只拿到 WRITE，句柄无法读回自己写的文件 | 角色链改为逐位求值（本批修复） |
| init 把 3.6 KB 的审计响应放在栈上 | 启动栈只有 4 KiB → 用户态 #PF，init 被杀 | 测试与协议记录统一改静态缓冲；已在 P6 注释中记录该约束 |
| P6 的撤销范围过大 | 把 init 的全部授权撤掉，后续 VFS 用例全部被拒 | 撤销限定到具体资源 |
| P6 与其他套件的顺序耦合 | P1 的角色降级测试会让 init 永久变 GUEST；P6 遗留的待处理询问会被 P1 误判为"泄漏提示" | P6 提前到 P1 之前执行，并在结束时清空询问队列 |

---

## 十二、当前状态与后续方向

**当前 HEAD（`105805d`，2026-09-01）之后的工作区状态**：`user/services/gui/` 与 `gui_demo` 存在未提交的本地修改（窗口/合成器相关的持续打磨）。

| 方向 | 状态 | 参考 |
| --- | --- | --- |
| 像素 GUI 打磨（窗口管理细节、刷新率） | 进行中 | [gui_design.md](gui_design.md) |
| 网络栈完善（多连接、重传/超时、DHCP/DNS） | 规划 | [net_design.md](net_design.md) |
| 国际化扩展（更多输入法、更全字模） | 规划 | [i18n_design.md](i18n_design.md) |
| SMP 多核调度 | 规划 | [kernel_roadmap.md](kernel_roadmap.md) |
| 全量 PIE（`ET_DYN`） | 地址区间已预留 | `kernel/include/kernel/rng.h` |
| 服务在线热重启（serial/term/keyboard/vfs） | 规划 | `user/services/manager/manager.c` |
| POSIX 兼容层 | 未规划 | [faq.md](faq.md) |

> 本文只记录**已发生**的提交；设计意图与决策理由见 [kernel_roadmap.md](kernel_roadmap.md)、[requirements.md](requirements.md)、[microkernel_audit.md](microkernel_audit.md)。历轮测试的判定与证据见 [test_report.md](test_report.md)。

> 返回 [文档索引](README.md)
