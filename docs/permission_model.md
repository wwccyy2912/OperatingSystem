# OpSys 基于属性的动态权限模型（Attribute-Based Dynamic Permission Model）

> 版本：v0.4（P0 + P1 + P2 已落地）
> 日期：2026-08-13
> 状态：**P0 地基 ✅（31/31 测试通过）** —— **P1 perm-engine ✅（回归 31/31 + P1 10/10）** —— **P2 接入 ✅（回归 31/31 + P1 10/10 + P2 Gate 3/3 + P2 VFS 4/4）**；P3（上下文/频率/路径约束）、P4（持久化/审计/开发者/恢复模式）待启动（P3/P4 协议接口已预留，见 §十）；含原设计五层模型 + 架构修正补充（补充一 ~ 补充八）
> 关联：docs/vfs_design.md（书签 + Powerbox）、docs/kernel_roadmap.md、kernel/cap/、kernel/ipc/、user/services/perm/

---

## 〇、设计摘要

传统 Unix 的 UID/GID 模型过于粗放（普通用户 or 万能 Root）；Android 的 AID
模型隔离了应用，但对**自然人用户本身**的权限控制依然薄弱（家长控制、临时授权、
数据共享范围）。

OpSys 采用**「基于属性的动态权限模型」**，彻底抛弃 Root 账户，实现真正意义的
细粒度控制。模型分五层：**身份层、角色层、权限原子层、上下文层、策略层**。

**本文档的目标**：把概念设计落成可实现的接口定义与分阶段计划，并与 OpSys 现有
代码库约束对齐。**补充一 ~ 补充八 为架构评审结论**——其中「身份层内核化」与
「禁止内核→用户态同步策略查询」是两条不可妥协的红线，其余为落地细则。

---

## 一、现有代码库约束（设计必须遵守的事实）

| 约束 | 出处 | 对设计的影响 |
|------|------|--------------|
| IPC 不暴露发送者身份 | `kernel/ipc/ipc.c:74,332,408`（`sender_tid` 仅内部唤醒用）、`sys_ipc_recv`（syscall.c:206-228，无 sender 出参） | **身份层必须内核化（补充一）**，否则任何 subject 校验都可伪造 |
| IPC 是完全开放总线 | `port_entry_t` 记录 `owner_tid/owner_pid`（ipc.h:33-39）但从不检查；任意进程可 send/recv/call 任意端口（ipc.c:307-452） | 端口级 ACL 属后续增强；当前靠「消息内身份 + 用户态服务校验」 |
| 能力表每进程 1024 槽 | `cap_table_t`（cap.h:39-43）、`MAX_CAPS=1024` | 能力缓存（补充二）会消耗槽位；需预留 + 支持按 atom 批量撤销 |
| 能力「可过期」基础已有 | `cap_entry_t`（cap.h:29-36）为静态结构；`cap_lookup`（cap.c:291-297）是唯一使用点检查 | **能力生命周期编码（补充三）只需扩展结构体 + 惰性检查** |
| revoke **不级联** | `cap_revoke`（cap.c:222-259）只清本槽、bump 代次；已授予副本不受影响 | 设计「一键撤回全部相关 Capability」需要 **cap_revoke_by_atom**（补充三） |
| cap_create 无任何授权检查 | `sys_cap_create`（syscall.c:132-150）任意进程可自铸任意类型 | 新模型下自铸被 perm-engine 接管：应用不再直接 cap_create |
| 敏感 syscall 目前只有 4 个被能力门控 | map_memory（syscall.c:290-294, MEM+RIGHT_WRITE）、bind/unbind_irq（840-867, IRQ+RIGHT_READ+obj_id 匹配）、io_read8/write8（879-922, IO_PORT 范围） | 新原子权限必须逐一映射到能力门控路径；`sys_unmap_memory`（350-381）至今无检查 |
| 凭证 `cred_t` 是装饰 | `kernel/include/kernel/cred.h:14-16` 声明「检查逻辑在各子系统」，但无任何子系统读取 | 直接废弃 cred 路线，统一走 SubjectID + 原子权限 |
| 唯一用户态授权点是 VFS 书签 | `perm_check`（vfs_server.c:207-231）仅两处调用：create_bookmark（1024）、resolve_bookmark（1103）；普通 open/read/write 完全绕过（543-832） | Phase 2 要把 perm-engine 接入全部文件操作 + 敏感 syscall |
| 现有 Powerbox 基础设施可复用 | `perm-manager.c`（授权表 64 行、查询队列 16 行、CHECK/ANSWER/QUERY/REVOKE/GRANT/UI_SHOW 六操作码，perm.h:55-62）、term 的 `perm.ui` 渲染（term.c:514-624） | perm-engine 是 perm-manager 的进化版，而非重写 |
| 身份是客户端自报哈希 | `SHELL_APP_HASH 0x5E11E5`（shell.c:1124）；`app_id_hash` 随消息体传递（fs.c:349），无人验证 | **Phase 0 必修**：替换为内核签发的 SubjectID |
| 无持久化存储 | `fs_mem_driver` 为内存卷（32 MiB RW，重启即失） | Policy.db 现阶段易失；持久化随块设备驱动后置（补充六） |
| 无窗口管理器/焦点概念 | — | 「前台专用」上下文需要前台状态来源（补充五） |
| 类型风格 | `u8/u16/u32/u64/i32`、`_t` 后缀、`模块_动词_名词` | 本文档所有定义遵守 |

