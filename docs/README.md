# OpSys 项目文档

> 适用版本：OpSys v0.9-dev（HEAD 105805d + 工作区）　|　最后更新：2026-09-20
>
> 这里是 OpSys 全部文档的导航中心。不知道从哪读起？看下面的[阅读路线](#二阅读路线)。

OpSys 是一个从零实现的 x86_64 微内核操作系统：Ring 0 只保留调度、内存、IPC、能力四类机制，全部设备驱动与系统服务运行在 Ring 3 并通过同步 IPC 协作。本目录收录了它的**需求、架构、设计、接口参考、开发与测试手册**。

---

## 一、文档地图

### 1.1 入门与导航

| 文档 | 内容 | 适合谁 |
| --- | --- | --- |
| [../README.md](../README.md) | 项目主页：特性总览、架构速览、快速开始、命令速查 | 所有人（第一站） |
| [getting_started.md](getting_started.md) | 快速上手：依赖安装、构建、运行、调试、故障排查 | 想跑起来的人 |
| [faq.md](faq.md) | 常见问题：设计取舍、构建环境、使用、故障排查四类问答 | 遇到疑问的人 |
| [CHANGELOG.md](CHANGELOG.md) | 版本沿革：按提交整理的开发日志与里程碑 | 想了解演进史的人 |

### 1.2 架构与设计（必读）

| 文档 | 内容 |
| --- | --- |
| [architecture.md](architecture.md) | **系统架构总览**：分层、引导链、服务拓扑、IPC 模型、内存布局、能力机制、子系统索引 |
| [requirements.md](requirements.md) | 需求规格说明书：模块划分、技术选型、接口约定、测试策略、交付物 |
| [kernel_roadmap.md](kernel_roadmap.md) | 内核开发方向决策：性能/实用性/安全三维度评审、Ring 0/3 归属定案、已定案决策记录 |
| [microkernel_audit.md](microkernel_audit.md) | 微内核化审计报告：各子系统逐项结论、迁移候选、性能关键路径核对 |

### 1.3 安全与权限

| 文档 | 内容 |
| --- | --- |
| [permission_model.md](permission_model.md) | 基于属性的动态权限模型（ABAC）：身份/角色/原子/上下文/策略五层，能力生命周期，Powerbox，账户服务与退出保护，命令策略三层架构 |
| [permission_reference.md](permission_reference.md) | **权限引擎参考手册**：原子表、角色与默认规则、判定顺序与决策标志、全部 PERM_OP_* 协议、审计事件、策略快照格式、频率隔离、perm 命令行与排错决策树 |
| [ops_format.md](ops_format.md) | `.ops` 应用包格式与沙盒授权契约：二进制布局、manifest 规范、权限原子映射、内核门控、验收标准 |

### 1.4 子系统设计

| 文档 | 内容 |
| --- | --- |
| [vfs_design.md](vfs_design.md) | VFS：对象模型（Volume/Item/FileHandle/Enumerator）、书签、FSKit-lite 驱动协议、分阶段实施与决策记录 |
| [tui_design.md](tui_design.md) | TUI：分层架构、API 参考、协议定义、字体渲染、组件库，附录含 v0.4 窗口管理器（wm） |
| [gui_design.md](gui_design.md) | 像素 GUI：libgui 绘制库、gui 合成器协议、脏区合成、输入路由、事件隔离 |
| [net_design.md](net_design.md) | 网络：PCnet-Fast III 驱动、ARP/IPv4/ICMP/UDP/TCP 协议栈、net 服务 IPC 协议 |
| [i18n_design.md](i18n_design.md) | 国际化：UTF-8 编解码、宽字符 libc、CJK 字模、TUI/GUI 渲染、拼音输入法 |
| [runtime_design.md](runtime_design.md) | C Runtime：启动流程、malloc 设计（快路径/ASLR/就地扩展）、atexit、errno、信号 |
| [runtime_quick_ref.md](runtime_quick_ref.md) | Runtime 速查：常用 API、内存布局、DO/DON'T、性能建议 |

### 1.5 接口参考

| 文档 | 内容 |
| --- | --- |
| [syscall_reference.md](syscall_reference.md) | 系统调用参考：编号表（单一事实源）、调用约定、逐项参数与返回值、门控矩阵、错误码 |
| [service_reference.md](service_reference.md) | 服务与 IPC 协议参考：服务清单、启动拓扑、端口、opcode、结构体布局、关键时序图 |
| [shell_reference.md](shell_reference.md) | Shell 命令参考：逐条命令的语法、示例、权限要求与输出说明 |

### 1.6 开发与测试

| 文档 | 内容 |
| --- | --- |
| [developer_guide.md](developer_guide.md) | 开发者指南：代码风格、许可与 SPDX、微内核纪律、四类任务手册（新增服务/syscall/命令/库）、调试手段、常见陷阱 |
| [testing_guide.md](testing_guide.md) | 测试与验证指南：init 自检 8 套件清单、宿主机脚本矩阵、双通道观测模型、如何新增测试 |
| [shell_reference.md](shell_reference.md) §3.9 | v0.9 工具族：磁盘维护、开关机、服务监管、网络工具（含管理员代理链说明） |
| [test_report.md](test_report.md) | 历轮测试报告：三轮高强度测试 + 各版本迭代的回归记录与证据 |

---

## 二、阅读路线

### 路线 A：我想先把系统跑起来（约 30 分钟）

```
README.md  →  getting_started.md  →  faq.md（遇到问题时查阅）
   ↑
   └── 只想看命令：shell_reference.md
```

1. [README.md](../README.md) §四「快速开始」：装依赖 → `make iso` → `qemu-img create disk.img 8M` → `make run`。
2. 看到 `init: ALL SELFTESTS PASSED` 与登录提示后，用默认账户 `admin/admin` 登录。
3. 试 `help` / `ps` / `ls /Volumes` / `gui`，对照 [shell_reference.md](shell_reference.md)。

### 路线 B：我想理解这个系统怎么设计的（约 3 小时）

```
README.md §一～§三
  → architecture.md（全局图景）
      → kernel_roadmap.md（为什么这么切分 Ring 0/3）
      → microkernel_audit.md（现状审计）
      → permission_model.md（权限与身份）
      → vfs_design.md（文件系统对象模型）
      → tui_design.md + gui_design.md + net_design.md + i18n_design.md（子系统）
```

### 路线 C：我要改代码（先读这两篇再动手）

```
developer_guide.md（风格 + 任务手册 + 陷阱）
  → syscall_reference.md / service_reference.md（改接口时对照）
  → permission_reference.md（改权限判定/原子/策略时对照）
  → testing_guide.md（怎么验证没改坏）
  → docs/test_report.md（看历次回归的判定标准）
```

### 路线 D：我在排查一个具体问题

| 症状 | 先看 |
| --- | --- |
| 构建失败 / 缺工具 | [getting_started.md](getting_started.md) §故障排查、[faq.md](faq.md) B 类 |
| 启动卡住 / SELFTEST FAILURE | [testing_guide.md](testing_guide.md) §自检套件、[faq.md](faq.md) D 类 |
| 命令不能用 / 权限被拒 | [permission_model.md](permission_model.md)、[shell_reference.md](shell_reference.md) |
| 屏幕/图形不对 | [gui_design.md](gui_design.md)、[tui_design.md](tui_design.md) |
| 想加一个系统调用 | [developer_guide.md](developer_guide.md) §新增系统调用、[syscall_reference.md](syscall_reference.md) |

---

## 三、文档状态一览

| 文档 | 状态 | 说明 |
| --- | --- | --- |
| README.md | ✅ 已对齐当前代码 | 覆盖至 v0.8-dev 开发线 |
| getting_started.md | ✅ 新增 | 依据 Makefile / scripts 实际内容编写 |
| architecture.md | ✅ 新增 | 全局架构总览 |
| syscall_reference.md | ✅ 新增 | 以 `syscall_numbers.h` 与 `syscall.c` 为准 |
| service_reference.md | ✅ 新增 | 以各服务协议头文件为准 |
| shell_reference.md | ✅ 新增 | 以 `shell.c` 注册表为准 |
| gui_design.md / net_design.md / i18n_design.md | ✅ 新增 | 覆盖 v0.8-dev 三大方向 |
| developer_guide.md / testing_guide.md / faq.md | ✅ 新增 | 开发与运维手册 |
| CHANGELOG.md | ✅ 新增 | 依据 git 提交历史 |
| requirements.md | ⚠️ 设计基线 | 项目初期的需求规格，实现已超出其范围（见下文说明） |
| kernel_roadmap.md | ⚠️ 阶段性基线 | 决策记录仍然有效，部分"现状基线"数字为当时快照 |
| microkernel_audit.md | ⚠️ 阶段性快照 | 审计结论有效，迁移候选状态请以当前代码为准 |
| vfs_design.md / tui_design.md / permission_model.md | ✅ 设计 + 落地 | 含 Phase 落地记录与决策定案，正文按设计意图阅读 |
| runtime_design.md / runtime_quick_ref.md | ✅ 基本对齐 | 少量 `v0.1 限制` 章节为历史记录 |
| ops_format.md | ✅ 冻结契约 | Phase A 契约，改动需同步升级版本号 |
| test_report.md | ✅ 追加式记录 | 每轮迭代追加，越靠后越新 |

> **阅读旧文档的注意事项**：`requirements.md`、`kernel_roadmap.md`、`microkernel_audit.md` 是**设计过程文档**，记录的是做出决策时的思考与当时的现状快照。其中的"规划/未实现"条目可能已经在后续版本落地（例如窗口管理器、GUI、网络）。判断某项功能的当前状态，请以 **代码** 与 **README.md / 本目录的参考类文档** 为准。

---

## 四、术语表

| 术语 | 英文 | 含义 |
| --- | --- | --- |
| 微内核 | Microkernel | 只在内核保留机制（调度/内存/IPC/能力），策略与驱动放用户态 |
| 主体 | Subject / `subject_id` | 内核在进程创建时签发、在 IPC 交付时填充的不可伪造身份（u64） |
| 能力 | Capability | 内核能力表中的一条记录：类型 + 权限位 + 对象 ID + 生命周期字段 |
| 原子权限 | Atom | 权限模型中的语义化权限点（如 `ATOM_DATA_DOCS_READ`），是授权的语义索引 |
| 角色 | Role | OWNER / ADMIN / STANDARD / CHILD / GUEST / AUDITOR |
| Powerbox | Powerbox | 默认拒绝下由系统 UI 向用户发起的显式授权询问流程 |
| 书签 | Security-Scoped Bookmark | 不透明 blob，客户端用它访问资源而无需知道路径 |
| 机制/策略分离 | Mechanism / Policy | 内核提供机制，用户态服务决定策略 |
| 决策下沉 | Decision Sinking | 授权判定放进内核能力表查找，避免 syscall 路径回调用户态 |
| 抹位 | Rights Masking | 授权时把申请权限与策略允许权限取交集，得到只减不增的句柄权限 |
| blob | Blob | 嵌入内核镜像的用户态服务 ELF 映像，供 `BlobGet` + `ProcessCreate` 拉起进程 |
| FSKit-lite | FSKit-lite | 用户态文件系统驱动协议：驱动主动向 vfs 发起 MOUNT 握手 |
| FS 驱动 | FS Driver | 实现卷存储的用户态进程（内存卷 / virtio-blk 磁盘卷） |
| 合成器 | Compositor | 管理多个窗口像素缓冲并按 Z 序合成到帧缓冲的服务（`gui`） |
| 脏区 | Dirty Rect | 只重绘发生变化的矩形区域以降低刷新开销 |
| 双通道观测 | Dual-channel Observation | 自动化测试同时观察串口（调试日志）与 VGA（shell 屏幕） |
| 自检 fail-fast | Boot Selftest Fail-fast | init 的启动自检任一套件失败即停机，避免"带病启动" |

---

## 五、文档贡献约定

新增或修改文档时请遵守：

1. **语言与风格**：简体中文为主，术语首次出现给出英文；标题从 `#` 起逐级使用 `##` `###`；多用表格与带语言标注的代码块。
2. **文件头**：注明适用版本与最后更新日期（形如 `> 适用版本：OpSys v0.8-dev（git HEAD 105805d）　|　最后更新：YYYY-MM-DD`）。
3. **事实优先**：接口、常量、结构体布局必须与代码一致；引用具体文件（必要时带行号），**不要凭记忆描述**。
4. **不重复**：概述放 README，细节放专题文档，参考类信息放 `*_reference.md`，用链接互相引用而不是复制粘贴。
5. **许可**：`docs/` 目录采用 [CC BY 4.0](LICENSE)，新增文档默认适用。
6. **同步更新**：改动接口/流程时同步更新对应文档，并在 `CHANGELOG.md` 追加一行；新增文档请登记到本索引的[文档地图](#一文档地图)与[状态一览](#三文档状态一览)。

---

## 六、相关链接

- 项目主页：[../README.md](../README.md)
- 构建系统：[../Makefile](../Makefile)、[../scripts/build.sh](../scripts/build.sh)、[../scripts/run.sh](../scripts/run.sh)
- 代码风格：[../.clang-format](../.clang-format)
- 引导配置：[../boot/grub.cfg](../boot/grub.cfg)
- 链接脚本：[../scripts/user.ld](../scripts/user.ld)
- 文档许可：[LICENSE](LICENSE)
