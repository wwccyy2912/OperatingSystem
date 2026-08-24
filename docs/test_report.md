# OpSys 全面测试报告（三轮高强度）

> 日期：2026-08-16
> 依据：`.omo/plans/full-test-plan.md`（R1 基线回归 + R2 盲区补测 + R3 压力/边界）
> 驱动：`scripts/smoke_test.py`（QEMU 双通道自动化：serial 服务日志 + VGA screendump 解码，
> 观测模型见 `docs/ops_format.md` §9）
> 结果：**全部通过** —— 正常模式 63 项 + `--drive` 追加 5 项，0 崩溃、0 回归、`make iso` 0 新警告。

## 一、测试环境与方法

| 项 | 值 |
|---|---|
| 构建 | `make iso` → `build/opsos.iso`（GRUB，`-Wall -Wextra -O2`，0 新警告） |
| QEMU | `qemu-system-x86_64 -m 256M -vnc 127.0.0.1:0`<br>`-serial file:build/serial.log`<br>`-monitor unix:/tmp/opsys-mon.sock` |
| 磁盘卷 | `--drive` 模式附加 `disk.img`（8 MiB，virtio-blk-pci `disable-modern=on`） |
| 输入 | monitor `sendkey` 注入；Powerbox 面板用 `sendkey y` 应答（~3s hold） |
| 取证 | `build/serial.log`（2702 行，两轮启动：初启 + `system_reset` 后复启）+ VGA 解码 |
| 崩溃标记 | `KERNEL PANIC\|Triple fault\|BOOT:` 出现次数 = **0** |

## 二、第一轮：基线回归 + 服务冒烟（R1）

| # | 测试项 | 方法 | 结果 |
|---|---|---|---|
| R1.1 | `make iso` 0 新警告 | 构建 | ✅ |
| R1.2 | init 49 项回归套件 | wait_for 5 个锚点 | ✅（串口） |
| R1.3 | 服务启动序列 | wait_for 7 个 `svc * started` | ✅（串口） |
| R1.4 | serial-test 自检 | wait_for `serial-test: PASS` | ✅（串口） |
| R1.5 | shell 冒烟（meminfo/ports/threads/uptime/ps） | sendkey + 响应关键字 | ✅（5/5，VGA） |
| R1.6 | VFS 内存卷 + 卷枚举（ls/ls-root/write/cat/stat） | sendkey + 写读一致 + `ls /` 列卷 | ✅（5/5，VGA） |
| R1.7 | Powerbox 书签流（授权/拒绝往返） | 8 步：pre-auth -105 → panel y → 授权 → revoke → -105 | ✅（8/8） |
| R1.8 | pkg 流（install/list/run + hello 信号自测） | 4 步 | ✅（4/4） |
| R1.9 | pkg 沙盒（manifest 权限签发/拒绝 + 自授失败） | install sbox_demo --perms → `set_time OK`；sbox_demo_noperm → `DENIED` + `self-grant = -3` | ✅（10/10，串口） |
| R1.10 | 书签跨 move 仍有效 | bm_resolve after move | ✅（并入 R1.7） |
| R1.11 | screendump 基线 | VGA 解码管线 | ✅ |

**R1 检查**：VGA 场景 10 + 串口锚点 12 + Powerbox 流程 8 + pkg 流程 4 + pkg 沙盒 10 = 44 项（KBD/flaky 锚点计入 R2）。

## 三、第二轮：盲区补测（R2）

