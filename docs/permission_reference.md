# OpSys 权限引擎参考手册（Permission Engine Reference）

> 适用版本：OpSys v1.0-dev（HEAD 105805d + 工作区）　|　最后更新：2026-09-20
>
> 本文是 **`perm` 服务（权限引擎 / PDP）的操作与实现参考**：它由哪些部件组成、一次判定按什么顺序做决定、
> 协议长什么样、策略怎么存、命令行怎么用、被拒绝时怎么定位。设计动机与决策记录见
> [permission_model.md](permission_model.md)；VFS 侧的对象模型见 [vfs_design.md](vfs_design.md)；
> 逐 op 的线格式见 [service_reference.md](service_reference.md) §6.5 与 §6.15。

---

## 一、它在系统里的位置

```
       客户端进程（shell / 应用 / 服务）
                 │  ① IpcCall("vfs", ...)      —— 只与 VFS 对话，从不直接问 perm
                 ▼
            vfs_server  ──② PERM_OP_CHECK（携带内核填充的 sender subject）
                 │              ▼
                 │        perm 引擎（本手册的对象）
                 │         · 授权表 / 角色表 / 规则表 / 上下文表 / 频率表 / 审计环
                 │         · 判定 → 允许(抹位后的位掩码) / 拒绝 / 建询问
                 │              │
                 │              ├─③ 需要用户裁决时 → IpcSend("perm.ui") → term 面板
                 │              └─④ 允许时 CapGrantToSubject() → 把能力写进**目标进程**的内核能力表
                 ▼
            驱动 / 文件内容
```

三条不可动摇的分工（[permission_model.md](permission_model.md) §三/§四）：

1. **身份由内核签发**：`subject_id` 在进程创建时分配、在 IPC 交付时由内核填充，服务端一律通过
   `IpcRecvFrom` 取用，**绝不接受请求体里的身份字段**。
2. **判定在内核之外、但结果在内核之内**：用户态的 perm 只负责"决定"；决定被编码成内核能力
   （`atom_id` + `subject` + `expiry` + `quota` + `scope`），敏感系统调用在内核里做纯表查找，**零 IPC**。
3. **decision cache 就是能力表**：判定结果一旦签发，就不再需要 perm 参与；撤销通过
   `CapRevokeByAtom` 让缓存立刻失效。

---

## 二、身份：subject 从哪来

| 项 | 说明 |
| --- | --- |
| 分配 | 内核 `process_create()`，全局唯一、**永不复用**；`0` = 内核/System，`1` = init |
| 读取 | 服务端 `IpcRecvFrom(port, buf, &len, &tok, &sender_subject)`；客户端用 `GetSubject()` 自查 |
| 用途 | 授权表、角色表、上下文表、频率表、审计条目**全部以 subject 为键** |
| 与 PID 的关系 | **两套互不相同的命名空间**：PID 会被复用，subject 不会。命令行里两者都能用，但解析规则不同（见 §十） |
| 与控制台账户的关系 | `user` 服务在登录时把 **subject → 账户**绑定，并把角色同步进 perm（`PERM_OP_ROLE_SET`）；登出解绑 |

---

## 三、原子权限表（23 个）

原子是"语义化的权限点"，是授权与策略的索引。定义在 `kernel/include/kernel/atom.h`（内核与用户态共享）。