---

## 二、五层模型总览（原设计）

```
┌─────────────────────────────────────────────────────────────┐
│  策略层  Policy   perm-engine 规则链：默认 / 用户覆盖 / 环境    │
│  ┌─────────────────────────────────────────────────────────┐ │
│  │ 上下文层 Context  Once / Timed / Foreground / Path / Freq │ │
│  │  ┌─────────────────────────────────────────────────────┐ │ │
│  │  │ 权限原子层 Atom  <域>.<子域>:<操作>  如 hw.camera.*   │ │ │
│  │  │  ┌─────────────────────────────────────────────────┐ │ │ │
│  │  │  │ 角色层 Role   Owner/Admin/Standard/Child/...     │ │ │ │
│  │  │  │  ┌─────────────────────────────────────────────┐ │ │ │ │
│  │  │  │  │ 身份层 Identity  UserID(UUID)/Service/App     │ │ │ │ │
│  │  │  │  └─────────────────────────────────────────────┘ │ │ │ │
│  │  │  └─────────────────────────────────────────────────┘ │ │ │
│  │  └─────────────────────────────────────────────────────┘ │ │
│  └─────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────┘
```

### 2.1 身份层（Identity & Subject）—— 取代 UID

- 不使用数字 UID，使用**全局唯一 UUID** 作为用户标识（UserID）；系统保留特殊
  `System` 标识（仅用于内核固件更新，无操作文件能力）。
- **自然人用户（Human User）**：登录密码或生物特征实体。
- **服务主体（Service Principal）**：如 `vfs_server`、`audio_server`，无「登录」
  概念，仅持有启动时由 `servmgr` 授予的特定权限。
- **应用主体（App Subject）**：每个 `.ops` 应用实例化后拥有唯一 SubjectID
  （如 `app.com.editor.session_uuid`）。
- **人格（Persona）**：一个自然人可拥有多个人格（工作模式 / 儿童模式），切换
  人格时权限策略自动重载。

### 2.2 角色层（Roles）—— 取代「是否为 Root/管理员」

取消万能 Root，引入可叠加的细粒度管理角色：

| 角色 | 典型权限（非全部） | 边界 |
|------|-------------------|------|
| **Owner** | 修改系统时间、安装系统更新（签名验证）、创建其他用户 | 无法读取其他用户的加密数据 |
| **Admin** | 安装/卸载全局应用、修改防火墙策略 | 无法绕过沙盒读取用户文档 |
| **Standard** | 日常使用，安装应用到自己的容器 | 默认角色 |
| **Child** | 无法修改系统设置；购买/权限请求需 Owner 批准 | 时间限额、内容过滤强制启用 |
| **Guest** | 无持久化存储，重启后数据清零 | 临时使用 |
| **Auditor** | 仅查看系统日志、访问审计跟踪 | 无修改权 |

### 2.3 权限原子层（Atomic Permissions）

格式 `<域>.<子域>:<操作>`，粒度极细（节选）：