| # | 盲区 | 方法 | 结果 |
|---|---|---|---|
| R2.1 | keyboard 焦点（TAKE/RELEASE_FOCUS） | init KBD_OP 直调：TAKE→0、重取幂等、owner RELEASE→0、非 owner→-3 | ✅ `=== KBD Focus: 1/1 passed ===` |
| R2.2 | IRQ/通知路径（keyboard IRQ1） | sendkey 注入后字符回显 | ✅（R1.5/R3.4 隐式覆盖） |
| R2.3 | VFS 余下 op（REVOKE_BM/CREATE_DIR/DELETE） | bm_revoke→resolve 无缓存；mkdir/rm 往返 | ✅（4/4） |
| R2.4 | pkg REMOVE + install 错误路径 | remove hello → list 0 → 二次 remove FAILED | ✅（3/3） |
| R2.5 | 信号（SIGNAL/KILL/SIGRETURN） | hello 7 段信号自测 | ✅（R1.8 覆盖） |
| R2.6 | 进程监控（PROCESS_WAIT/LIST/KILL） | ps 列表 + kill 错误路径 + flaky 重启策略 | ✅（flaky FAILED 锚点命中） |
| R2.7 | 磁盘卷 + virtio-blk | `--drive`：write → cat → `system_reset` → cat 复查 | ✅（见 R3.5） |
| R2.8 | term 渲染（VGA 面板） | screendump + PPM 解码（`tools/vga_decode.py`） | ✅（观测模型全流程） |
| R2.9 | runtime（malloc/realloc/atexit/init_array） | runtime_demo 未接线 Makefile | ⏸ 暂缓（见 §五） |
| R2.10 | 权限引擎 P3/P4 预留接口 | init P2V 4 项 round-trip | ✅（不倒退） |

**R2 新增检查**：KBD 焦点 1 + 书签撤销 2 + mkdir/rm 2 + pkg remove 3 + kill 1 + flaky 锚点 1 = 10 项。

## 四、第三轮：压力/边界 + 持久化（R3）

| # | 测试项 | 方法 | 结果 |
|---|---|---|---|
| R3.1 | IPC 压力（多客户端） | init 100k 往返 + 3 并发 hello spawn | ✅（并入 R3.3） |
| R3.2 | 内存边界/耗尽 | `fallocate /Users/big.bin` → 32 MiB 卷 NOSPC → uptime 存活 | ✅（NOSPC at 32 MiB） |
| R3.3 | 多进程并发 | spawn×3 → 计数 `hello: signal self-test PASSED` baseline+3 → ps 无 hello | ✅（5/5） |
| R3.4 | 键盘洪水 | `flood_command` 5ms/键注入 stat → 回显完整 + 结果出现 | ✅（2/2） |
| R3.5 | 持久化重启 | `--drive`：write persist.txt → `system_reset` → 复启后 cat 5 bytes | ✅（5/5） |
| R3.6 | 全量回归确认 | 第一轮全部重跑 | ✅（63 项重跑全过） |

**R3 新增检查**：fallocate 2 + spawn/ps 5 + flood 2 + 磁盘持久化 5 = 14 项（含 `--drive` 模式）。

## 五、已知记录

1. **R2.9 runtime 补测暂缓**：`runtime_demo`/`tui_demo` 未接线 Makefile（既有决策，AGENTS.md
   有载）。malloc/realloc 已由 init 堆守卫测试与 hello 信号自测隐式覆盖，专项补测待 demo
   接入后执行。
2. **`--drive` 复启后 `answer_panels` 报 `TIMEOUT[vga] panel 2`**：良性。复启仅 1 个 init
   待决面板，`answer_panels` 应答后继续等待第 2 个面板直至超时；`cat` 复查不受影响
   （READ 走角色链授权，无面板）。已计入 R3.5 通过。
3. **`SERIAL_ANCHORS` 的 P2 gate/P2V/KBD 锚点用正则交替**（摘要行 | 首测试行）：守护
   `sys_debug_log` token bucket（512/tick、桶上限 1024）未来回退时摘要行被截断的情况；
   本次运行均命中摘要行本身。
4. **串口启动竞态**：内核 debug_log 直写 COM1 与 serial 服务写入器在启动期竞争，偶发吃掉
   `manager: ` 前缀首字节（观察过 `anager: term started`）。锚点从服务名开始匹配，语义不变。

## 六、证据

- `build/serial.log`（2702 行）：`=== Results: 31/31 passed ===`、`=== P1 Permissions:
  10/10 passed ===`、`=== P2 Gate: 3/3 passed ===`、`=== P2 VFS: 4/4 passed ===`、
  `=== KBD Focus: 1/1 passed ===` 各 2 次（两轮启动）；`serial-test: PASS` ×2；
  `manager: flaky marked FAILED` ×2；崩溃标记 0。