| 分类 | 原子 | 含义 |
| --- | --- | --- |
| 系统 | `ATOM_SYS_SHUTDOWN` | 关机 / 重启 / 停机 |
| | `ATOM_SYS_SET_TIME` | 修改系统时间 |
| | `ATOM_SYS_SET_TIMEZONE` | 修改时区 |
| 硬件 | `ATOM_HW_CAMERA_CAPTURE` | 摄像头采集 |
| | `ATOM_HW_MIC_RECORD` | 麦克风录音 |
| | `ATOM_HW_GPU_HIGH_PERF` | 高性能 GPU |
| | `ATOM_HW_LOC_COARSE` / `ATOM_HW_LOC_PRECISE` | 粗略 / 精确定位 |
| 数据 | `ATOM_DATA_DOCS_READ` | 读用户文档（**VFS 读请求映射到此**） |
| | `ATOM_DATA_DOCS_WRITE` | 写用户文档（**VFS 写请求映射到此**） |
| | `ATOM_DATA_DL_WRITE` | 写下载目录 |
| | `ATOM_DATA_APP_CONTAINER_READ` | 读应用容器 |
| | `ATOM_DATA_SYS_LOGS_READ` | 读系统日志 |
| | `ATOM_BOOKMARK_RESOLVE` | 解析安全作用域书签 |
| 网络 | `ATOM_NET_BIND` | 绑定本地端口 / 监听 |
| | `ATOM_NET_CONNECT` | 建立连接 / 发送数据 |
| | `ATOM_NET_WIFI_SCAN` / `ATOM_NET_WIFI_SET` | 扫描 / 配置无线网络 |
| 管理 | `ATOM_PKG_INSTALL` | 安装应用包 |
| | `ATOM_PKG_UPDATE_SYS` | 更新系统包 |
| | `ATOM_SERVICE_MANAGE` | 管理服务（**管理面的总开关**） |
| 开发者 | `ATOM_SYS_DEBUG` | 调试设施（panic 钩子、控制台输入等） |
| | `ATOM_CAP_GRANT_SELF` | 为自己铸造原子能力 |

> `ATOM_NONE = 0` 表示"无原子语义"；`ATOM_MAX` 是上界哨兵。**23 是真实原子数**，
> 枚举共 24 个取值（含 `ATOM_NONE`）、加上哨兵共 25 个符号。

---

## 四、角色与默认规则种子

角色决定"默认能不能"，与账户绑定（`user` 服务登录时同步）：

| 角色 | 取值 | 定位 |
| --- | --- | --- |
| OWNER | 0 | 完全控制（设备所有者） |
| ADMIN | 1 | 系统管理（无 OWNER 级硬件权限） |
| STANDARD | 2 | 默认角色（**未登记主体的兜底角色**） |
| CHILD | 3 | 受限 |
| GUEST | 4 | 最小权限 |
| AUDITOR | 5 | 只读 + 审计 |

服务启动时播种的规则链（`SeedRules()`，共 23 条）：

| 角色 | 允许 | 显式拒绝 |
| --- | --- | --- |
| OWNER | DOCS_READ, DOCS_WRITE, DL_WRITE, NET_CONNECT, SERVICE_MANAGE, PKG_INSTALL, SYS_DEBUG, CAP_GRANT_SELF | — |
| ADMIN | DOCS_READ, DOCS_WRITE, DL_WRITE, NET_CONNECT, SERVICE_MANAGE, PKG_INSTALL | — |
| STANDARD | DOCS_READ | —（写没有规则 → 走默认拒绝 → 询问用户） |
| CHILD | DOCS_READ | DOCS_WRITE |
| GUEST | — | DOCS_READ, DOCS_WRITE, NET_CONNECT |
| AUDITOR | DOCS_READ, SYS_LOGS_READ | DOCS_WRITE |

**"没有规则"与"拒绝规则"是两种不同的结果**：前者落到默认拒绝（可以弹 Powerbox 询问用户），
后者是策略明确说不（**不弹窗**，直接 `VFS_ERR_ACCESS`）。

---

## 五、一次判定的完整流程（v1.0）

### 5.1 决策顺序

