# .ops 应用包格式与沙盒授权（v0.3 契约，Phase A 冻结）

> 契约状态：**已冻结**（Phase A）。头文件 `user/services/pkg/pkg.h` 与本文档为本里程碑
> 的并行实现依据；冻结后任何并行任务不得单方面修改本文档或 pkg.h 中已定义的字段/op。
> 如需变更，先与编排者确认并更新本文档版本号。

版本：v1.0（2026-08）

## 1. 目标

- 定义 `.ops` 应用包二进制格式（v1 简化版：单 ELF payload，不做 squashfs/压缩）。
- 定义 pkg-manager 沙盒授权流：应用的能力完全由包内 manifest 声明，pkg-manager
  （内核背书）按 manifest 向应用 subject 签发 atom capability。
- 定义 pkg 端口协议（`user/services/pkg/pkg.h`）与权限原子名映射表。

## 2. .ops 文件布局（v1）

```
偏移      大小      字段
0         4         magic   = 0x3153504F  ("OPS1" little-endian)
4         4         version = 1
8         4         manifest_len  (u32, ≤ PKG_MANIFEST_MAX=512)
12        4         payload_len   (u32, > 0)
16        manifest_len   manifest 文本（UTF-8, LF 换行；≤100 列为 host 侧 ops_pack.py 强制，pkg-manager 解析器不校验行宽）
16+manifest_len  payload_len   单 ELF（x86_64, 由 SYS_PROCESS_CREATE 直接加载）
```

- 无对齐要求；全部整数小端。
- `magic` 错误 → `ERR_INVAL`；`manifest_len` 越界 → `ERR_INVAL`；`payload_len == 0` → `ERR_INVAL`。

## 3. manifest 文本格式

每行 `key=value`，`#` 开头为注释行，空行忽略。未知 key → `ERR_INVAL`（严格解析）。

| key | 必填 | 说明 |
|---|---|---|
| `app_id` | 是 | 应用标识，`[a-zA-Z0-9_]{1,63}`，作为安装目录名与进程名 |
| `app_name` | 否 | 显示名（TUI/日志用），仅记录性，pkg-manager 忽略 |
| `version` | 否 | 版本字符串，仅记录性，pkg-manager 忽略 |
| `entry` | 否 | 入口名，仅记录性，pkg-manager 忽略（实际入口取 ELF entry） |
| `permissions` | 否 | 逗号分隔的原子名列表（见 §4），空/缺省 = 无任何权限 |

示例：

```
# hello.ops manifest
app_id=hello
app_name=Hello Demo
version=1.0
permissions=
```

```
app_id=sbox_demo
app_name=Sandbox Demo
permissions=sys.set_time
```

## 4. 权限原子名映射（pkg-manager 内置表，封闭集合）

manifest 中出现的每个名字必须命中下表；未命中 → `ERR_INVAL`（拒绝安装）。
**禁授清单**（出现在 manifest 中 → 拒绝安装）：`service.manage`、`cap.grant_self`、
`sys.debug`（管理面原子，应用永不可声明）。

| manifest 名 | atom_id | 说明 |
|---|---|---|
| `sys.set_time` | ATOM_SYS_SET_TIME | 改系统时间（沙盒演示主用例） |
| `sys.set_timezone` | ATOM_SYS_SET_TIMEZONE | 改时区 |
| `sys.shutdown` | ATOM_SYS_SHUTDOWN | 关机 |
| `hw.camera.capture` | ATOM_HW_CAMERA_CAPTURE | 摄像头 |
| `hw.mic.record` | ATOM_HW_MIC_RECORD | 麦克风 |
| `hw.gpu.high_perf` | ATOM_HW_GPU_HIGH_PERF | GPU 高性能 |
| `hw.loc.coarse` | ATOM_HW_LOC_COARSE | 粗粒度定位 |
| `hw.loc.precise` | ATOM_HW_LOC_PRECISE | 精确定位 |
| `data.docs.read` | ATOM_DATA_DOCS_READ | 文档读 |
| `data.docs.write` | ATOM_DATA_DOCS_WRITE | 文档写 |
| `data.dl.write` | ATOM_DATA_DL_WRITE | 下载目录写 |
| `data.app.container.read` | ATOM_DATA_APP_CONTAINER_READ | 应用容器读 |
| `data.sys.logs.read` | ATOM_DATA_SYS_LOGS_READ | 系统日志读 |
| `bookmark.resolve` | ATOM_BOOKMARK_RESOLVE | bookmark 解析 |
| `net.bind` | ATOM_NET_BIND | 网络绑定 |
| `net.connect` | ATOM_NET_CONNECT | 网络连接 |
| `net.wifi.scan` | ATOM_NET_WIFI_SCAN | Wi-Fi 扫描 |
| `net.wifi.set` | ATOM_NET_WIFI_SET | Wi-Fi 配置 |
| `pkg.install` | ATOM_PKG_INSTALL | 安装应用（Phase A 记录，暂不强制） |