- VGA 解码：Powerbox 面板文本、shell 回显、`fallocate: NOSPC at 32 MiB`、`ps` 无 hello 行。
- 最终消息：`=== SMOKE PASSED (R1 + R2 + R3) ===`（VGA，仅 shell 通道）。

## 七、结论

三轮共 **68 项检查（正常 63 + `--drive` 5）全部通过**，49 项 init 回归套件不倒退，
`make iso` 0 新警告，QEMU 全程 0 崩溃。R1-R3 覆盖矩阵（计划 §五）各模块均已覆盖，
唯一暂缓项 R2.9（runtime_demo 未接线）已记录待后续执行。

---

## 八、补充：R2.9 专项补测执行（2026-08-21）

R2.9（runtime_demo/tui_demo 专项补测）此前因 demo 未接线 Makefile 而暂缓；本次
`runtime_demo`/`tui_demo` 已接入 Makefile（`USER_C`/`SVC_NAMES`/`SVC_LINK_RULE`/
`blob.c`），补充验证完成（驱动：`scripts/verify_demos.py`，QEMU 双通道：

| # | 检查项 | 结果 |
|---|---|---|
| 1 | `exec runtime_demo`：全局构造函数先于 main（resource id=42） | ✅ |
| 2 | atexit 逆序（3→2→1，counter=3） | ✅ |
| 3 | `.fini_array` 析构（Resource destructor called） | ✅ |
| 4 | malloc/calloc/realloc 就地扩展（buf2 保留内容） | ✅ |
| 5 | signal() 注册（SIGUSR1/SIGTERM/SIGPIPE，prev=SIG_DFL） | ✅ |
| 6 | `exec tui_demo`：term 端口解析 + 渲染完成 + 干净退出 | ✅ |
| 7 | `exec hello`：信号自测 7 段（注册→自杀→handler→IGN→DFL→SIGTERM 终止 143） | ✅ |
| 8 | `exec window_demo`：3 窗口渲染 + 键盘焦点切换（VGA 解码 `*` 标记）+ 干净退出 | ✅ |

> 注：引导后 P2V 套件会遗留一个 init EXEC 待决 Powerbox 面板（已知行为，
> test_report §五.2），面板持有键盘焦点；脚本先 `sendkey y` 应答再执行命令。

## 九、补充：生产加固回归（2026-08-21，第二轮）

内核侧生产加固（安全门控、崩溃恢复、force_exit 修复）后的回归扩充与验证：

| 项 | 结果 |
|---|---|
| 经典套件 31→33（+FPU/SSE 交错、+IPC 对端死亡 crashpeer） | ✅ 33/33 |
| P2 门控 3→5（+notify 限本进程、+debug_getchar COM1 门控） | ✅ 5/5 |
| 新增 P3 Crash Recovery（kill pkg → 自动重启 → 端口恢复） | ✅ 1/1 |
| smoke R1-R3 全量 | ✅ 62/62 |
| smoke `--drive`（R3.5 跨重启持久化） | ✅ 65/65 |
| P4 资源耗尽（2026-08-21 第三轮）：IPC 超长消息边界 ERR_INVAL；线程表 1024 耗尽 ERR_NOMEM + join 释放后恢复 | ✅ 2/2 |
| P5 零拷贝读路径（2026-08-22）：System blob 经共享池 READ_ONLY 映射，内容与 chunked 读一致 | ✅ 1/1 |
| 用户账户 + 退出保护（2026-08-22，`scripts/verify_users.py`）：login admin / whoami / useradd bob / users / logout / login bob / bob 越权 stop 被拒(-9) / admin stop pkg 成功 | ✅ 8/8 |
| 全量回归 + smoke（2026-08-22，MAX_THREADS 1024→2048 后）：classic 33/33 + P1 10/10 + P2 5/5 + P2V 4/4 + KBD 1/1 + P3 1/1 + P4 2/2 + P5 1/1；smoke R1-R3 全量 + `--drive` | ✅ 全绿 |