```
PERM_OP_CHECK(subject, resource{vol_uuid,item_id}, access, scope_hash, url)
  │
  ├─ 0. 隔离期？           subject 处于隔离 → 拒绝(VFS_ERR_ACCESS) + QUARANTINED，**不弹窗**
  │
  ├─ 1. 授权命中？         grant_find(subject, resource, scope, access)
  │      · TTL 过期        → 就地回收 + PERM_EV_EXPIRE 审计 + EXPIRED 标志，视同不存在
  │      · scope 不相等    → 不算命中，记 SCOPE_MISMATCH 标志，继续往下
  │      · 命中             → 允许，granted = grant.access ∩ requested（抹位）+ GRANT_BEAT
  │
  ├─ 2. 角色链（逐位求值） RuleLookup(role, atom)
  │      · 请求的每个 READ/WRITE 位各自查一次它的原子：
  │          全部允许 → 允许(granted = 这些位) + ROLE_CHAIN
  │          任一显式拒绝 → 拒绝 + ROLE_CHAIN（**不弹窗**）
  │          其余（部分无规则 / 全无规则）→ 落到第 3 步
  │      · EXEC/COW 等无原子位（ATOM_NONE）**永不被角色链授予**（抹位）
  │
  └─ 3. 默认拒绝 → 询问
         · 后台上下文（已登记且 foreground=0）→ 拒绝 + BACKGROUND，**不建询问、不推面板**
         · 否则 → 建/复用一条 PENDING 询问 → IpcSend("perm.ui") 推面板
                  → 应答 VFS_ERR_ACCESS；用户 `perm answer <id> y` 后
                     · 写入授权记录（可带 TTL 与 scope）
                     · CapGrantToSubject() 把能力签发进发起进程
```

### 5.2 决策标志 `flags`（`perm_resp_check_t.flags`）

调用者（以及自检套件）靠它区分"为什么是这个结果"，不必解析日志文本：

| 标志 | 含义 |
| --- | --- |
| `PERM_DEC_GRANT_BEAT` | 由一条显式授权记录直接放行 |
| `PERM_DEC_ROLE_CHAIN` | 由角色的规则链放行或拒绝 |
| `PERM_DEC_DEFAULT_DENY` | 没有任何规则/授权覆盖 |
| `PERM_DEC_BACKGROUND` | 因主体是后台而被拒（且未弹窗） |
| `PERM_DEC_QUARANTINED` | 因主体处于隔离期而被拒 |
| `PERM_DEC_SCOPE_MISMATCH` | 存在覆盖该资源的授权，但作用域不匹配 |
| `PERM_DEC_EXPIRED` | 存在覆盖该资源的授权，但已过期并被回收 |

### 5.3 抹位（rights masking）规则

| 场景 | 结果 |
| --- | --- |
| 授权命中 | `granted = grant.access & requested` —— 只多不少地裁掉请求里没被覆盖的位 |
| 角色链放行 | `granted = 请求中的 READ/WRITE 位中被规则允许的子集`；**EXEC/COW 等无原子位一律丢掉** |
| 典型例子 | 请求 `READ` + `EXEC` → 句柄只带 `READ`（P2V 自检断言）；请求 `READ` + `WRITE` 且角色两者都允许 → 两个位都带 |
| 语义 | 句柄权限**只减不增**；后续每次 READ/WRITE 都用句柄实际持有的位复检 |

### 5.4 上下文与隔离对判定的影响

- **上下文**只影响"要不要弹窗"，不改变授权与规则的结果：登记为后台的主体在默认拒绝路径上直接失败。
  未登记 = 前台（这是 P1/P2 语义不变的保证）。
- **隔离**是第 0 步的短路：隔离期间连授权命中都不再看，直接拒绝且不弹窗。

---

## 六、IPC 协议总表

端口：`"perm"`（服务自身）与 `"perm.ui"`（向 `term` 推送面板）。全部请求/响应结构见 `user/services/perm/perm.h`。