## 5. 沙盒授权流（Phase A 定案）

**原则**：权限身份 = 内核签发的 `subject_id`（不可伪造）。应用从 `pkg` 端口连入时，
pkg-manager 用 `ipc_recv_from` 取 kernel 认证的调用者 subject，经
`proc_info_by_subject(subject)` 交叉校验 pid，再按 manifest 向该 subject 签发原子。

```
shell: pkg install <name> [--perms=a,b,c]
  → pkg-manager: blob_get(name) 取内嵌 ELF → 构造 manifest（app_id=name,
    permissions=--perms）→ 内存打包 .ops → fs_write 到
    /Volumes/Users/Apps/<name>/app.ops （写入 Users 内存卷，重启不保留；仅 Disk 卷持久）
  （pkg-manager 首次写 Users 卷会触发 Powerbox → term 面板 → 用户 y）

shell: pkg run <app_id>
  → pkg-manager: fs_read /Volumes/Users/Apps/<app_id>/app.ops → 解析 manifest
    → 记录 s_pending[pid]{app_id, atoms[]} → process_create(app_id, payload)
    → 返回 pid

应用启动 → libpkg: pkg_ready()
  （无参数：app_id 由内部 get_subject() + proc_info_by_subject() 推导）
  → 连 "pkg" 端口 → PKG_OP_APP_READY
  → pkg-manager: ipc_recv_from → subject → proc_info_by_subject(subject)
    校验 proc->name == app_id 且 pid == s_pending 记录 → 匹配
    → 对 manifest 每个原子: cap_grant_to_subject(subject, atom, RIGHT_ALL, 0, 0)
    → 回复 0
  → 应用后续系统调用（如 os_set_time）由内核 cap 表门控
```

**拒绝路径（沙盒演示核心）**：manifest 未声明 `sys.set_time` → pkg-manager 不签发
`ATOM_SYS_SET_TIME` → 应用 `os_set_time()` 返回 `ERR_NOCAP`；应用**无法自授**（见 §6）。

## 6. 内核门控（Phase A 内核改动，防绕过）

现状：`SYS_CAP_CREATE_ATOM`/`SYS_CAP_REVOKE_BY_ATOM`/`SYS_CAP_GRANT_TO_SUBJECT`
与 perm `do_grant` 四处入口，Phase A 关闭后已全部门控，沙盒无法自授。门控表：

| 入口 | 门控 | 说明 |
|---|---|---|
| `sys_cap_create_atom` | 调用者持 `ATOM_CAP_GRANT_SELF` | 无此原子 → `ERR_NOCAP` |
| `sys_cap_grant_to_subject` | 调用者持 `ATOM_SERVICE_MANAGE` | 同上 |
| `sys_cap_revoke_by_atom` | 调用者持 `ATOM_SERVICE_MANAGE` | 同上 |
| `sys_cap_has_atom` | 调用者持 `ATOM_SERVICE_MANAGE` | 只读查询：`subject` 是否持 `atom` → 1/0 |
| perm `do_grant` | 调用者持 `ATOM_SERVICE_MANAGE` | 能力制（见下） |
| perm `do_answer` | 调用者持 `ATOM_SERVICE_MANAGE` | 能力制：term（UI 代理）应答用户裁决；init/perm/pkg 管理面；沙盒应用无法自答（防提权） |
| perm `do_revoke` | 自撤销恒允许；撤销**他人**授权须持 `ATOM_SERVICE_MANAGE` | 防任意 Ring 3 进程撤销他人授权（DoS） |
| `sys_panic` | 调用者持 `ATOM_SYS_DEBUG` | 开发者模式原子；防任意 Ring 3 进程一键崩溃内核（DoS） |
| `sys_kill` | 自杀恒允许；杀**他人**须持 `ATOM_SERVICE_MANAGE` | 防 SIGKILL 掉 manager/serial 等关键服务（DoS） |
| `sys_notify` | 目标必须为本进程线程 | 防向外部线程注入虚假通知位 |
| `sys_debug_getchar` | 调用者持 COM1（0x3F8-0x3FF）IO 端口读能力 | 控制台输入归 serial 驱动；防窃取/消耗控制台输入 |
| `sys_fb_get_info` / `sys_fb_map` | 调用者持 `ATOM_SERVICE_MANAGE` | 显示帧缓冲为共享系统资源，归 term 服务（UI 代理） |

门控用 `cap_lookup_by_atom(proc->cap_table, proc->subject_id, ATOM, 0)` 判存在（同
syscall.c:818 SYS_SET_TIME 模式）。