- **系统与硬件**：`sys.shutdown`、`sys.set_timezone`、`hw.camera.capture`、
  `hw.microphone.record`、`hw.gpu.high_performance`、`hw.location.coarse` /
  `hw.location.precise`
- **数据与文件**：`data.user.documents.read`、`data.user.downloads.write`、
  `data.app.container.read`、`data.system.logs.read`、`bookmark.resolve`
- **网络**：`net.inet.bind`（<1024 需额外授权）、`net.inet.connect`、
  `net.wifi.scan`、`net.wifi.set`
- **管理**：`pkg.install`、`pkg.update.system`、`service.manage`

### 2.4 上下文绑定层（Context Binding）

| 绑定类型 | 示例 | 效果 |
|---------|------|------|
| Foreground Only | `hw.camera.capture` 仅限前台 | 切后台自动回收（类 Android） |
| Once | `bookmark.resolve` | 单次有效，下次需重新授权 |
| Timed | `net.inet.connect` | 15 分钟有效 |
| Path-bound | `data.user.documents.write` 限 `/Users/alice/Work/` | 无法写其他目录 |
| Frequency-bound | `hw.location.precise` | 1 小时最多 10 次 |
| Trust-level | 官方签名 Trust=High 自动授；第三方 Trust=Low 弹窗 | 依据签名等级 |

### 2.5 策略层（PDP / PEP）

- 决策交给 **`perm-engine`**（用户态服务），维护**规则链**：全局默认策略
  （如「所有应用默认禁止访问通讯录」）+ 用户自定义策略（如「微信可用位置，仅
  使用期间」）+ 动态环境策略（如「电量 <10% 拒绝 hw.gpu.high_performance」）。
- **决策缓存**：授予 Capability 时直接编码「生命周期 + 作用域」，如
  `expiry = now + 600`，内核超时自动失效，无需再问 perm-engine。
- **持久化**：`/Users/<user>/Library/Security/Policy.db`（SQLite/JSON），记录
  `SubjectID, Atom, Scope, GrantedBy, Timestamp, Revoked`。

### 2.6 与 VFS / IPC 的结合

1. 文件访问：应用请求打开文件 → `vfs_server` 调 `perm-engine.check(subject,
   atom="bookmark.resolve", context={url})`；若只读授权，`vfs_server` 返回的
   FileHandle Capability 自动抹去 `Write` 位。
2. 敏感系统调用：App 调 `sys_set_time` → 内核询问 perm-engine 是否有
   `sys.set_time`，无则直接返回 `EPERM`。

### 2.7 Root 的消除

- 无全局 Root 账户；系统维护走恢复模式（物理按键 + 加密签名 / TPM）。
- 无 `sudo`；提权必须由 perm-engine 弹窗请求 Owner 生物识别确认。
- 开发者模式创建 `Developer` 角色（`sys.debug`、`cap.grant.self`），控制台红色
  水印 + 加密审计日志。

### 2.8 UI 聚合

原子权限聚合成人类可读标签：
- `hw.camera.capture + hw.microphone.record + foreground` →
  「此应用想要使用**相机和麦克风**（仅前台）」
- 权限中心以「服务」为维度（相机/位置/照片/蓝牙/本地网络）而非文件路径；
  一键撤回 → 该应用所有相关 Capability 立即被内核作废。

---

## 三、补充一（红线）：身份层必须内核化

**现状问题**：当前所有身份凭据（`app_id_hash`）是客户端消息内自报的 u32，
内核 IPC 不向接收方暴露发送者身份，任何进程可伪造任意身份。

**结论**：SubjectID 必须由内核签发、由内核在 IPC 交付时填充，用户态不可伪造。
这是整个模型的承重墙——不修它，角色/原子/上下文/策略全都是在沙地上盖楼。

**设计**：

```
subject_id_t = u64   /* 内核全局唯一，永不重用 */

process_t 增加：subject_id            /* 进程所属主体 */
            persona_id                /* 人格（自然人可用多个） */

sys_ipc_recv 增加可选出参：
    u64 *sender_subject   /* 内核从 pending msg 的 sender 线程反查填充 */

ipc_pending_msg_t 增加：sender_subject_id   /* 入队时由内核填，用户不可改 */
```