| # | op | 方向 | 关键字段 | 说明 |
| --- | --- | --- | --- | --- |
| 1 | `PERM_OP_QUERY` | 控制台/UI | `query_id`（0=最先的 PENDING） | 取出待裁决询问（含 label 与发起进程信息） |
| 2 | `PERM_OP_ANSWER` | 控制台/UI | `query_id`, `allow`, **`ttl_ticks`**, **`scope_hash`** | 裁决；允许时写授权 + 签发能力。**回答不会放大作用域** |
| 3 | `PERM_OP_CHECK` | **仅 vfs_server** | `resource`, `access`, `scope_hash`, `subject_id`（由服务端填）, `url` | 同步判定；响应含 `granted` 与 **`flags`** |
| 4 | `PERM_OP_REVOKE` | shell/管理面 | `subject_id`（0=全部）, `resource`（零 UUID=全部） | 丢弃授权，返回撤销条数 |
| 5 | `PERM_OP_GRANT` | 管理面/测试 | `resource`, `access`, `subject_id`, `atom`, **`expiry_ticks`**, **`scope_hash`**, **`source`** | 直接签发授权（绕过 Powerbox） |
| 6 | `PERM_OP_UI_SHOW` | perm → term | `label`, `state` | 推送/更新面板 |
| 7 | `PERM_OP_ROLE_SET` | 管理面 / user 服务 | `subject_id`, `role` | 角色热更新 |
| 8 | `PERM_OP_DUMP` | 管理面/测试 | — | 导出角色表与规则表（文本行） |
| 9 | `PERM_OP_CONTEXT` | 管理面 / UI agent | `subject_id`, `foreground`, **`list`** | 设置上下文；`list=1` 时只读 |
| 10 | `PERM_OP_FREQ` | 管理面/测试 | `subject_id`, `atom`, `reset`, **`clear_quarantine`** | 频率计数查询/清零、隔离解除 |
| 11 | `PERM_OP_POLICY_SAVE` | 管理面 | **`include_expired`** | 导出策略快照 |
| 12 | `PERM_OP_POLICY_LOAD` | 管理面 | `size`, `data[]` | 导入快照（全有或全无 + 热更新） |
| 13 | `PERM_OP_AUDIT` | 管理面/测试 | **`subject_id`**, **`verdict_filter`**, **`since_tick`**, **`max_entries`** | 导出审计（最旧在前，可过滤） |
| 14 | `PERM_OP_SET_QUIET` | 管理面/测试 | `quiet` | 抑制 UI 推送（自检用，避免开机弹窗） |
| 15 | `PERM_OP_CTX_QUERY` | 管理面/UI | — | 只读读取上下文表（v1.0 新增） |

**门控**：

| op | 要求 |
| --- | --- |
| `CHECK` / `ANSWER` | 调用者必须持 `ATOM_SERVICE_MANAGE`（否则 `ERR_DENIED`）。CHECK 更是只信任 vfs_server —— 否则任何进程都能探测他人授权（授权预言机） |
| `ROLE_SET` | 管理角色，**或** 持 `ATOM_SERVICE_MANAGE` 且进程名为 `user` 的账户服务（防止被降级的 init 自我提权） |
| `CONTEXT` 写 / `FREQ` 清隔离 / `POLICY_*` / `AUDIT` / `DUMP` / `SET_QUIET` | 管理面（`ATOM_SERVICE_MANAGE`） |
| `CTX_QUERY` / `QUERY` | 只读，不设门槛 |

---

## 七、审计

### 7.1 事件表（`perm_audit_ent_t.event`）

| 事件 | 触发点 |
| --- | --- |
| `PERM_EV_CHECK_ALLOW` / `PERM_EV_CHECK_DENY` | 每一次判定 |
| `PERM_EV_POWERBOX` | 默认拒绝并**创建了询问**（真正"询问了用户"） |
| `PERM_EV_ANSWER` | 用户对询问的裁决 |
| `PERM_EV_GRANT` / `PERM_EV_REVOKE` | 授权签发 / 撤销 |
| `PERM_EV_ROLE_SET` | 角色热更新 |
| `PERM_EV_CONTEXT` | 上下文切换 |
| `PERM_EV_QUARANTINE` | 进入 / 解除隔离 |
| `PERM_EV_EXPIRE` | 授权因 TTL 到期被回收 |
| `PERM_EV_POLICY_SAVE` / `PERM_EV_POLICY_LOAD` | 策略快照导出 / 导入 |

### 7.2 verdict 语义（**易错点**）

`perm_audit_ent_t.verdict` **直接使用规则表的枚举**，不是独立的 0/1 约定：