**perm `do_grant`/`do_answer`/`do_revoke` 门控为能力制而非角色制**：授予以「调用者
持有 `ATOM_SERVICE_MANAGE` 原子」为准，而非 `role_is_management`。原因：grant 必须
能压倒角色默认（§四 grants beat role defaults）——P1 回归把 init 热切换为 GUEST 后
仍要求 `PERM_OP_GRANT` 成功，而 init 即使 GUEST 也持 `ATOM_SERVICE_MANAGE`（种子）；
应用永不可持该原子（原子表封闭集合，见 §4），故应用无法授/无法答/无法撤销**他人**
授权（自撤销恒允许，无危害）。perm 通过
`cap_has_atom(caller_subject, ATOM_SERVICE_MANAGE)` 查询（`SYS_CAP_HAS_ATOM`，新增
syscall 66）。`do_role_set` 仍为角色制（管理平面热切换）。

**种子（不破 P0/P1/P2 回归）**：
- `kernel_main.c` init 能力表追加 `ATOM_CAP_GRANT_SELF` + `ATOM_SERVICE_MANAGE` +
  `ATOM_SYS_DEBUG`（RIGHT_ALL）→ init 的 cap_create_atom/cap_grant_to_subject/
  cap_revoke_by_atom 测试、P1 GRANT/ANSWER/REVOKE 测试与 SYS_PANIC 门控保持可用。
- `process_desc.c`（sc_sys_process_create 尾部，进程能力表分配后）**按 blob 身份 +
  调用者门控**种子：调用者须持 `ATOM_SERVICE_MANAGE`，且用户传入 blob 与
  `blob_get("manager")`/`blob_get("perm")`/`blob_get("pkg")`/`blob_get("term")`
  memcmp 相等 → 新进程能力表种 `ATOM_SERVICE_MANAGE`。**调用者门控是生产加固**：
  否则应用可 blob_get("perm") + process_create 逐字节副本 → 内容一致种子 → 获得
  管理原子（提权）。合法链路：init → manager → {perm,pkg,term}，init/manager 均持
  原子，真实服务 spawn 不受影响。perm 的 decision_encode/grant_revoke、
  pkg-manager 的 manifest 签发与 term 的 Powerbox 应答（UI 代理）均依赖此种子。
- syscall 编号追加（`SYS_CAP_HAS_ATOM = 66`，不重排），`SYS_COUNT` 同步为
  `SYS_CAP_HAS_ATOM + 1`。
- 零拷贝（Phase 3）：`SYS_SHM_CREATE = 67`（受信服务分配物理页池，门控
  `ATOM_SERVICE_MANAGE`）、`SYS_SHM_MAP = 68`（池页只读映射到客户端，门控
  `ATOM_SERVICE_MANAGE` + 池表校验，防任意物理内存映射）；`SYS_COUNT` 同步为
  `SYS_SHM_MAP + 1`。种子表扩展：`vfs`（导出文件页）、`fs_mem_driver`（创建池）。

## 7. pkg 端口协议摘要（详见 user/services/pkg/pkg.h）

端口名 `"pkg"`。所有消息 < 4096，`req[0]=op`，请求/响应 = 固定结构体。

| op | 方向 | 作用 |
|---|---|---|
| `PKG_OP_INSTALL(1)` | shell → pkg | 从内核 blob 安装（name + 可选 perms） |
| `PKG_OP_LIST(2)` | shell → pkg | 列已安装应用（Users/Apps 枚举） |
| `PKG_OP_RUN(3)` | shell → pkg | 运行应用（返回 pid） |
| `PKG_OP_REMOVE(4)` | shell → pkg | 删除应用（递归删目录） |
| `PKG_OP_APP_READY(5)` | 应用 → pkg | 就绪握手；pkg 据此签发 manifest 原子 |

## 8. 验收标准（Phase A）

1. `make iso` 0 新警告。
2. QEMU 回归：31/31 + P1 10/10 + P2 Gate 3/3 + P2V 4/4 + KBD 1/1 全绿（init 种子后不回归）。
3. `pkg install hello`（Powerbox y）→ `pkg list` 显示 hello → `pkg run hello` 正常退出。
4. `pkg install sbox_demo --perms=sys.set_time` → `pkg run sbox_demo` → 打印
   `set_time OK`。
5. `pkg install sbox_demo_noperm`（无权限）→ `pkg run sbox_demo_noperm` → 打印
   `set_time DENIED (no permission)`，且应用尝试 `cap_create_atom(ATOM_SYS_SET_TIME)`
   自授也返回 `ERR_NOCAP`（证明门控有效）。
6. `scripts/ops_pack.py` 能在 host 侧打包/校验 `.ops`（与 pkg-manager 解析互认）。

## 9. 验收记录（Phase A，2026-08-16，QEMU 实测）✅