- 引导：`System` 主体（subject 0）由内核创建；init 进程领 subject 1；
  `servmgr` 为每个服务进程按服务名注册 Service Principal；应用实例化时分配
  App Subject（uuid）。
- **现有 `app_id_hash` 兼容过渡**：Phase 0 内核同时把 `sender_subject_id`
  放入消息头供 perm-engine 读取，`app_id_hash` 字段废除或降级为调试标签。

**验收标准**：vfs_server 能通过 `ipc_recv` 拿到不可伪造的发送者 SubjectID；
两个恶意进程互相冒充 `SHELL_APP_HASH` 的尝试在 perm-engine 侧被拒绝。

---

## 四、补充二（红线）：禁止内核 → 用户态同步策略查询

**现状问题**：原设计 2.6-2 写「内核系统调用处理函数生成请求发送给 perm-engine
询问」。这在微内核中是**死锁红线**：

1. syscall 处理器常在持有自旋锁/内核状态时运行，阻塞等待用户态 IPC 回复；
2. perm-engine 自己也是用户态进程，它执行时必然调用内核服务（内存映射、IPC
   收发……），若内核正被第一步的锁占住 → 相互等待 → 死锁；
3. 即使无锁，每次敏感 syscall 一次 IPC 往返的延迟也不可接受。

**结论**：**决策下沉，内核只做本地查表**。把「perm-engine 询问」从 syscall
路径上彻底移除，改为：

```
授予路径（异步，可阻塞）：
  主体/用户 → perm-engine 弹窗/策略判定
    → perm-engine 调用 cap_grant / cap_create 把「决策结果」编码进能力
      （atom_id + subject + scope + expiry + quota）

使用路径（同步，不可阻塞）：
  应用调 sys_set_time
    → syscall 处理器 cap_lookup(subject, atom_id=sys.set_time, scope)
    → 命中且未过期 → 放行；否则 EPERM（=现有 ERR_NOCAP 语义）
  无任何 IPC 往返，无锁内阻塞。
```

- 这正是原设计「决策缓存」段的正确形态，本文档将其提升为**唯一允许的形态**。
- 撤权路径：`perm-engine` 调新的 `cap_revoke_by_atom`（补充三）批量作废。

**验收标准**：`sys_set_time` 未授权返回 `EPERM` 且耗时 < 1µs 级（纯内核查表，
无 IPC）；内核代码中不存在任何「syscall → 用户态服务 → 回内核」的调用链。

---

## 五、补充三：能力生命周期与作用域编码

`cap_entry_t`（cap.h:29-36）扩展：

```c
typedef struct {
    cap_t       handle;
    cap_type_t  type;          /* 保留现有类型枚举 */
    rights_t    rights;        /* RIGHT_* 位，使用点仍查位 */
    u64         obj_id;        /* 对象/范围编码（见 scope 说明） */
    u64         obj_ptr;
    u32         ref_count;
    /* ---- 新增 ---- */
    u16         atom_id;       /* 权限原子枚举（补充四） */
    subject_id_t subject;      /* 持有者之外的签发对象（默认=表所有者） */
    u64         expiry_ticks;  /* 0 = 永久；超时后 cap_lookup 视同不存在 */
    u32         quota;         /* 0 = 无限；每次成功使用后由 syscall 递减 */
    u64         scope_hash;    /* 可选：bookmark id / 路径哈希 / 端口号 */
} cap_entry_t;
```

**惰性过期检查**（无需内核定时扫描）：

- `cap_lookup` 入口加：`if (e->expiry_ticks && now >= e->expiry_ticks)` →
  视同 `NULL`（可选：置 `type=NONE` 回收槽位）。复用现有代次计数器机制，
  不引入新锁。
- `quota` 递减点：每个门控 syscall 成功路径调用 `cap_consume(cap)`。

**撤权传播（新增内核 API）**：

```c
/* perm-engine 专用：按 (subject, atom) 批量作废某主体持有的能力 */
int cap_revoke_by_atom(subject_id_t subj, u16 atom_id, u64 scope_hash);
```

- 遍历目标进程能力表，匹配 `subject + atom_id (+scope)` 的条目全部
  `cap_revoke` 语义作废（bump 代次）。