关键修复：`alloc_thread` 漏清 `force_exit`（SIGKILL 线程槽位回收后新进程在首个
检查点被静默杀死，code 0）；`ipc_cleanup_process`/`irq_cleanup_process`（进程死亡
时销毁端口/唤醒阻塞对端/释放注册名与 IRQ 线）；blob 注册 fail-fast。

2026-08-22 追加修复：
- **login -9 根因**：ROLE_SET 原按调用方角色门控（`role_is_management`），user 账户
  服务（STANDARD 角色）被拒 → 登录绑定后角色同步失败。改为「OWNER/ADMIN 角色
  **或**（ATOM_SERVICE_MANAGE + 内核进程名 `user`）」双门控；P1 test 8（降级 init
  不可自我提权）与 login 角色同步同时满足。
- **shell `users` 显示 `(%u)` 字面量**：shell_printf 不支持 `%u`，改用 `%d`。
- **输出字符串 em-dash**：VGA 字体仅覆盖 0x20–0x7E，`—` 渲染为空格，全部改为 ASCII `-`。
- **TUI 非破坏性弹框**：term 新增 TERM_OP_SNAPSHOT/RESTORE；`tui_confirm` /
  `tui_input_line` 弹框前后保存/恢复屏幕区域与光标，不再残留对话框边框。
- **MAX_THREADS 1024→2048**：P4 断言需 1000/1023 槽，user 服务 +1 线程后仅余 999；
  扩表 + P4 测试同步更新（≥2000）。

---

## 第四轮：v0.4 窗口管理器（2026-08-22）

| 项 | 结果 |
|---|---|
| wm 服务启动（manager spawn，端口 `wm` 注册，3 线程：server/input/主） | ✅ |
| wm_demo 桌面：libwm 创建 3 窗口（Terminal/Files/Settings）+ 内容行 + 注册表 3/3 | ✅ |
| 合成器渲染：VGA 解码见三个边框窗口 + 标题 + 正文（113x38 网格内） | ✅ |
| 焦点：新建窗口默认聚焦（`* Settings`）；`1` → `* Terminal`；`2` → `* Files` | ✅ |
| 移动：`l` 使焦点窗口右移 1 格（几何变化，越界钳制） | ✅ |
| 会话退出：`q` → 释放键盘焦点 + 清屏 + wm_demo 销毁窗口退出 + shell 恢复交互 | ✅ |
| `scripts/verify_wm.py` | ✅ 8/8 |
| 回归：classic 33/33 + P1 10/10 + P2 5/5 + P2V 4/4 + KBD 1/1 + P3 1/1 + P4 2/2 + P5 1/1 | ✅ 57/57 |
| smoke R1-R3 全量 + `--drive`（含 wm 进程出现在 ps：PID=19 THR=3） | ✅ 全绿 |
| `verify_users.py`（登录/建号/越权拒杀/管理员杀） | ✅ 8/8 |
| `make iso` | ✅ 0 警告 |

架构要点：wm 是纯 IPC 客户端——渲染经 term（显示所有者，ATOM_SERVICE_MANAGE
门控 fb），输入经 keyboard 焦点路由；窗口注册表操作用 `ipc_recv_from` 的
内核 subject 做 owner 门控（DESTROY/MOVE/WRITE），管理面
（ATOM_SERVICE_MANAGE）可跨窗口操作。BLOB_MAX_ENTRIES 24→26（+wm/+wm_demo）。

---

## 第五轮：环境变量 + 命令策略三层架构（2026-08-23）

| 项 | 结果 |
|---|---|
| libc 环境：`environ`/`getenv`/`setenv`/`unsetenv`/`putenv`（C11 §7.22.4） | ✅ |
| shell：`export`/`unset`/`env` 命令 + `PS1` 动态 prompt | ✅ QEMU 验证 |
| policy 服务：角色→命令 verdict 表（GUEST/CHILD 禁 exec/kill/stop/useradd 等 18 条种子） | ✅ |
| shell 命令过滤：启动/登录/登出时从 policy 拉取 verdict，执行时拦截，救急列表兜底 | ✅ guest 登录后 `exec`/`kill` 被拒 |
| 环境变量定位：纯用户偏好（PS1/EDITOR），**不承载安全策略**（用户架构决策） | ✅ |
| 回归：classic 33/33 + P1 10/10 + P2 5/5 + P2V 4/4 + KBD 1/1 + P3 1/1 + P4 2/2 + P5 1/1 | ✅ 57/57 |
| `make iso` | ✅ 0 警告 |