**验收方法**：`make iso` 构建 → QEMU（`-vnc 127.0.0.1:0` + `-serial file:` + unix
monitor socket）→ sendkey 注入 Powerbox `y` → serial log + screendump 双路取证 →
VGA framebuffer 解码（1024x768 32bpp @ 0xfd000000，8x16 字体 / 9x20 px 网格 = 113x38
cells，PPM 头 `P6\n1024 768\n255\n`，pixel data 在 maxval 行之后）。验收驱动同时
检测崩溃标记 `KERNEL PANIC|Triple fault|BOOT:`。

**观测模型（修正）**：shell 提示符/命令回显/错误只渲染到 VGA linear framebuffer；
serial 永不显示 shell 输出（`TERM_DEBUG_SERIAL_MIRROR` 编译排除）。serial 承载服务
libc printf（pkg/hello/sbox_demo 的 debug_log）。Powerbox 面板只渲染到 VGA；用 QEMU
monitor `sendkey y` 回答，等待约 3 s（verdict hold 100 ticks + restore + focus
release）；授权按 (subject, resource) 粒度，多次 PENDING 需多轮 y。init (PID 1)
启动时有一个 pending 面板（`Access: X` → `System/Kernel/init.elf`），验收驱动先
`answer_panels()`（max_rounds=5）应答再键入首个命令。

| # | 标准（§8） | 结果 | 证据 |
|---|---|---|---|
| 1 | `make iso` 0 新警告 | ✅ | exit 0；`-Wall -Wextra -O2` 0 新警告 |
| 2 | QEMU 回归 31/31 + P1/P2 Gate/P2V/KBD 全绿 | ✅ | serial.log：`=== Results: 31/31 passed ===`、`=== P1 Permissions: 10/10 passed ===`、`=== P2 Gate: 3/3 passed ===`、`=== P2 VFS: 4/4 passed ===`、`=== KBD Focus: 1/1 passed ===` |
| 3 | `pkg install hello`（Powerbox y）→ list 显示 → run 正常退出 | ✅ | `pkg: installed 'hello' (0 atom(s), 90053 bytes)`；VGA `pkg list: 1 app(s) installed`；`pkg: run 'hello' pid 17 (0 atom(s) pending)`；信号自测 PASSED；SIGTERM 后 `proc: LAST_THREAD pid=17 code=143 waiting_tid=-1`（128+15 正常按信号终止；hello 未被 wait，waiting_tid=-1，无 survival 行） |
| 4 | `pkg install sbox_demo --perms=sys.set_time` → run → `set_time OK` | ✅ | `pkg: installed 'sbox_demo' (1 atom(s), 85906 bytes)`；`pkg: run 'sbox_demo' pid 18 (1 atom(s) pending)`；`pkg: signed 'sbox_demo' pid 18 with 1 atom(s)`；`set_time OK` |
| 5 | `pkg install sbox_demo_noperm` → run → `set_time DENIED` + 自授 `ERR_NOCAP` | ✅ | `pkg: installed 'sbox_demo_noperm' (0 atom(s), 85888 bytes)`；`signed ... with 0 atom(s)`；`set_time DENIED (no permission)`；`self-grant attempt = -3 (expect ERR_NOCAP)` |
| 6 | `scripts/ops_pack.py` host 打包/校验与 pkg-manager 解析互认 | ✅ | cross_check 7/7 PASS（见下） |

**§8.6 互认细节（host ops_pack ↔ pkg-manager）**：
- 格式一致：16 B 头（magic/version/manifest_len/payload_len 小端）+ manifest +
  ELF payload；`OPS_MAGIC=0x3153504F`（"OPS1" LE）、`OPS_VERSION=1`、
  `manifest_len ∈ (0,512]`、`plen > 0`、`16+mlen+plen == 总长`（pkg_ops_parse 精确
  尺寸检查，host 侧同规则）。
- 双向往返：host `ops_pack.py pack` 产物 hello.ops（90096 B）与 sbox_demo.ops
  （85906 B = 16+42+85848）被 pkg-manager 解析规则接受；pkg-manager 风格
  （`app_id=` 单行 / `app_id`+`permissions=`）构造的 sys_*.ops 被 ops_pack check
  接受（含 sys_sbox_demo_noperm.ops；sbox_demo_noperm 是 sbox_demo.elf 的内核 blob
  别名，`kernel/blob/blob.c` 注册，非独立 ELF）。
- 封闭原子表 19 项（§4）双方一致；管理面原子 `service.manage` / `cap.grant_self` /
  `sys.debug` 双向拒绝（负例 PASS）。

备注：hello 安装共 4 次尝试 / 3 轮 Powerbox `y`（安装路径按 (subject, resource)
多次授权）；其余命令各 1 轮 y。验收后 serial.log 停在 sbox_demo_noperm 正常退出，
无崩溃/挂起。