- 这解决现有 revoke **不级联**的语义缺口（cap.c:222-259 只清本槽），满足原
  设计「一键撤回 → 相关 Capability 立即作废」。
- 能力表 1024 槽上限：原子能力缓存是长期驻留资源，需评估扩容
  （`MAX_CAPS` 静态池约 40MB/进程表，见 cap.c:71-74），或改为按需懒创建。

---

## 六、补充四：权限原子的表示与映射

**字符串 `<域>.<子域>:<操作>` 只是策略层的 DSL**；跨 IPC / 内核必须编译为
整数，否则每次检查都要字符串比较、且内核无法静态校验。

```c
typedef enum {
    ATOM_NONE = 0,
    /* 系统与硬件 */
    ATOM_SYS_SHUTDOWN, ATOM_SYS_SET_TIME, ATOM_SYS_SET_TIMEZONE,
    ATOM_HW_CAMERA_CAPTURE, ATOM_HW_MIC_RECORD,
    ATOM_HW_GPU_HIGH_PERF, ATOM_HW_LOC_COARSE, ATOM_HW_LOC_PRECISE,
    /* 数据与文件 */
    ATOM_DATA_DOCS_READ, ATOM_DATA_DOCS_WRITE, ATOM_DATA_DL_WRITE,
    ATOM_DATA_APP_CONTAINER_READ, ATOM_DATA_SYS_LOGS_READ,
    ATOM_BOOKMARK_RESOLVE,
    /* 网络 */
    ATOM_NET_BIND, ATOM_NET_CONNECT, ATOM_NET_WIFI_SCAN, ATOM_NET_WIFI_SET,
    /* 管理 */
    ATOM_PKG_INSTALL, ATOM_PKG_UPDATE_SYS, ATOM_SERVICE_MANAGE,
    /* 开发者 */
    ATOM_SYS_DEBUG, ATOM_CAP_GRANT_SELF,
    ATOM_MAX
} atom_id_t;
```

- 字符串 ↔ 枚举映射表由 perm-engine 维护（含名称表供审计/UI 打印）。
- **atom → 底层权利位的映射**：一个 atom 映射到一个或多个 `RIGHT_*` 位
  （如 `data.user.documents.write` → `RIGHT_WRITE`；`bookmark.resolve` →
  `RIGHT_READ`）。内核门控点继续检查现有 `cap_lookup(rights)`，atom 是语义层
  索引、rights 是执行层位掩码，两层都检查。
- 现有 4 个门控路径（MEM/IRQ/IO_PORT）保留：它们是「能力持有即用」的硬件
  访问模型，与新原子模型并存——串口/键盘服务持有 IO_PORT 能力即属此类，
  Phase 2 再决定是否迁移到原子层。

---

## 七、补充五：上下文绑定的实现归属

| 绑定 | 实现位置 | 说明 |
|------|---------|------|
| **Once** | perm-engine 维护状态；或 `quota=1` 的能力 | 每次 check 后递减/撤销 |
| **Timed** | **内核能力 `expiry_ticks` 兜底** + perm-engine 重评估 | 能力超时内核自动作废，无需回问 |
| **Frequency-bound** | perm-engine（唯一合理位置） | 计数在用户态策略层：每次 check 命中计数；内核能力只表达「当前窗口内允许」 |
| **Foreground Only** | 需要「前台」事实来源（窗口管理器/焦点服务） | 当前无此概念；先用「短期 Timed + 后台事件触发 revoke」近似，Phase 3 落地真前台 |
| **Path-bound** | **绑定 bookmark id（scope_hash）而非路径正则** | 与 vfs_design 书签体系一致；正则留在策略层做预过滤，内核只比 64 位哈希 |
| **Trust-level** | 应用签名验证（blob 元数据 + 签名链） | 当前无签名体系，Phase 4 引入；之前 Trust 统一 Low（弹窗） |

---

## 八、补充六：持久化与存储约束

- 现状 `fs_mem_driver` 为内存卷（32 MiB RW，重启即失），**Policy.db 现阶段
  无法真正持久化**。