### 第五轮补充（2026-08-23 下午）：动态内存优化 + useradd 漏洞

| 项 | 结果 |
|---|---|
| malloc 大小分桶（tcache 风格）：16..2048 共 8 桶，小分配 O(1) 取用/归还，桶满溢出回全局 first-fit 保持合并；realloc 就地增长跨桶查找（block_is_free/unlink） | ✅ 回归含 heap guard 全过 |
| PMM next-fit hint：bitmap_find_free 从上次位置续扫（两遍回绕），重复小分配 O(run) 而非 O(total) | ✅ |
| **useradd 角色漏洞修复**：无效角色名曾静默 `atoi→0=OWNER`（可意外提权）；现严格校验角色名/纯数字，非法即报错 | ✅ `useradd bad badrole` → invalid role |
| 回归 57/57 + P3 1/1 + P4 2/2 + P5 1/1；`make iso` 0 警告 | ✅ |

### 第五轮补充 2（2026-08-23）：账户锁定/解锁 + 自动锁定策略

| 项 | 结果 |
|---|---|
| `user_lock <name>` / `user_unlock <name>`（admin，禁自锁/禁锁最后 admin） | ✅ |
| 自动锁定：连续 5 次错误密码 → 账户 disabled，正确密码也拒绝 | ✅ 串口 `auto-locked after 5 failed logins` |
| 解锁重置 fail_count；`users` 列表标注锁定账户（`L` 后缀） | ✅ |
| 回归 57/57 + P3 1/1 + P4 2/2 + P5 1/1；`make iso` 0 警告 | ✅ |

### 第五轮补充 3（2026-08-23）：启动/自检流程优化

| 项 | 结果 |
|---|---|
| 自检 fail-fast：每套件（classic/P1/P2/P2V/KBD/P3/P4/P5）失败即打印 `SELFTEST FAILURE` 并中止后续启动（此前失败只计数、静默继续） | ✅ |
| 自检汇总：`init: ALL SELFTESTS PASSED (33/33)` + `entering idle loop` | ✅ 串口确认 |
| 回归 57/57 + P3 1/1 + P4 2/2 + P5 1/1；`make iso` 0 警告 | ✅ |

### 第五轮补充 4（2026-08-23）：Shell 命令全量检查

| 项 | 结果 |
|---|---|
| 40 个注册命令逐一审查（help/echo/pid/free/clear/cap/ports/sleep/threads/mutex/exec/uptime/exit/reboot/kill/ps/ls/cat/stat/tee/fallocate/mkdir/rm/bm_*/perm_*/mv/pkg/login/logout/whoami/passwd/useradd/userdel/user_lock/user_unlock/users/stop/export/unset/env） | ✅ 全部定义齐全、逻辑正确 |
| 修复 stop 错误分支 em-dash（`" — "` → `" - "`，VGA 仅 0x20-0x7E） | ✅ |
| QEMU 实测：free/uptime/pid/ps/help/env/tee/cat/mkdir/ls/rm 全部 PASS | ✅ |
| 回归 57/57 + P3 1/1 + P4 2/2 + P5 1/1；`make iso` 0 警告 | ✅ |

### 第五轮补充 5（2026-08-23）：系统动态机制 - 策略运行时热更新

| 项 | 结果 |
|---|---|
| `policy_set <role> <cmd> <allow\|deny\|unset>`（admin 代理）：运行时调整命令策略，立即生效 | ✅ guest 的 exec 由 DENY→allow 后成功 spawn |
| `policy_dump`：导出策略表（ROLE cmd verdict） | ✅ |
| 架构：user 服务做管理代理（持 ATOM_SERVICE_MANAGE + 按调用者 subject 校验 OWNER/ADMIN）；policy 服务持管理原子可查他人能力 | ✅ |
| 修复：cap_has_atom 查询他人需自身持原子 → policy 加入 s_svc_blobs 种子列表 | ✅ |
| 回归 57/57 + P3 1/1 + P4 2/2 + P5 1/1；`make iso` 0 警告 | ✅ |