```c
#define PERM_VERDICT_DENY  0
#define PERM_VERDICT_ALLOW 1
```

也就是说 **1 = granted、0 = denied**。写日志分析工具或加断言时按枚举判断，不要按数字直觉判断
（早期文档曾把这条写反，v1.0 已修正注释并在 §7.3 的过滤器语义里保持同一套取值）。

### 7.3 环形缓冲与过滤

| 项 | 值 / 行为 |
| --- | --- |
| 容量 | `PERM_AUDIT_MAX = 64` 条（受 4096 字节报文上限约束） |
| 覆盖 | 满了覆盖最旧的一条 |
| 顺序 | `PERM_OP_AUDIT` 返回**最旧在前** |
| 过滤 | `subject_id`（0=全部）、`verdict_filter`（0=全部、1=仅 granted、2=仅 denied）、`since_tick`（0=不限）、`max_entries`（0=全部） |
| 持久化 | 默认不落盘；`perm audit save [url]` 导出为文本（默认 `/Volumes/Disk/perm.audit`） |

---

## 八、策略快照（持久化）

### 8.1 v2 布局（`PERM_POLICY_VERSION_V2 = 2`）

```text
header (16 B) : magic 'POLY' u32 | version u32 | grant_count u32 | role_count u32
grants (64 槽) : subject_id u64 | expiry_ticks u64 | access u32 | scope_hash u32
                 | source u8 | vfs_resource_t(24 B)          = 49 B/槽
roles  (64 槽) : subject_id u64 | role u8                     =  9 B/槽
总大小         : 16 + 64*49 + 64*9 = 3728 B  ≤ PERM_POLICY_MAX(3840) ✔（有 _Static_assert 钉死）
```

表序填充、空槽全零，因此 `save → load → save` **字节一致**（P2V 自检断言这一点）。

### 8.2 v1 兼容

读到 version 1 的快照时按旧布局（grant 36 B、role 12 B，无 expiry/scope/source）解析，
新字段补 0，来源标记为 `PERM_SRC_POLICY`。**v2 记录则原样保留快照里的来源** ——
这既让快照真实反映"这条授权是谁给的"，也保住了上面的字节一致断言。

### 8.3 加载语义

| 项 | 规定 |
| --- | --- |
| 全有或全无 | 先整体校验（magic/version/count/role 范围/access 非零/source 范围），失败**完全不动**当前策略 |
| 热更新 | 成功后逐条 `CapRevokeByAtom` + `DecisionEncode`，让已在运行进程的能力表同步 |
| 规则表 | **不在快照里**（规则是静态策略）；加载不触碰规则表，P6 自检断言 `rule_count` 不变 |
| 过期授权 | `include_expired=0`（默认）时跳过并**就地回收**过期槽位 |
| 文件 | 由**管理员会话**读写（shell `perm save/load`，默认 `/Volumes/Disk/perm.policy`）；perm 服务自身不做 I/O，因此没有"策略引擎偷偷改文件"的路径 |

---

## 九、频率与隔离

| 参数 | 值 | 位置 |
| --- | --- | --- |
| 拒绝阈值 | `PERM_DENY_THRESHOLD = 8` | `perm.h` |
| 隔离时长 | `PERM_QUARANTINE_TICKS = 3000`（30 s @100 Hz） | `perm.h` |
| 滚动窗口 | `PERM_DENY_WINDOW_TICKS = 1000` | `perm.h` |
| 计数槽位 | `PERM_FREQ_SLOTS = 64`（每槽 48 B） | `perm-manager.c` |
| 粒度 | **每个 (subject, atom) 对**独立计数与隔离 | — |

```text
判定 → 命中 hits++ / 拒绝 denies++
窗口超时 → denies 清零、窗口起点前移
denies ≥ 8 → quarantine_until = now + 3000（写 PERM_EV_QUARANTINE）
隔离期   → 第 0 步短路拒绝、不弹窗、QUARANTINED 标志
到期或释放 → 恢复正常，并重置 denies/窗口（避免"刚解除又被一条拒绝重新隔离"）
```