- 分阶段策略：
  - Phase 0-2：策略存 perm-engine 内存 + 提供 `export/import`（serial/脚本），
    验收演示用；重启后恢复默认策略（可接受）。
  - 块设备驱动（virtio-blk）+ 持久卷就绪后：`/Users/<user>/Library/Security/
    Policy.db` 落盘；审计日志（加密）同卷。
- 角色/人格切换 = 策略集热重载（perm-engine 读新策略 → 对受影响主体执行
  `cap_revoke_by_atom` → 重新签发），无需重启服务。

---

## 九、补充七：端到端流程（含身份验证）

**场景：App `com.editor`（前台，Persona=alice.work）请求打开
`/Users/alice/Work/report.md`（可写），应用在安装时已声明需要
`data.user.documents.write`（Timed 30min, Path=/Users/alice/Work/）**
```
1. 安装期：pkg.install 流程中 perm-engine 审查声明 →
   生成策略记录 {subject=app.com.editor.uuid, atom=DATA_DOCS_WRITE,
   scope=bookmark(/Users/alice/Work/), context=Timed(30min), by=alice}
   → 落 Policy.db（内存态）
2. 打开请求：editor → vfs_server ipc_call(OPEN_ITEM, {path})
   → 内核在消息头填 sender_subject_id = app.com.editor.uuid
3. vfs_server → perm-engine ipc_call(check{subject, atom=DATA_DOCS_WRITE,
   scope=bookmark_hash, context={now, foreground=true}})
4. perm-engine 评估规则链：
   - 全局默认：DATA_DOCS_WRITE 默认拒绝 ✓
   - 用户覆盖：alice 已批准（安装期）✓
   - 环境策略：电量 65% > 10%，不触发 GPU 类拒绝 ✓
   → 命中 → 签发能力：cap_create(atom=DATA_DOCS_WRITE, rights=READ|WRITE,
     scope=bookmark_hash, expiry=now+30min, quota=0) 写入 editor 能力表
   → 返回 check=OK
5. vfs_server 创建 FileHandle（抹 Write 位逻辑：若 atom 只含 READ，
   handle->access 不含 WRITE）→ 返回 editor
6. editor 读写：vfs_server 每次按 handle 上的 access 位放行（现有
   do_read/do_write 检查，vfs_server.c:765-768/811-814）
7. 30 分钟后 / 切后台 / alice 在权限中心撤回：
   - 超时：内核 cap_lookup 惰性过期，下一次敏感 syscall EPERM；
     vfs_server 侧对已开 handle 按 scope 重新 check → 拒绝
   - 撤回：perm-engine → cap_revoke_by_atom(subject, DATA_DOCS_WRITE,
     scope) → editor 持有的相关能力全部作废，已打开 handle 下次操作失败
8. 审计：每一步（check/签发/撤回）写 {subject, atom, scope, granted_by,
   timestamp}，加密落审计卷
```

**敏感 syscall 示例（sys.set_time）**：Admin 在设置里授权「终端可改时间，
15 分钟」→ perm-engine 给终端签发 `ATOM_SYS_SET_TIME, expiry=now+15min` →
终端调 `sys_set_time` → 内核 `cap_lookup(subject, ATOM_SYS_SET_TIME)` 命中 →
放行；15 分钟后内核自动作废 → `EPERM`，全程零 IPC 往返。

---

## 九·五、用户账户服务与 ROLE_SET 管理面（2026-08 落地）

### 9.5.1 user 账户服务（user/services/user/main.c）

- **职责**：自然人账户生命周期（LOGIN/LOGOUT/PASSWD/USERADD/USERDEL/USERS/
  WHOAMI/VERIFY/STOP）。端口名 `user`。
- **身份绑定**：登录时把内核签发的 `caller_subject`（ipc_recv_from，不可伪造）
  绑定到账户表项；`whoami` 按调用方 subject 反查。
- **角色同步**：登录成功后把账户角色写入 perm-engine 的 `s_roles`（ROLE_SET），
  使「自然人角色」实时成为「进程角色」，VFS/能力门控按此裁决。
- **启动自举**：账户表为空时创建 `admin/admin`（OWNER），并提示改密
  （`passwd`）。
- **密码散列**：FNV-1a-64 + 每账户随机盐（完整性保护，非加密存储；文档明确
  为演示级）。