### 第五轮补充 6（2026-08-23 晚）：问题修复 + cd/关机/UNIX 风格

| 项 | 结果 |
|---|---|
| **kill -3 修复**：admin 登录后 SIGKILL 其他进程改走 user 服务代理（USER_OP_KILL，持 ATOM_SERVICE_MANAGE + OWNER/ADMIN 校验 + 保护关键服务）；非 admin 保持内核门控并给清晰提示 | ✅ admin `kill 1` → `'init' (PID 1) killed` |
| **whoami -4 修复**：未登录输出 `nobody`（UNIX 风格，退出码 0） | ✅ |
| **users -9 修复**：未登录/非 admin 输出友好提示 `permission denied - OWNER/ADMIN login required` | ✅ |
| **cd/pwd 命令**：shell 维护 cwd，`cd [dir]` 校验目录存在 + 类型；VFS 命令（ls/cat/tee/mkdir/rm/stat/fallocate/mv/bm_create）支持**相对路径** | ✅ 相对 tee/cat 工作 |
| **shutdown 命令**：新 syscall SYS_SHUTDOWN（39，门控 ATOM_SYS_SHUTDOWN）：ACPI PM1a 端口 0x604 S5 → 回退 8042 复位 | ✅ 编译通过 |
| **UNIX 风格**：`bm <create\|resolve\|revoke>`、`perm <answer\|query\|revoke>`、`policy <set\|dump>`、`userlock`/`userunlock` 别名（下划线原名保留兼容） | ✅ policy dump 生效 |
| 回归 57/57 + P3 1/1 + P4 2/2 + P5 1/1；`make iso` 0 警告 | ✅ |

### 第五轮补充 7（2026-08-23 深夜）：TUI 组件库 + 文件管理器 + 命令交互 TUI 化

| 项 | 结果 |
|---|---|
| libtui 新增 `tui_menu`（v1.3）：标题+盒子+滚动列表，j/k/s/w 移动、Enter 选择、q 取消，非破坏性覆盖（快照/恢复同 tui_confirm） | ✅ |
| **`fm` TUI 文件管理器**：浏览目录/卷，Enter 进目录，文件 v=查看/d=删除（确认框），q 退出；相对/绝对路径 | ✅ QEMU 验证 |
| **rm/mv TUI 确认对话框**（破坏性/变更操作） | ✅ `rm` 弹 Confirm Delete，n 取消 |
| **kill 无参 → TUI 进程选择**（process_list + tui_menu） | ✅ 菜单出现、q 取消 |
| **users → TUI 账户弹窗**（admin） | ✅ |
| 回归 33/33 + P1 10/10（全量 57/57 待确认）；`make iso` 0 警告 | ✅ |

### 第六轮（2026-08-24）：Shell 历史/补全 + TUI 剩余功能

| 项 | 结果 |
|---|---|
| 键盘服务：0xE0 扩展键（方向键/Home/End/PgUp/PgDn）解码为单字节控制码（Up=0x0B Down=0x0C Left=0x08 Right=0x14 Home=0x01 End=0x05 PgUp=0x02 PgDn=0x06） | ✅ |
| **Shell 命令历史**：Up/Down 翻历史（16 条环形缓冲，去重），PgUp/PgDn 首/末，Home/End 行首/尾 | ✅ QEMU：pid→up×3→echo one |
| **Tab 补全**：首词补命令名，后续词补路径（cwd 相对/绝对），唯一匹配补全+空格，歧义列候选 | ✅ e+Tab→echo/exec/exit/export/env |
| **fm 重命名/复制**：r=rename（TUI 输入新名）、c=copy（读+写 .copy 副本） | ✅ rename p2vfs.bin→renamed |
| **mv 目标目录 TUI 选择**：`mv src ?` 列出 cwd 子目录菜单 | ✅ |
| tui_menu 增强：方向键 + Home/End/PgUp/PgDn | ✅ |
| `make iso` 0 警告 | ✅ |