> **粒度选择的理由**：按主体隔离会误伤同时在自我测试的 init —— 它在启动自检里会合法地累计十几次拒绝，
> 主体级隔离会在自检中途把它锁死。按 (subject, atom) 隔离只惩罚"一直要同一种权限却被拒"的行为，
> 这正是抗骚扰/抗探测想要的。

---

## 十、命令行（`perm` 家族，见 [shell_reference.md](shell_reference.md) §3.10）

| 命令 | 说明 | 需要 OWNER/ADMIN |
| --- | --- | --- |
| `perm audit [n] [allow|deny] [subject=<id>] [since=<tick>]` | 查看审计（可过滤） | 否 |
| `perm audit save [url]` | 审计导出为文本文件 | 是 |
| `perm ctx [list]` | 查看前台/后台表 | 否 |
| `perm ctx <subject|pid> fg|bg` | 绑定上下文 | 是 |
| `perm freq [subject|all]` | 命中/拒绝/隔离状态 | 否 |
| `perm freq release <subject|all>` | 解除隔离 | 是 |
| `perm save [url] [all]` | 写策略快照（`all` 含过期授权） | 是 |
| `perm load [url]` | 读回策略快照并热加载 | 是 |
| `permguard [list|<subject|pid> [release]]` | 隔离管理入口 | 是（release） |

参数解析规则：**先按十进制 subject 解析并用 `ProcInfoBySubject` 验证存活**；否则当作 PID，
用 `ProcessList` 确认存活后再从上下文表反查 subject；都失败就明确报错，**绝不猜测**。

---

## 十一、与其它子系统的接口

| 方向 | 接口 | 说明 |
| --- | --- | --- |
| VFS → perm | `PERM_OP_CHECK` | 每次 open/read/write/move/create_dir/delete/enum/stat_handle/truncate/bookmark 解析都问一次 |
| perm → 内核 | `CapGrantToSubject` / `CapRevokeByAtom` / `CapHasAtom` | 把决定编码为能力；查询他人持有时需要自己是管理面 |
| 内核 → 服务 | `IpcRecvFrom` 填充的 subject | perm 判定与审计的唯一身份来源 |
| `user` → perm | `PERM_OP_ROLE_SET` | 登录绑定时同步角色（管理面门控：管理角色或"持原子且名为 user"） |
| perm → term | `IpcSend("perm.ui")` | 推送询问面板（`PERM_OP_SET_QUIET` 可抑制） |
| `net` → 内核 | `CapHasAtom(subject, ATOM_NET_*)` | v1.0 起网络操作也走原子门控；`user` 登录时签发 `NET_CONNECT`/`NET_BIND` |
| 应用包 → perm | `.ops` manifest 的权限声明 | `pkg-manager` 按声明签发原子能力（见 [ops_format.md](ops_format.md)） |

---

## 十二、常量与上限

| 常量 | 值 | 含义 |
| --- | --- | --- |
| `PERM_MAX_GRANTS` | 64 | 授权表槽位 |
| `PERM_MAX_ROLES` | 64 | 角色表槽位 |
| `PERM_MAX_RULES` | 96 | 规则表槽位（播种 23 条） |
| `PERM_MAX_QUERIES` | 16 | 询问队列深度（相同待裁决询问会复用） |
| `PERM_FREQ_SLOTS` | 64 | 频率/隔离计数器槽位 |
| `PERM_CTX_SLOTS` / `PERM_CTX_LIST_MAX` | 16 / 16 | 上下文表容量 / 单次列表返回上限 |
| `PERM_AUDIT_MAX` | 64 | 审计环容量 |
| `PERM_POLICY_MAX` | 3840 | 快照上限（v2 实际 3728） |
| `ATOM_MAX` | 24 | 原子枚举上界（真实原子 23 个） |

---

## 十三、排错决策树