### 9.5.2 ROLE_SET 的管理面门控（perm-manager.c do_role_set）

ROLE_SET（热切换角色）是管理面操作。门控条件（二选一）：

1. **角色管理面**：调用方自身角色为 OWNER/ADMIN（`role_is_management`）——
   经典管理面，init 启动时被 seed 为 OWNER，P1 回归测试依赖此路径；
2. **user 账户服务**：调用方能力表持有 `ATOM_SERVICE_MANAGE` **且**内核
   `SYS_PROC_INFO_BY_SUBJECT` 返回的进程名为 `user`（`caller_is_user_service`）。
   user 服务通过 blob-identity 种子获得该原子（process_desc.c），因此它能
   以 STANDARD 角色身份替登录用户同步角色。

注意：仅凭 `ATOM_SERVICE_MANAGE` 原子**不足以**放行——init 也持有该原子
（它 spawn 全部服务），若 P1 测试把 init 降级为 GUEST 后它仍能
ROLE_SET 自我提权，则违反「管理面不可自升」约束（P1 test 8）。因此原子
必须与内核签发的进程名绑定，把授权限定在真正的 user 服务上。

其余调用方一律 `ERR_DENIED`。授权依据是内核在 `ipc_recv_from` 交付的
不可伪造调用方 subject + `proc_info_by_subject` 的不可伪造进程名。

### 9.5.3 系统程序退出保护（stop 命令）

- shell `stop <svc>`：TUI 确认框 → 当前账户 whoami → 掩码密码输入 →
  `USER_OP_VERIFY`（只验密码不绑定）→ `USER_OP_STOP`。
- user 服务在 STOP 处理中**二次校验**调用方为 OWNER/ADMIN，并拒绝关闭
  系统关键服务（serial/term/keyboard/vfs/fs_mem_driver/fs_virtio_blk_driver/
  perm/manager/user）；kill 由持有 ATOM_SERVICE_MANAGE 的 user 服务执行
  （shell 不是管理面，无杀进程能力）。
- 验证：`scripts/verify_users.py`（登录/建号/列号/登出/越权拒杀/管理员杀）。

---

## 十、补充八：分阶段实施路线图
| Phase | 内容 | 关键交付 | 验收标准 |
|-------|------|---------|---------|
| **P0 地基** ✅ | 内核 SubjectID + 消息头身份 + 能力生命周期 | `process_t.subject_id`、`SYS_IPC_RECV_FROM` sender 出参、`cap_entry_t` 扩展（atom/expiry/quota/scope）、`SYS_CAP_CREATE_ATOM`/`SYS_CAP_CONSUME`/`SYS_CAP_REVOKE_BY_ATOM`、`atom.h` 枚举 | 两个进程互冒充身份被拒；能力超时自动失效；批量撤权生效；31/31 测试通过（27 现有 + 4 新增 P0 测试）|
| **P1 引擎** ✅ | perm-engine（perm-manager 进化版） | 策略存储（内存+导出）、角色解析、规则链评估、check API、能力签发路径、UI 聚合标签 | 弹窗→授权→能力签发闭环；角色切换热重载；UI 显示聚合文案；回归 31/31 + P1 10/10 |
| **P2 接入** ✅ | VFS 全操作接入 + 敏感 syscall 门控 | 文件 open/read/write 全部走 check；FileHandle 能力化（抹位）；`sys_set_time` 等走能力门控 | 未授权读写被拒且 EPERM；只读授权拿不到 WRITE 句柄；现有书签流程兼容；回归 31/31 + P1 10/10 + P2 Gate 3/3 + P2 VFS 4/4 |
| **P3 上下文** | 前台感知、频率计数、路径约束 | 焦点/前台服务；频率计数；scope=bookmark 落地（协议已预留：`perm_req_check_t.scope_hash`、`PERM_OP_CONTEXT`/`PERM_OP_FREQ`、perm-engine 频率计数表） | 切后台摄像头能力回收；1h 10 次位置限制生效 |
| **P4 治理** | 持久化、审计、开发者模式、恢复模式 | Policy.db 落盘（块设备就绪后）、加密审计、Developer 角色+水印、Recovery 签名链（协议已预留：`PERM_OP_POLICY_SAVE`/`PERM_OP_POLICY_LOAD` 热重载、`PERM_OP_AUDIT` 审计环形缓冲） | 重启策略保留；审计不可篡改；无 Root 路径可达 |