```text
被拒绝了（-105 EACCES / VFS_ERR_ACCESS）
  │
  ├─ perm audit deny 10        看最近 10 条拒绝，读 event 与 flags 相关事件
  │     · CHECK_DENY           → 规则链拒绝或默认拒绝；看 atom 与 subject
  │     · POWERBOX             → 默认拒绝**并且**弹了面板（用户没答/答了 n）
  │     · QUARANTINE           → 主体被隔离：perm freq <subject> 看剩余 tick，
  │                              perm freq release <subject> 解除
  │
  ├─ perm ctx list             主体是不是被登记成后台了？（后台拒绝不会弹窗，容易被误认为"没反应"）
  │
  ├─ perm ctx <subject> fg     需要交互的场景把它切回前台
  │
  ├─ whoami                    当前账户与角色；GUEST/CHILD 的写入是**显式拒绝**（不会弹窗）
  │
  ├─ perm save / perm load     怀疑策略状态异常时先导出快照留证，再决定是否回滚
  │
  └─ 仍然不通？                检查 vfs 侧：句柄是否已失效（VFS_ERR_STALE 而不是 ACCESS），
                               资源是否属于只读卷（VFS_ERR_READONLY），
                               路径是否被名称校验拒绝（ERR_INVAL）
```

常见误判：

| 现象 | 真实原因 |
| --- | --- |
| "我用管理员登录了还是被拒" | 命令要求的能力未必随登录签发（例如 `ATOM_SYS_DEBUG` 从不自动签发）；或被隔离中 |
| "没有弹出面板，说明没走到权限检查" | 可能正是**后台上下文**或**隔离期**在起作用，它们刻意不弹窗 |
| "撤销授权后旧句柄还能读" | v1.0 起每次读都复检；若仍能读，先确认撤销的是**同一资源**（授权按 (subject, 资源UUID+itemID, scope) 命中） |
| "改了文件但没生效" | 策略快照要显式 `perm load`；它不会自动加载 |

---

## 十四、验证锚点

| 锚点 | 位置 | 证明什么 |
| --- | --- | --- |
| `P6 Permission/VFS: 10/10` | init 启动自检 | 授权 TTL 过期、scope 匹配、后台拒绝不弹窗、8 次拒绝隔离 + 手动解除、审计事件完整、策略 save→load（含角色/规则未被破坏） |
| `P1 Permissions: 10/10` | init 启动自检 | 身份、OWNER 自动放行、角色热重载、拒绝不弹窗、Powerbox 授权、撤销、GRANT 覆盖角色 |
| `P2 Gate: 6/6` | init 启动自检 | 敏感系统调用的内核门控（含未授权 reboot/halt → `ERR_NOCAP`） |
| `P2 VFS: 4/4` | init 启动自检 | VFS 全 op 授权与**能力抹位** |
| `perm audit/ctx/freq/save/load/permguard` | `scripts/verify_tools.py` | 命令面端到端可用（32 项检查的一部分） |

---

## 十五、源码索引

| 内容 | 位置 |
| --- | --- |
| 协议与常量（唯一事实源） | `user/services/perm/perm.h` |
| 引擎实现 | `user/services/perm/perm-manager.c`（判定 DoCheck、审计 AuditAppend、策略 PolicySerialize/PolicyApply、频率/隔离、上下文） |
| 原子枚举 | `kernel/include/kernel/atom.h` |
| 内核能力校验 | `kernel/cap/cap.c`（`CapLookupByAtom`）、`kernel/syscall/syscall.c`（各敏感 syscall 的门控点） |
| 内核身份签发 | `kernel/process/process.c`（subject 分配）、`kernel/ipc/ipc.c`（`IpcRecvFrom` 填充） |
| VFS 侧调用点 | `user/services/vfs/vfs_server.c` 的 `PermCheck()` 及其所有调用点 |
| 命令行 | `user/services/shell/cmd_perm.c`、`user/services/shell/shell.c` 的 `CmdPerm` |
| 面板渲染 | `user/services/term/term.c` 的 `perm.ui` 处理 |
| 设计文档 | [permission_model.md](permission_model.md)（§13 为本轮落地记录） |

> 返回 [文档索引](README.md)