**顺序原则**：P0 是 P1-P4 的前提（身份+生命周期不落地，上层全是空中楼阁）；
P1 与现有 Powerbox 并行演进（perm-manager → perm-engine 平滑替换）；P2 的
syscall 门控依赖 P0 的 cap 扩展。

---

## 十一、风险与未决问题

| # | 风险/问题 | 等级 | 缓解 |
|---|----------|------|------|
| 1 | 身份层工作量被低估（IPC 路径、进程生命周期、兼容 app_id_hash） | 高 | P0 单列、先做最小内核改动 |
| 2 | 能力表 1024 槽 / 静态池 40MB 可能被原子缓存撑爆 | 中 | 评估 MAX_CAPS 扩容或懒创建（cap.c:71-74） |
| 3 | 撤权与「已打开句柄」的语义：内核作废能力后，VFS 句柄需重新校验 | 中 | P2 明确「每次操作按 scope 重查」策略 |
| 4 | 前台感知依赖窗口管理器，当前无此组件 | 中 | 先用 Timed 近似；P3 再引入 |
| 5 | 生物识别 / TPM / 恢复模式超出当前硬件能力 | 低（远期） | 现阶段用「物理串口 + 签名 blob」近似 |
| 6 | 无签名体系 → Trust-level 全为 Low | 低 | P4 引入签名链 |
| 7 | 策略 DSL（字符串原子）与内核枚举的双维护成本 | 低 | 映射表单点维护 + 编译期断言 |
| 8 | 审计「不可篡改」在无真实持久化/TPM 前无法达成 | 中 | P4 前审计写入只追加卷 + 导出验证 |

---

## 十二、总结：粒度对照表

| 维度 | 传统 Unix / Android | 本设计模型 |
|------|--------------------|-----------|
| 身份 | UID（数字）/ AID | 内核签发 SubjectID（UUID）+ 人格（Persona） |
| 特权账户 | Root（万能） | 无 Root，拆分 Owner/Admin，需物理确认 |
| 权限粒度 | rwx | `<域>.<子域>:<操作>` 编译为原子枚举 |
| 时间控制 | 无（永久） | 单次、限时（能力 expiry 兜底）、前台专用 |
| 路径控制 | 整个挂载点/目录 | 书签 scope_hash + 策略层预过滤 |
| 资源频率 | 无 | perm-engine 计数限制每小时次数 |
| 决策位置 | 无（内核硬编码） | perm-engine（PDP）+ 内核能力缓存（PEP） |
| 审计 | 弱（sudo logs） | 强制加密审计，主体不可抵赖 |

### 9.5.4 命令策略三层架构（v0.5）

**用户架构决策**：环境变量不承载安全策略（避免多用户覆盖/沙盒逃逸/远程会话
混淆），只存进程本地用户偏好（PS1/EDITOR/LANG）。命令可用性由三层决定：

1. **Capability（内核）**——能不能做。进程缺 `ATOM_*`/能力时执行直接
   `ERR_NOCAP`/VFS `-105`（硬限制，不可伪造）。
2. **Policy DB（policy 服务）**——用不用。`user/services/policy/` 维护
   `角色 → {命令 → ALLOW/DENY/UNSET}` 表（GUEST/CHILD 禁 exec/kill/stop/
   useradd 等）；IPC：QUERY（shell 拉取）/ SET / DUMP（仅 OWNER/ADMIN）。
3. **Shell 覆盖（启动参数/登录态）**——这一次怎么用。shell 启动/登录/登出
   时向 policy 服务拉取当前角色的 verdict 表（FNV-1a 哈希过滤表），执行时
   拦截 DENY；policy 服务不可达时用硬编码**救急列表**（help/ls/cat/echo/
   env/export/unset/login/whoami/exit/reboot）保证管理员可恢复。

验证：guest 登录后 `exec`/`kill` 被 shell 拦截（Policy 层）；guest 的 VFS
读被能力层拒绝（Capability 层）。环境变量仅影响 PS1 等偏好。
