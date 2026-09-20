# 测试与验证指南（Testing & Verification Guide）

> 适用版本：OpSys v0.8-dev（git HEAD 105805d）　|　最后更新：2026-09-19
>
> 本文给出 OpSys 三层测试体系的完整落地方法：init 内置启动自检（Ring 3 端到端、fail-fast）、宿主机 Python + QEMU 自动化脚本矩阵、构建期静态检查，并附逐套件断言清单与串口锚点、双通道观测模型、新增测试模板、回归判定标准与常见失败排查。

## 一、测试体系总览

### 1.1 三层结构

```
                    ┌──────────────────────────────────────────────────────┐
                    │ 第 3 层：构建期静态检查                              │
                    │   make iso          → -Wall -Wextra（Makefile:37,53）│
                    │   make format-check → clang-format --dry-run -Werror │
                    └────────────────────────┬─────────────────────────────┘
                                             │ 产出 build/opsos.iso
                                             v
   ┌──────────────────────────────────────────────────────────────────────────┐
   │ 第 2 层：宿主机 Python 自动化（QEMU monitor + sendkey + screendump）      │
   │   smoke_test.py / accept.py / verify_wm.py / verify_window_demo.py        │
   │   verify_users.py / verify_demos.py / ops_pack.py / tools/vga_decode.py   │
   │        串口 build/serial.log ←──┐                ┌──→ VGA PPM → 文本       │
   └─────────────────────────────────┼────────────────┼────────────────────────┘
                                     │                │
   ┌─────────────────────────────────┴────────────────┴────────────────────────┐
   │ QEMU 客户机：GRUB2（multiboot2, gfxmode 1024x768x32）→ kernel.elf          │
   │ 第 1 层：Ring 3 init（PID 1）启动自检 —— 8 个套件 / 58 项断言              │
   │   任一套件失败 → BootSelftestFail() → 打印 !!! SELFTEST FAILURE ... !!!    │
   │   并停止后续启动阶段（user/services/init/main.c:2494-2499）                │
   └──────────────────────────────────────────────────────────────────────────┘
```

### 1.2 各层职责与边界

| 层 | 运行位置 | 覆盖内容 | 失败语义 | 主要实现 |
|---|---|---|---|---|
| 第 1 层 启动自检 | 客户机 Ring 3（init, PID 1） | 系统调用 ABI、线程/调度、IPC、能力与权限引擎、VFS 授权、键盘焦点、崩溃恢复、资源耗尽、零拷贝读 | **fail-fast**：打印横幅后 init 停止后续启动阶段，不再拉起服务 | `user/services/init/main.c`（2586 行） |
| 第 2 层 宿主自动化 | 宿主机 Python 3 + QEMU monitor | 端到端场景：shell 命令、VFS/Powerbox 书签、pkg 沙盒、磁盘持久化、GUI/WM/账户流、`.ops` 打包互认 | 脚本以非 0 退出码表示失败，失败时 dump 串口尾部与当前 VGA 屏 | `scripts/*.py`、`tools/vga_decode.py` |
| 第 3 层 构建期检查 | 宿主机工具链 | 编译告警（`-Wall -Wextra`）、代码风格（clang-format） | 告警不中断构建（无 `-Werror`），靠"0 新警告"纪律判定；风格检查以 `-Werror` 失败 | `Makefile:32-57`、`Makefile:355-371` |

第 1 层是唯一能在客户机内部做**白盒**断言的层（可直接读内核返回值、断言错误码）；第 2 层只能在**外部**观测（串口文本 + 屏幕像素），因此它的断言全部是正则锚点。

### 1.3 一次完整回归的执行序列

```bash
# 1) 构建（必须先做，脚本不会自己 make：scripts/*.py 中没有任何 make 调用）
make iso                                  # 观察 0 新警告
make format-check                         # 需宿主机安装 clang-format

# 2) 启动自检由 QEMU 启动过程自动完成，宿主机只做观测
python3 scripts/smoke_test.py             # R1 基线 + R2 盲区 + R3 压力
python3 scripts/smoke_test.py --drive     # 追加 virtio-blk 跨复位持久化
python3 scripts/smoke_test.py --step-timeout 60   # 宿主负载高时放宽单步超时

# 3) 专项验证（按改动范围选跑）
python3 scripts/verify_wm.py
python3 scripts/verify_window_demo.py
python3 scripts/verify_users.py
python3 scripts/verify_demos.py

# 4) 与 OS 无关的宿主工具自测
python3 scripts/ops_pack.py check <out.ops>
python3 tools/vga_decode.py <screen.ppm> [--font user/services/term/font.h]
```

> **v1.0 更新**：自检套件现在是 **9 个** —— 新增 `P6 Permission/VFS`（10 项）：授权 TTL 过期、作用域匹配、
> 后台主体拒绝不弹窗、频率隔离与手动解除、审计事件完整、策略快照 save-load（含角色与规则未被破坏的断言）、
> fstat-by-handle、截断缩放零填充、书签过期与续期、移动环检测与保留名校验。
> **顺序约束**：P6 在 P1 之前执行 —— P1 的角色热重载用例会把 init 永久降级为 GUEST（并断言它无法自我提权），
> 而 P6 的 VFS 段需要角色链放行；P6 结束时清空自己产生的待处理 Powerbox 询问，避免被 P1 误判为提示泄漏。

## 二、第 1 层：init 启动自检套件

### 2.1 测试框架与计数器

`init` 的测试框架是 4 个宏加一对全局计数器，位于 `user/services/init/main.c:55-83`：

| 宏 | 行为 | 源码 |
|---|---|---|
| `TEST(name)` | `printf("  TEST: %s ... ", name)`，`tests_run++` | `user/services/init/main.c:60-64` |
| `PASS()` | `tests_pass++`，`printf("PASS\n")` | `user/services/init/main.c:66-70` |
| `FAIL(msg)` | `printf("FAIL: %s\n", msg)`（**不**增加 pass） | `user/services/init/main.c:72-75` |
| `ASSERT(cond, msg)` | 条件不成立时 `FAIL(msg); return;` —— 直接返回，因此该测试项永远不会执行到 `PASS()` | `user/services/init/main.c:77-83` |

套件之间用独立计数器互不干扰：经典套件 `tests_run/tests_pass`（:57-58）、P1 `p1_run/p1_pass`（:1104-1105）、P2 `p2_run/p2_pass`（:1533-1534）、P2V `p2v_run/p2v_pass`（:1669-1670）、KBD `kbd_run/kbd_pass`（:2115-2116）、P3 `p3_run/p3_pass`（:2216）、P4 `p4_run/p4_pass`（:2295）、P5 `p5_run/p5_pass`（:2390）。每个套件另有自己的前缀宏（`P1_TEST`/`P2V_ASSERT`/`KBD_PASS`/...），其中 P4 与 P5 的 `ASSERT` 是变参形式，失败行会带上格式化参数（`user/services/init/main.c:2314-2322`、`:2404-2412`）。

串口上一行通过的测试形如（真实抓取样本见 `build/vbox-dbg-serial.log:60`）：

```text
  TEST: ThreadYield(ring3->ring0->ring3) ... PASS
  TEST: ipc_port_create ... (port=1) PASS
  TEST: get_time ... FAIL: get_time should return > 0
```

### 2.2 套件清单与 fail-fast 门

`main()` 按固定顺序跑 8 个套件，每个套件跑完立即比对"passed == run"（`user/services/init/main.c:2503-2580`）：

| # | 套件名（`BootSelftestFail` 第一参数） | 入口函数 | 项数 | 计数变量 | 判定位置 |
|---|---|---|---|---|---|
| 1 | `classic syscall suite` | `RunTests()`（:1019） | 34 | `tests_pass/tests_run` | :2577-2578（**最后**才判） |
| 2 | `P1 permission engine` | `RunP1PermTests()`（:1507） | 10 | `p1_pass/p1_run` | :2536-2537 |
| 3 | `P2 syscall gate` | `RunP2GateTests()`（:1644） | 5 | `p2_pass/p2_run` | :2542-2543 |
| 4 | `P2 VFS authorization` | `RunP2VfsTests()`（:2085） | 4 | `p2v_pass/p2v_run` | :2549-2550 |
| 5 | `KBD focus` | `RunKbdFocusTests()`（:2203） | 1 | `kbd_pass/kbd_run` | :2554-2555 |
| 6 | `P3 crash recovery` | `RunCrashRecoveryTests()`（:2281） | 1 | `p3_pass/p3_run` | :2559-2560 |
| 7 | `P4 resource exhaustion` | `RunResourceExhaustionTests()`（:2375） | 2 | `p4_pass/p4_run` | :2567-2568 |
| 8 | `P5 zero-copy read` | `RunZeroCopyTests()`（:2478） | 1 | `p5_pass/p5_run` | :2573-2574 |

合计 **58 项断言**。注意两个顺序性事实：

1. 经典套件在**服务管理器被 spawn 之前**运行（`RunTests(); InitProtocol();` 在 `ProcessCreate("manager")` 之前，见 `:2506-2507` 与 `:2523`），但它的失败判定放在所有套件之后（`:2576-2578`），所以经典套件失败时串口仍会看到管理器与服务被拉起，然后才停机。
2. P1/P2/P2V/KBD/P3/P4/P5 全部在管理器启动**之后**运行（`:2535` 起），它们通过 `P1PortGet()` 轮询端口（200 次 × `Sleep(10)`，`:1143-1151`）等待 `vfs`/`perm`/`keyboard` 等服务就绪，因此不存在启动竞态。

### 2.3 经典 syscall 套件（`RunTests`，34 项）

调用顺序见 `user/services/init/main.c:1019-1056`（注意源码中 bench 与 stress 函数的定义位置与调用顺序不同）。

| # | 测试标题字符串（串口精确可见） | 判据要点 | 源码 |
|---|---|---|---|
| 1 | `DebugLog(ring3->ring0->ring3)` | `DebugLog(...) == 0` | :189-194 |
| 2 | `ThreadYield(ring3->ring0->ring3)` | 调用返回即可 | :196-200 |
| 3 | `ipc_port_create` | 返回端口号 `> 0` | :202-208 |
| 4 | `port_register + port_get` | 注册 `test_svc` 后 `PortGet` 必须命中同一端口号 | :210-219 |
| 5 | `PortGet(nonexistent)` | 未注册名字返回 `< 0` | :221-226 |
| 6 | `cap_create` | `CapCreate(0,0) > 0` | :228-234 |
| 7 | `map_memory` | `CAP_TYPE_MEM` 能力 + 页对齐地址映射成功 | :236-245 |
| 8 | `get_pid` | `GetPid() == 1`（init 是第一个用户进程） | :247-252 |
| 9 | `get_free_pages` | 空闲页 `> 0`，并打印 `(N pages, N MB)` | :254-260 |
| 10 | `ThreadCreate(worker)` | 工作线程创建 + 两次 yield + join | :262-273 |
| 11 | `get_time` | 时钟 `> 0`（tick 基准已启动） | :275-281 |
| 12 | `sleep` | `Sleep(10)` 返回 0 且实测 `elapsed >= 3` tick | :283-293 |
| 13 | `thread_join` | `join` 返回 0 且 `exit_code == 0` | :392-402 |
| 14 | `FPU/SSE state across context switches` | 两个线程交错 SSE2 级数，`fxsave/fxrstor` 缺失即失败 | :1003-1017（worker :971-1001） |
| 15 | `unmap_memory` | 映射后 `UnmapMemory` 返回 0 | :404-413 |
| 16 | `heap guard pages` | 堆基址 ± 守卫页映射失败、守卫页 unmap 返回 `ERR_INVAL`、守卫下页映射成功 | :415-452 |
| 17 | `CapGrant(error paths)` | 授予不存在的 PID 必须失败 | :454-462 |
| 18 | `cap_revoke` | 撤销后同一句柄 `map_memory` 必须失败 | :464-474 |
| 19 | `cap expiry (lazy revoke on consume)` | 过期原子首次 consume 返回 `ERR_NOENT`，重复 consume 亦然（不复活） | :478-496 |
| 20 | `cap quota (consume until revoked)` | quota=2：两次 consume 成功，第三次起 `ERR_NOENT` | :498-508 |
| 21 | `CapRevokeByAtom(atom + scope)` | 按原子批量撤销的计数、scope 精确匹配、未命中原子/错误 subject 撤销数为 0 | :510-550 |
| 22 | `ipc_send + ipc_recv` | 跨线程收发 5 字节 "hello"，长度与内容逐字节断言 | :552-582 |
| 23 | `subject identity + unforgeable sender (ipc_recv_from)` | 载荷中伪造的 subject **不得**泄漏；`sender_subject` 必须等于 `GetSubject()` | :584-616 |
| 24 | `IpcCall(error paths)` | 调用不存在端口返回 `< 0` | :618-627 |
| 25 | `ipc_call to dead peer wakes with ERR_NOENT` | spawn `crashpeer`（收信后不回复即退出）：阻塞中的 `IpcCall` 必须被唤醒为 `ERR_NOENT`；名字可被新进程重新注册 | :636-683 |
| 26 | `user stack canary fires on overflow` | spawn `canarytest`：进程退出码必须为 `128+6 = 134`（SIGABRT） | :695-710 |
| 27 | `set_affinity` | `ThreadSetAffinity(1, 0) == 0` | :712-718 |
| 28 | `bench: 100k get_time syscalls` | 10 万次裸 syscall，打印 `(100000 calls, N ticks, cyc_hi=.. cyc_lo=..)`；**只测不判**（始终 PASS） | :298-316 |
| 29 | `bench: 1k solo yields (no switch)` | 1000 次"无对端"yield（`next == cur` 早退路径） | :328-343 |
| 30 | `bench: 1k yield round-trips` | 1000 次与伙伴线程的 ping-pong yield | :355-390（伙伴 :345-353） |
| 31 | `vspace_alloc (alloc + map + pattern + ERR_INVAL)` | 4 页保留区对齐且 ≥ 1 GiB 下界、写入/回读模式一致、`0x4001` 与 flags=1 均返回 `ERR_INVAL` | :722-765 |
| 32 | `ThreadSetCtx(valid + error paths)` | 自身 tid 成功；`ctx_size+1`/负数 tid → `ERR_INVAL`；未知 tid → `ERR_NOENT` | :769-802 |
| 33 | `stress: 1000 threads` | 批量创建 ≥1000 线程（每批 32，批间 join 归还页），共享计数器必须精确等于创建数 | :806-915 |
| 34 | `stress: 100k IPC round-trips` | 10 万次 `IpcCall` 往返，每次校验回显序号，最后断言 `exit_code == 0` | :919-958 |

该套件把 3 个 bench 计入 `tests_run`，因此它们是**回归计数的一部分**（这也是"经典套件 34 项"这个数字的由来）。

> 低内存自适应：线程压力测试会按 `GetFreePages()` 预留 128 页并假设每线程约 12 页，空间不足时打印 `(low-memory: capping to N threads, 128 reserve)` 或 `(low-memory: skipping, only N pages free)` 后仍 `PASS`（`user/services/init/main.c:829-847`）。所以 `-m` 小于 256M 时套件可能"通过但未真正跑满 1000 线程"。

### 2.4 P1 权限引擎（10 项）

套件头 `=== P1 Permission Engine (live vfs+perm) ===`（`:1508`），运行前后用 `P1SetQuiet(1/0)` 抑制 Powerbox 面板弹出（查询与裁决语义不变，仅不推送 `UI_SHOW`，`:1493-1505`）。

| # | 测试标题字符串 | 判据要点 | 源码 |
|---|---|---|---|
| 1 | `subject identity: get_subject + vfs WHOAMI` | `GetSubject() == 1`；`vfs` 的 `VFS_OP_WHOAMI` 返回的 subject 必须等于 `GetSubject()`（内核填的发送者身份不可伪造） | :1194-1219 |
| 2 | `OWNER chain auto-allows WRITE on System (no Powerbox)` | `System/Kernel/init.elf` 的 WRITE 书签在 OWNER 链下直接放行，并捕获资源键 `g_p1_res` 供后续 GRANT/REVOKE 使用 | :1223-1236 |
| 3 | `ROLE_SET init→GUEST + live hot reload` | 管理面 `PERM_OP_ROLE_SET` 把 init 降为 GUEST 成功后，**同一个** WRITE 请求必须立刻变成 `VFS_ERR_ACCESS` | :1240-1265 |
| 4 | `GUEST chain DENY (READ) raises NO Powerbox prompt` | READ 被链 DENY 后 `PERM_OP_QUERY` 必须返回 `ERR_NOENT`（策略裁决不产生面板） | :1269-1290 |
| 5 | `default-deny → Powerbox → ANSWER allow → grant` | EXEC 走默认拒绝 → QUERY 返回 `PERM_QUERY_PENDING` 且 `label` 非空 → ANSWER allow → 重试成功 | :1294-1332 |
| 6 | `REVOKE drops grants → default deny again` | `PERM_OP_REVOKE` 撤销数 ≥ 1，EXEC 重新被拒；结尾主动 QUERY+ANSWER(deny) 清理面板，避免残留面板永久持有键盘焦点 | :1336-1378 |
| 7 | `GRANT beats role default (GUEST denies READ)` | 显式 GRANT 必须压过 GUEST 的 READ DENY 链 | :1382-1408 |
| 8 | `ROLE_SET denied for non-management (GUEST)` | 已降级为 GUEST 的调用者自我提权返回 `ERR_DENIED` | :1412-1430 |
| 9 | `DUMP exports role map + rule table` | `role_count >= 2`、`rule_count >= 1`，且导出文本里能 `strstr("subject=1 role=4")`（GUEST=4）并存在 `rule:` 行 | :1434-1463 |
| 10 | `CapGrantToSubject(self + unknown subject)` | 对自己签发原子后 `CapConsume` 成功；未知 subject（`0x7FFFFFFF`）返回 `ERR_NOENT` | :1467-1483 |

### 2.5 P2 敏感 syscall 门控（5 项）

套件头 `=== P2 Sensitive Syscall Gate ===`（`:1645`）。这一层是**纯内核能力表查询，零 IPC**（`:1524-1531`）。

| # | 测试标题字符串 | 判据要点 | 源码 |
|---|---|---|---|
| 1 | `set_time unauthorized -> ERR_NOCAP` | 未持 `ATOM_SYS_SET_TIME` 时 `OsSetTime()` 必须在触碰用户内存/CMOS 之前返回 `ERR_NOCAP` | :1563-1579 |
| 2 | `set_time authorized (ATOM_SYS_SET_TIME cap) -> 0` | `CapCreateAtom(ATOM_SYS_SET_TIME,...)` 后同一调用返回 0 | :1583-1602 |
| 3 | `reboot unauthorized -> ERR_NOCAP` | 无 `ATOM_SYS_SHUTDOWN` 时 `sys_reboot()` 返回 `ERR_NOCAP`；若门控位置错误，VM 会在这里复位（该行永不打印） | :1606-1616 |
| 4 | `notify foreign/nonexistent thread -> ERR_NOENT` | 目标线程不在本进程内返回 `ERR_NOENT`（外部与不存在同码，不泄漏存在性） | :1620-1628 |
| 5 | `debug_getchar unauthorized -> ERR_NOCAP` | 未持 COM1 IO 端口能力时读控制台返回 `ERR_NOCAP` | :1632-1640 |

### 2.6 P2 VFS 授权（4 项）

套件头 `=== P2 VFS Authorization (vfs+perm) ===`（`:2086`）。进入本套件前的状态是 P1 留下的：init 已是 GUEST，且持有 `(subject 1, g_p1_res, READ)` 授权。协议结构体很大（`path[1024]`、`data[4032]`、`policy[3584]`），全部使用静态暂存区，避免 init 单页栈溢出（`:1697-1730`）。

| # | 测试标题字符串 | 判据要点 | 源码 |
|---|---|---|---|
| 1 | `OPEN: unauthorized WRITE denied, P1 grant READ ok` | 未授权 `Users/p2vfs.bin` 的 CREATE\|WRITE → `VFS_ERR_ACCESS`；已授权 `init.elf` 的 READ → 成功且拿到非 0 句柄，随后 `VFS_OP_READ` 必须返回 > 0 | :1751-1786 |
| 2 | `抹位: READ\|EXEC open yields READ-only handle` | 只持 READ 时请求 READ\|EXEC → 打开成功（部分交集 beat），但句柄**只带** READ：READ 成功、WRITE 返回 `VFS_ERR_PERM` | :1790-1849 |
| 3 | `P3/P4 ifaces: context, freq, policy round-trip, audit` | CONTEXT upsert 成功；FREQ 计数 ≥1、reset 后归零；POLICY `SAVE→LOAD→SAVE` 字节完全一致；AUDIT 对 subject 1 同时存在 granted 与 denied 记录 | :1854-1947 |
| 4 | `5-op gates + enum lifecycle (grant → iterate → revoke)` | `create_dir`/`delete`/`enum_begin`/`move` 四个未授权 op 全部 `VFS_ERR_ACCESS`；对 `System/Kernel` 目录 GRANT READ 后 `enum_begin` + `enum_next` 成功；REVOKE 后下一次 `enum_next` 必须重新被拒（枚举器逐批重检） | :1951-2081 |

### 2.7 KBD 焦点（1 项）

套件头 `=== KBD Focus (live keyboard service) ===`（`:2204`），直接对 `keyboard` 端口发 `KBD_OP_TAKE_FOCUS(3)` / `KBD_OP_RELEASE_FOCUS(4)`（本地镜像常量定义在 `:2145-2146`，与 `user/services/keyboard/keyboard.c:130-131` 一致）。

| # | 测试标题字符串 | 判据要点 | 源码 |
|---|---|---|---|
| 1 | `TAKE_FOCUS/RELEASE_FOCUS ownership` | TAKE 成功 → 同一持有者重复 TAKE 幂等成功 → 持有者 RELEASE 成功 → 非持有者 RELEASE 返回 `ERR_NOCAP` | :2158-2201 |

> 覆盖分工：**所有权协议**在 init 内确定性验证；**物理按键路由**（持有焦点时按键到达谁）无法在 init 内注入扫描码，改由 QEMU 侧场景覆盖（`user/services/init/main.c:2094-2113`）。

### 2.8 P3 崩溃恢复（1 项）

套件头 `=== P3 Crash Recovery (service auto-restart) ===`（`:2282`）。

| # | 测试标题字符串 | 判据要点 | 源码 |
|---|---|---|---|
| 1 | `kill pkg -> manager auto-restart (port returns)` | `ProcessList` 找到 `pkg` 且端口存在 → `Kill(pid, SIGKILL)` 返回 0 → 管理器 `service_monitor` 重新 spawn，`PortGet("pkg")` 在 400 次 yield 内重新可用 | :2257-2279 |

依赖链：`process_reap()` → `ipc_cleanup_process()` 释放死进程的注册名/端口 → 管理器观察者重新 spawn（源码注释 `:2209-2214`）。**该测试要求 `pkg` 已经在运行**，因此需要管理器已完成到 5e 阶段的 spawn；在没有 virtio-blk 设备的环境里，管理器会在块设备端口上等满 2000 tick（`user/services/manager/manager.c:597-611`），`pkg` 尚未 spawn，本项即失败（详见 §7.3）。

### 2.9 P4 资源耗尽（2 项）

套件头 `=== P4 Resource Exhaustion ===`（`:2376`）。所有占用的资源在返回前全部释放。

| # | 测试标题字符串 | 判据要点 | 源码 |
|---|---|---|---|
| 1 | `IPC message > MAX_MSG_SIZE -> ERR_INVAL` | 发送 8192 字节（`MAX_MSG_SIZE = 4096`，`kernel/include/kernel/types.h:125`；边界检查 `kernel/ipc/ipc.c:393`）必须返回 `ERR_INVAL`，在入队/阻塞之前被拒 | :2332-2342 |
| 2 | `thread table exhaustion -> ERR_NOMEM, recoverable` | 连续 `ThreadCreate` 直到失败，失败码必须是 `ERR_NOMEM`，成功创建数 ≥ 2000（`MAX_THREADS = 2048`，`kernel/include/kernel/types.h:116`）；全部 join 后新线程可再创建 | :2349-2373 |

### 2.10 P5 零拷贝读（1 项）

套件头 `=== P5 Zero-Copy Read ===`（`:2479`）。

| # | 测试标题字符串 | 判据要点 | 源码 |
|---|---|---|---|
| 1 | `zero-copy READ_MAP: pool-backed blob matches chunked read` | 幂等 GRANT READ → `FsOpenItem("/Volumes/System/Kernel/init.elf")` → `vspace_alloc(0x100000)` → `FsReadMap()` 返回 `msize > 4096` → 映射区头 256 B 与尾 256 B 必须与 `FsRead` 分块读逐字节一致 | :2414-2476 |

### 2.11 串口锚点速查（便于 grep）

| 用途 | 正则 | 出处 |
|---|---|---|
| 经典套件汇总 | `=== Results: 34/34 passed ===` | `user/services/init/main.c:1055` |
| P1 汇总 | `=== P1 Permissions: 10/10 passed ===` | :1521 |
| P2 门控汇总 | `=== P2 Gate: 5/5 passed ===` | :1651 |
| P2 VFS 汇总 | `=== P2 VFS: 4/4 passed ===` | :2091 |
| KBD 汇总 | `=== KBD Focus: 1/1 passed ===` | :2206 |
| P3 汇总 | `=== P3 Crash Recovery: 1/1 passed ===` | :2284 |
| P4 汇总 | `=== P4 Resource Exhaustion: 2/2 passed ===` | :2379 |
| P5 汇总 | `=== P5 Zero-Copy Read: 1/1 passed ===` | :2481 |
| 全局成功 | `init: ALL SELFTESTS PASSED (34/34)` | :2580 |
| 进入空闲 | `init: entering idle loop` | :2581 |
| 失败横幅 | `!!! SELFTEST FAILURE: .* !!!` | :2495 |
| 失败停止 | `init: boot aborted after failed self-check` | :2496 |
| 定位失败项 | `  TEST: .* FAIL:` | `FAIL` 宏 :72-75、P4/P5 变参宏 :2314、:2404 |
| shell 进程创建 | `proc: CREATE pid=\d+ name=shell` | `kernel/process/process.c:259` |
| 服务启动（通用） | `manager: .* started \(PID=\d+\)` | `user/services/manager/manager.c:424` |
| 重启策略触发 | `manager: flaky marked FAILED` | `user/services/manager/manager.c:453` |
| 串口自检 | `serial-test: PASS` | `user/services/manager/manager.c:381` |
| 栈金丝雀 | `STACK SMASHING DETECTED: user stack canary mismatch` | `user/runtime/stack_chk.c:67` |
| hello 信号自检 | `hello: signal self-test PASSED` | `user/services/hello/main.c:105` |

## 三、fail-fast 机制

### 3.1 `BootSelftestFail` 的行为与输出格式

```c
static void BootSelftestFail(const char *suite, int passed, int ran) {
    printf("\n!!! SELFTEST FAILURE: %s %d/%d passed !!!\n", suite, passed, ran);
    printf("init: boot aborted after failed self-check\n");
    for (;;)
        ThreadYield();
}
```

（`user/services/init/main.c:2494-2499`）

| 观察点 | 事实 |
|---|---|
| 输出格式 | `!!! SELFTEST FAILURE: <套件名> <通过数>/<总数> passed !!!`，随后一行 `init: boot aborted after failed self-check` |
| 停机方式 | 无限 `ThreadYield()` 循环；init **不会**退出、不会打印 `ALL SELFTESTS PASSED`、也不会进入 `init: entering idle loop` |
| 内核与已启动服务 | 仍然存活并继续运行（可继续用串口观测、可 `screendump`），只是 init 不再推进后续阶段（源码注释 :2488-2493） |
| 横幅不含失败项名 | 横幅只有套件名与计数；定位具体失败项必须 grep 该套件跑出的 `TEST: <name> ... FAIL: <msg>` 行 |
| 触发点 | 每个套件跑完立即判定，共 8 处（见 §2.2）；经典套件在最后判定 |

示例（假设 P2 门控有 1 项失败）：

```text
=== P2 Sensitive Syscall Gate ===
  P2: set_time unauthorized -> ERR_NOCAP ... FAIL: unauthorized set_time accepted
  P2: set_time authorized (ATOM_SYS_SET_TIME cap) -> 0 ... (handle=9, ret=0) PASS

=== P2 Gate: 4/5 passed ===

!!! SELFTEST FAILURE: P2 syscall gate 4/5 passed !!!
init: boot aborted after failed self-check
```

### 3.2 启动阶段与自检的时间线

```text
kernel_main ──► Ring 3 init (PID 1)
                 │
                 ├─ RunTests()                 classic 34 项   ← 此时服务尚未 spawn
                 ├─ InitProtocol()            查询内核状态 / 注册 "init" 端口 / 演示线程
                 ├─ BlobGet("manager") + ProcessCreate("manager")
                 │      └─ manager 依次 spawn serial/term/keyboard/.../pkg/user/wm/policy/gui/net
                 ├─ RunP1PermTests()   ── 失败 ⇒ BootSelftestFail("P1 permission engine", ..)
                 ├─ RunP2GateTests()   ── 失败 ⇒ BootSelftestFail("P2 syscall gate", ..)
                 ├─ RunP2VfsTests()    ── 失败 ⇒ BootSelftestFail("P2 VFS authorization", ..)
                 ├─ RunKbdFocusTests() ── 失败 ⇒ BootSelftestFail("KBD focus", ..)
                 ├─ RunCrashRecoveryTests()     ── 失败 ⇒ ... "P3 crash recovery"
                 ├─ RunResourceExhaustionTests() ── 失败 ⇒ ... "P4 resource exhaustion"
                 ├─ RunZeroCopyTests() ── 失败 ⇒ ... "P5 zero-copy read"
                 ├─ classic 计数判定   ── 失败 ⇒ BootSelftestFail("classic syscall suite", ..)
                 ├─ printf("init: ALL SELFTESTS PASSED (34/34)")
                 └─ printf("init: entering idle loop") → for(;;) ThreadYield();
```

### 3.3 串口输出受内核令牌桶限流（会截断长输出）

用户态 `printf` 统一走 `SYS_DEBUG_LOG` → COM1（`user/lib/libc/stdio.c:24`、`:472`）。内核侧限制（`kernel/syscall/syscall.c:104-183`）：

| 常量 | 值 | 含义 |
|---|---|---|
| `DEBUG_LOG_MAX` | 512 | 单次 `debug_log` 最多拷贝/打印 512 字节（并按 UTF-8 字符边界回退） |
| `DEBUG_LOG_TICK_BUDGET` | 2048 | 每 tick 补充的预算字节数 |
| `DEBUG_LOG_BUCKET_MAX` | 4096 | 桶上限（突发额度） |

含义：一次 tick 内最多输出 4096 字节，超出的部分**静默丢弃**。这就是 `smoke_test.py` 对 P2/P2V/KBD 使用"摘要行 \| 首测试行"正则交替的原因（`scripts/smoke_test.py:349-366`）——历史回退时摘要行可能被截掉，而首个测试行一定会出现。

> 注意：`scripts/smoke_test.py:355-357` 的注释写的是 `DEBUG_LOG_TICK_BUDGET 512` + 桶上限 1024，与当前代码（2048 / 4096）不一致；数值以 `kernel/syscall/syscall.c:104-107` 为准。

## 四、第 2 层：宿主机脚本矩阵

### 4.1 公共运行模型

所有自动化脚本共享同一套机制（细节见 `scripts/smoke_test.py:76-98`）：

| 机制 | 实现 |
|---|---|
| QEMU 启动 | `qemu-system-x86_64 -cdrom <ISO> -m 256M` + 显示方式 + `-serial file:<log>` + `-monitor unix:<sock>,server=on,wait=off` |
| 命令注入 | 通过 monitor unix socket 发 `sendkey`（PS/2 扫描码），见 `scripts/smoke_test.py:101-126` |
| 屏幕取证 | `screendump <ppm>` → `parse_ppm` → `decode`（`tools/vga_decode.py`） |
| 串口取证 | 直接读 `-serial file:` 指定的文本日志（服务 `printf`） |
| 面板应答 | monitor 发 `sendkey y`，等待约 3 s（`scripts/smoke_test.py:267-274`） |
| 复位 | monitor 发 `system_reset`，用"日志偏移量之后匹配"检测新启动（`scripts/smoke_test.py:224-237`） |
| 清理 | `terminate()`（5 s 超时后 `kill`）+ 删除 socket/PPM（`scripts/smoke_test.py:319-329`） |

`sendkey` 语义要点：**一次 `sendkey` 调用里的所有键同时按下、调用结束时统一释放**，所以带 `shift` 的字符必须单独成一次调用，否则 shift 会"粘"到后续字符上（`scripts/accept.py:43-47` 有完整说明）。`smoke_test.py` 把连续无修饰键的字符合并成一次调用（`scripts/smoke_test.py:119-124`）。

### 4.2 脚本矩阵

| 脚本 | 用途 | 前置条件 | 命令行用法 | 主要观测锚点 | 产物路径 | 退出码 |
|---|---|---|---|---|---|---|
| `scripts/smoke_test.py` | R1 基线 + R2 盲区 + R3 压力三轮冒烟；`--drive` 追加磁盘持久化 | `make iso` 产出 `build/opsos.iso`；`--drive` 需 `disk.img` | `python3 scripts/smoke_test.py [--drive] [--step-timeout N]` | 15 条串口锚点 + 10 个 VGA 场景 + 6 个流程（见 §4.3） | 串口 `build/serial.log`；截图 `/tmp/opsys-screen.ppm`；socket `/tmp/opsys-mon.sock` | 0 = `=== SMOKE PASSED (R1 + R2 + R3) ===`；1 = `=== SMOKE FAILED: N failure(s) ===` |
| `scripts/accept.py` | Powerbox 书签授权 + MOVE 存活 + 撤销的 10 步验收 | 同上；**注意其锚点已陈旧（见 §4.10）** | `python3 scripts/accept.py` | `tee: 5 bytes written to /Users/a.txt`、`bm_create: FAILED (-105)`、`perm: ... perm_answer <id> y/n`、`bm_resolve: handle ...`、`move: 'b.txt' -> item N`、`perm_revoke: N grant(s) dropped` | 串口 `build/serial.log`；socket `/tmp/opsys-mon.sock`（**无 VGA/PPM**） | 0 = `=== ACCEPTANCE PASSED ===`；失败立即 `sys.exit(1)` |
| `scripts/verify_wm.py` | v0.4 文本窗口管理器（wm 服务 + wm_demo 桌面） | `make iso`；`disk.img`（脚本无条件附加 virtio-blk） | `python3 scripts/verify_wm.py` | `wm: port \d+ registered as 'wm'`、`[wm-demo] desktop session started`、`wm: desktop session active`、`wm: desktop session closed, focus released`、`[wm-demo] session ended, windows destroyed, exiting`；VGA `Terminal`/`Files`/`Settings` 与 `* Terminal` 焦点标记 | 串口 `build/wm-serial.log`；截图 `build/wm-screen.ppm`；socket `build/wm-mon.sock` | 0 = `ALL PASS`；1 = `SOME FAILED` |
| `scripts/verify_window_demo.py` | v0.4 最小窗口闭环（3 窗口 + 焦点键 + q 退出） | 同上 | `python3 scripts/verify_window_demo.py` | `[window-demo] desktop rendered, focus=1`、`[window-demo] focus=2`、`[window-demo] quit`；VGA `Window 1..3`、`Focus = Window 1/2` | `build/wd-serial.log`、`build/wd-screen.ppm`、`build/wd-mon.sock` | 0/1（同 `ALL PASS`/`SOME FAILED`） |
| `scripts/verify_users.py` | 账户与退出保护（login/whoami/useradd/users/logout/stop 越权） | 同上 | `python3 scripts/verify_users.py` | `login: ok - 'admin' (OWNER)`、`useradd: ok - 'bob' (STANDARD)`、`Confirm Stop`、`Admin password`、`stop: FAILED (-9)`、`stop: 'pkg' (PID N) stopped`；**其中 users 相关锚点已陈旧（§4.10）** | `build/usr-serial.log`、`build/usr-screen.ppm`、`build/usr-mon.sock` | 0/1 |
| `scripts/verify_demos.py` | hello / runtime_demo / tui_demo / window_demo 四个 blob 的分项验证 | 同上 | `python3 scripts/verify_demos.py` | `hello: signal self-test PASSED`、`proc: LAST_THREAD pid=N code=143`、`[runtime_demo] Resource constructor called`、`atexit handler 3 (counter=1)`、`[runtime_demo] Resource destructor called`、`buf2: still contains 'Hello from malloc!'`、`[tui-demo] Terminal port: N`、`[tui-demo] Demo rendering complete`、`[tui-demo] Demo finished`、`[window-demo] desktop rendered, focus=1`；**其 `[shell] cmd_exec` 锚点已陈旧（§4.10）** | `build/demo-serial.log`（留在工作区，便于事后取证）、`build/demo-screen.ppm`、socket `/tmp/opsys-demo-mon.sock` | 0/1，并打印逐项 SUMMARY |
| `scripts/ops_pack.py` | `.ops` 包宿主侧打包/校验（与 pkg-manager 解析规则互认） | 无（纯 host 工具，不需要 QEMU） | `python3 scripts/ops_pack.py pack <elf> <manifest.txt> <out.ops>`；`python3 scripts/ops_pack.py check <out.ops>` | 无（不涉及 QEMU） | 由 `pack` 生成的 `<out.ops>` | 0 = 成功；1 = 格式/manifest 违规（错误写到 stderr） |
| `scripts/verify_tools.py` | **v0.9 工具链端到端验证**（32 项）：自带 PCnet 网卡与 virtio-blk 磁盘启动 QEMU，登录 admin 后依次运行磁盘/文件/电源/服务/网络命令，再做主机侧往返（脚本在本机起 Python HTTP 服务与 UDP 监听，客户机用 `http 10.0.2.2 8088 /` 与 `udp send ...` 访问），最后用 `power halt -f` 验证 `SYS_HALT` 落到内核。注意：它自身占用 8088/9999 端口 | **v1.0 追加**：`perm audit/ctx/freq/save/load/permguard` 六项权限检查。
| `tools/vga_decode.py` | 把 `screendump` 的 PPM 解码为 113×38 文本网格（也作为库被上述脚本 import） | 一个 PPM 文件 + `user/services/term/font.h` | `python3 tools/vga_decode.py <screen.ppm> [--font <font.h>]` | 无 | stdout（113 行文本） | 0/1/2（缺参数打印 docstring 并 `exit(2)`） |

### 4.3 `scripts/smoke_test.py` 详解

常量与启动（`scripts/smoke_test.py:44-58`、`:642-661`）：

| 常量 | 值 | 说明 |
|---|---|---|
| `ISO` | `build/opsos.iso` | 脚本**不会**自动重建，改代码后必须先 `make iso` |
| `SERIAL_LOG` | `build/serial.log` | 启动时删除；退出时**保留**（取证用） |
| `MON_SOCK` | `/tmp/opsys-mon.sock` | 退出时删除 |
| `SCREEN_PPM` | `/tmp/opsys-screen.ppm` | 退出时删除 |
| `DISK` | `disk.img` | 仅 `--drive` 时附加 |
| `FONT_H` | `user/services/term/font.h` | 字形表路径（硬编码） |
| `BOOT_TIMEOUT` | 180 s | 等待"shell 进程创建 + VGA 提示符" |
| `STEP_TIMEOUT` | 40 s | 可用 `--step-timeout N` 覆盖 |
| `VNC_ADDR` | `127.0.0.1:0` | 无图形环境也可运行 |

三轮覆盖与流程（`main()`，`:680-711`）：

| 阶段 | 内容 | 实现 |
|---|---|---|
| 启动门 | 串口 `proc: CREATE pid=\d+ name=shell` → VGA `opsys:[^$]*\$` → `answer_panels()` | :663-678 |
| R1 串口锚点 | 15 条固定锚点，每条等待 **20 s**（不受 `--step-timeout` 影响） | `SERIAL_ANCHORS` :349-383、循环 :682-687 |
| R1 VGA 场景 | 10 个 shell 场景（`free`/`ports`/`threads`/`uptime`/`ps`/`ls`/`ls root`/`tee`/`cat`/`stat`），每条用 `STEP_TIMEOUT` | `VGA_SCENARIOS` :333-346、循环 :689-697 |
| R1.7 Powerbox 书签流 | 清理残留授权 → `bm_create` 授权前 -105 → 应答面板 → 创建/解析/move/再解析/撤销/再解析失败 | `run_powerbox_flow()` :386-424 |
| R1.8 pkg 流 | `pkg install/list/run hello` + 串口等 `hello: signal self-test PASSED` | `run_pkg_flow()` :427-446 |
| R1.9 pkg 沙盒 | 同一 ELF 装两次（带 `sys.set_time` 与不带），断言 `set_time OK` / `set_time DENIED (no permission)` 与自授 `self-grant attempt = -3` | `run_pkg_sandbox_flow()` :449-498 |
| R2 盲区 | `bm_revoke`、`bm_resolve` 无缓存、`mkdir`/`rm` 往返、`pkg remove` 与二次 remove 错误路径、`kill 99999` 错误路径 | `run_round2_flow()` :501-535 |
| R3.2 压力 | `fallocate /Users/big.bin` 打满 32 MiB Users 卷 → `NOSPC at N MiB` → `uptime` 证明存活 | `run_round3_flow()` :556-565 |
| R3.3 并发 | 连续 3 次 `exec`，用串口计数断言 `hello: signal self-test PASSED` 出现 baseline+3 次，再用 `ps` 断言 hello 已消失 | :567-589 |
| R3.4 键盘洪水 | `flood_command()` 以 ~5 ms/键注入 `stat /Volumes/Users`，断言回显完整 + 结果出现 | `flood_command` :129-158、断言 :591-599 |
| R2.7/R3.5 持久化（仅 `--drive`） | `tee /Volumes/Disk/persist.txt hello` → `cat` → `system_reset` → 重新等 shell 提示符 → 再 `cat` 仍是 5 字节 | `run_disk_persist_flow()` :603-639 |

`--drive` 追加的 QEMU 参数（`:657-659`）：

```bash
-drive file=disk.img,if=none,id=vd,cache=writethrough \
-device virtio-blk-pci,drive=vd,disable-modern=on
```

`cache=writethrough` 是"复位后仍能读到数据"的前提；`disable-modern=on` 让驱动走 legacy virtio 路径（与 `user/services/vfs/fs_virtio_blk_driver.c` 的实现匹配）。

### 4.4 `scripts/accept.py` 详解（10 步）

流程定义在文件头 docstring（`scripts/accept.py:15-31`）与 `main()`（`:188-247`）：

| 步 | 命令 | 期望锚点 |
|---|---|---|
| 1 | `tee /Users/a.txt hello` | `tee: 5 bytes written to /Users/a\.txt` |
| 2 | `bm_create /Users/a.txt r` | `bm_create: FAILED \(-105\)`（授权前 EACCES） |
| 3 | （不发命令）解析串口 `perm.ui` 提示行 | `perm: app 0x\w+ requests /Users/a\.txt \(R\) - perm_answer (\d+) y/n` → 取 query id |
| 4 | `perm_answer <id> y` | `perm_answer: query <id> -> ALLOWED \(0\)` |
| 5 | `bm_create /Users/a.txt r` | `bm_create: ok, \d+-byte bookmark cached` |
| 6 | `bm_resolve` | `bm_resolve: handle -?\d+, item 'a\.txt' \(id (\d+)\), access \d+` → 记录 item id |
| 7 | `move /Users/a.txt /Users b.txt` | `move: 'b\.txt' -> item \d+` |
| 8 | `bm_resolve` | 仍返回句柄，且 item id 必须与第 6 步**相同**（移动不换 itemID，脚本内比较，:233-237） |
| 9 | `perm_revoke` | `perm_revoke: \d+ grant\(s\) dropped` |
| 10 | `bm_resolve` | `bm_resolve: FAILED \(-105\)`（撤销后再次 EACCES） |

与 `smoke_test.py` 的差异：`accept.py` 用 `-display none`（无 VNC、不截图），全部依赖串口文本；把 `BOOT_TIMEOUT`/`STEP_TIMEOUT` 直接硬编码为 180/40（`:39-40`）。**但当前 HEAD 上 shell 输出不在串口，其第 0 步与第 1 步锚点都不可能出现**，详见 §4.10。

### 4.5 `verify_wm.py` / `verify_window_demo.py`

| 项 | `verify_wm.py` | `verify_window_demo.py` |
|---|---|---|
| 被测对象 | `wm` 服务 + `wm_demo` 桌面（libwm 3 窗口） | `window_demo` 最小闭环 |
| 启动锚点 | 串口 `init: idle loop`（180 s，**陈旧**，见 §4.10） | 同左 |
| 启动后 | `sleep 6` → 应答启动面板 → `sleep 2` | `sleep 8` → 应答启动面板 → `sleep 2` |
| 检查项 | wm 服务注册、桌面会话开始、三窗口渲染（VGA `Terminal`/`Files`/`Settings`）、新窗口默认聚焦（VGA `* Settings`）、`1` 切到 `* Terminal`、`2` 切到 `* Files`、聚焦窗口接受移动键（串口 `wm: desktop session active`）、`q` 退出会话并恢复 shell（VGA `wm-ok`） | 三窗口 + 状态行渲染、串口 `focus=1`、`2` 后 VGA `Focus = Window 2` + 串口 `focus=2`、`q` 后串口 `quit` |
| 崩溃检测 | 日志中出现 `KERNEL PANIC` / `Triple fault` 即判失败（:237-242） | 同左（:186-190） |
| 提示符检测 | `opsys:[^$]*\$`（v0.6 的 `opsys:<cwd>\$` 形式） | 无 |
| 结束语 | `ALL PASS` / `SOME FAILED` | 同左 |

两个脚本都**无条件**把 `disk.img` 作为 virtio-blk 附加（`:157-164` / `:140-147`），因此不会遇到"缺盘导致管理器等待"的问题（§7.3）。

### 4.6 `verify_users.py`

启动后按 docstring（`:15-24`）执行 8 项检查：

| # | 命令 | 期望（VGA 文本） | 源码行 |
|---|---|---|---|
| 1 | `login admin admin` | `login: ok - 'admin' (OWNER)` | :188-190 |
| 2 | `whoami` | `admin (OWNER)` | :192-194 |
| 3 | `useradd bob standard bobpw` | `useradd: ok - 'bob' (STANDARD)` | :196-200 |
| 4 | `users` | `Accounts (2):` 且 `bob 2` | :202-204（**陈旧**，§4.10） |
| 5 | `logout` + `whoami` | `logout: ok`；随后 `not logged in` | :206-211 |
| 6 | `login bob bobpw` + `stop pkg` + `y` + `bobpw` | `Confirm Stop` → `Admin password` → `stop: FAILED (-9)`（STANDARD 越权被拒） | :213-223 |
| 7 | `logout` + `login admin admin` | `login: ok - 'admin' (OWNER)` | :225-229 |
| 8 | `stop pkg` + `y` + `admin` | `stop: 'pkg' (PID N) stopped` | :230-236 |

该脚本的按键注入是**逐字符独立 `sendkey` + 0.15 s 间隔**（`:77-93`），原因是对话框会在字符之间重新 park 键盘，合并发送会与键盘服务的 park 路由竞争（脚本内注释 :79-81）。

### 4.7 `verify_demos.py`

`exec_and_wait(name, anchors, labels)`（`:191-210`）的结构值得注意：它先发 `exec <name>`，确认"命令被接受"，再逐条等该 demo 自己的串口锚点；失败时会重发（最多 3 次）并 `screendump` 存证。

| demo | 锚点 | 验证意图 |
|---|---|---|
| `hello` | `hello: signal self-test PASSED`、`proc: LAST_THREAD pid=\d+ code=143` | 信号迁移端到端（SIGTERM 默认动作退出码 143 = 128+15） |
| `runtime_demo` | `[runtime_demo] Resource constructor called`、`atexit handler 3 (counter=1)`、`[runtime_demo] Resource destructor called`、`buf2: still contains 'Hello from malloc!'` | 全局构造先于 main、atexit 逆序、`.fini_array` 析构、realloc 就地扩展 |
| `tui_demo` | `[tui-demo] Terminal port: \d+`、`[tui-demo] Demo rendering complete`、`[tui-demo] Demo finished` | TUI 渲染路径 |
| `window_demo` | `[window-demo] desktop rendered, focus=1` | 仅验证 spawn 与渲染（交互焦点/退出由 `verify_window_demo.py` 覆盖） |

### 4.8 `scripts/ops_pack.py`（host 侧 `.ops` 打包/校验）

与 OS 内部实现无关，但属于"测试工具链"的一部分：pkg-manager 安装的 `.ops` 由它打包，验收标准要求双向互认（`docs/ops_format.md:218-229`）。

| 项 | 值 | 出处 |
|---|---|---|
| 二进制布局 | 16 B 头（magic/version/manifest_len/payload_len 小端）+ manifest + ELF payload | `scripts/ops_pack.py:27-35`、`docs/ops_format.md:16-26` |
| `MAGIC` | `0x3153504F`（`"OPS1"` 小端） | :46 |
| `VERSION` | 1 | :47 |
| `MANIFEST_MAX` | 512 | :48 |
| `HEADER_LEN` | 16 | :49 |
| 合法 manifest 键 | `app_id`（必填，`[a-zA-Z0-9_]{1,63}`）、`app_name`、`version`、`entry`、`permissions` | :52-54 |
| 封闭原子表 | 19 项（`sys.set_time` … `pkg.install`） | :57-66 |
| 禁授原子 | `service.manage`、`cap.grant_self`、`sys.debug` | :68 |
| 校验内容 | magic/version/manifest_len/payload_len、`16+mlen+plen == 文件大小`、行宽 ≤100、未知键/重复键、payload 必须 `\x7fELF` | :158-206 |

```bash
python3 scripts/ops_pack.py pack build/user/services/hello.elf hello.manifest hello.ops
python3 scripts/ops_pack.py check hello.ops
```

### 4.9 `tools/vga_decode.py`

| 项 | 值 | 出处 |
|---|---|---|
| 单元尺寸 | `CELL_W=9`、`CELL_H=20`；字形 `8x16` | `tools/vga_decode.py:33-34` |
| 亮度阈值 | `LUM_THRESH=400`（白 765 vs 深蓝 144） | :35 |
| 匹配容错 | `MAX_DIST=8`（汉明距离，抗锯齿/闪烁） | :36 |
| 字体解析 | 正则定位 `s_font[95][16] = {`，**按块顺序**映射到 `0x20+i`（不依赖注释里的字符字面量，避免转义引号被误解析） | :39-55，字体表 `user/services/term/font.h:26` |
| PPM 解析 | 仅接受 `P6`、maxval 255、像素数据足够长；否则 `ValueError` | :58-70 |
| 解码 | `cols = w // 9`、`rows = h // 20`；每格取 16×8 位图，正/反（光标）两种解释取更近者 | :73-112 |
| CLI | `--font` 可覆盖字体路径，默认 `user/services/term/font.h` | :115-129 |

1024×768 帧缓冲 → 113 × 38 单元（`1024//9 = 113`，`768//20 = 38`），与 `docs/ops_format.md:196-198` 的记录一致。

### 4.10 已在 HEAD 上核实为陈旧的脚本锚点

这些是**文档化事实**，不是猜测；本文只记录，不修改脚本（脚本改动不在本文范围内）。

| 脚本:行 | 锚点 | 实际代码 | 后果 |
|---|---|---|---|
| `scripts/accept.py:169`（`opsys\$`）、`:191`（`tee: ...`） | 期望 shell 输出与提示符出现在**串口** | shell 从不调用 `DebugLog`（`grep -c DebugLog user/services/shell/shell.c` = 0），输出经 `TERM_OP_WRITE` 交给 term（`user/services/shell/shell.c:125`、`:443-477`）；串口镜像默认编译排除（`user/services/term/term.c:1038`） | 第 0 步即 `BOOT FAILED`；脚本还用了 `-display none`，也不会截图 |
| `scripts/accept.py:199` | `perm: app 0x\w+ requests /Users/a\.txt \(R\) - perm_answer (\d+) y/n` | 当前 label 由 `BuildLabel()` 生成：`perm: <name> (PID n) 请求访问 <url> (R) — 输入 perm_answer <id> y/n`（`user/services/perm/perm-manager.c:699-717`） | 即使修好串口通道，第 3 步的 query id 也解析不到 |
| `scripts/verify_wm.py:172`、`verify_users.py:176`、`verify_demos.py:182`、`verify_window_demo.py:150` | 串口 `init: idle loop` | init 实际打印 `init: entering idle loop`（`user/services/init/main.c:2581`） | 该正则永不匹配 → 180 s 后 `BOOT TIMEOUT`，脚本退出 1 |
| `scripts/verify_demos.py:196` | `\[shell\] cmd_exec: pid=\d+ tick=\d+` | shell 的 `[shell] cmd_*` 调试行已被移除（`grep cmd_exec user/services/shell/shell.c` 无结果；该清理见 `docs/test_report.md` 第八轮补充 6） | "命令被接受"这一项必然 FAIL，并触发 3 次重发（每次 30 s）后继续跑后面的 demo 锚点 |
| `scripts/verify_users.py:203` | VGA `Accounts \(2\):` | `users` 现在走 TUI 菜单：空列表打印 `Accounts: (none)`（`shell.c:4151`），否则弹 `TuiMenu(..., "Accounts (Enter/q)", ...)`（`shell.c:4180`），菜单同步等待 Enter/q（`user/lib/libtui/tui.c:542-601`） | 该步 FAIL，且脚本未发送关闭键，后续输入可能被菜单吃掉 |
| `scripts/verify_demos.py:186`、`verify_window_demo.py:153` | 注释称"P2V 留下一张 init EXEC 待决面板" | 当前 init 的 P1 测试 5/6 会自行 ANSWER（`:1318-1331`、`:1355-1376`），P1 全程 `P1SetQuiet(1)` 抑制 UI_SHOW（`:1493-1505`），P2V 的拒绝路径都是**链裁决 DENY**（`user/services/perm/perm-manager.c:823-828`，不产生面板） | `answer_panels()` 通常什么都不做；若无面板，它只打印一行 `TIMEOUT[vga]`（`scripts/smoke_test.py:295-304`）或静默返回（`scripts/verify_demos.py:150-160`），属良性 |

> 结论：**当前 HEAD 上可直接用于回归的脚本是 `smoke_test.py`（含 `--drive`）**；`verify_*.py` 需先修正上表锚点，`accept.py` 需从串口模型迁移到 VGA 模型。它们记录的**流程与断言意图**仍然有效，本指南 §4.3-§4.7 即以其意图为准。

## 五、观测模型（关键）

### 5.1 双通道模型

```text
        ┌──────────── QEMU 客户机 ────────────┐
        │  Ring 3 服务 printf → SYS_DEBUG_LOG │──► COM1 ──► -serial file:build/serial.log
        │  shell 输出 → TERM_OP_WRITE → term  │
        │      → SYS_FB_MAP 线性帧缓冲         │──► VGA 1024x768x32 ──► monitor screendump ──► PPM ──► vga_decode ──► 文本
        │  Powerbox 面板 → perm.ui → term 渲染 │                （面板与 shell 共用同一帧缓冲，也只能从 VGA 读到）
        └─────────────────────────────────────┘
                     ▲                                    ▲
                     │ monitor sendkey（PS/2 扫描码）      │
                 宿主机脚本：串口锚点 + VGA 文本锚点 + PPM 像素取证
```

| 通道 | 承载内容 | 读取方式 | 陷阱 |
|---|---|---|---|
| 串口 | **只有**内核 `SerialPuts` 与用户态 `printf`/`DebugLog`（内核启动日志、服务日志、`proc:` 事件） | 读 `-serial file:` 日志文本 | 单次上限 512 B、每 tick 预算 2048 B/桶 4096 B（§3.3）；启动期内核直写 COM1 与 serial 服务写入器会竞争，偶发吃掉行首字节（`docs/test_report.md` 五.4） |
| VGA | shell 提示符/回显/命令输出、TUI 组件、Powerbox 面板、窗口管理器渲染、像素 GUI | `screendump` + `parse_ppm` + `decode`（或 PPM 像素采样） | 只能读到**当前屏**；面板/菜单会遮挡；解码依赖字体表与网格几何 |
| 键盘 | monitor `sendkey` 注入 PS/2 扫描码 → keyboard 服务解码 → 按焦点路由 | `sendkey` + 等待 | 面板/TUI 菜单持有焦点时键入会被它们吃掉；一次 `sendkey` 内的多键是"同时按下"语义 |

### 5.2 为什么 shell 输出不在串口

1. 用户态 `printf` 走 `DebugLog`（`user/lib/libc/stdio.c:24`、`:472`），写的是内核 `SYS_DEBUG_LOG` → COM1。
2. shell 不调用 `DebugLog`；它的每个字符都通过 IPC 交给 term：`TERM_OP_WRITE`（`user/services/shell/shell.c:125`）→ `s_term_port`（`:443-477`）。
3. term 把文本渲染进线性帧缓冲；可选的串口镜像被 `#ifdef TERM_DEBUG_SERIAL_MIRROR` 包住，且没有任何地方定义该宏（`user/services/term/term.c:1038-1057`）。
4. 结论：要看到 shell 的任何一行（包括错误码），只能截图 + 解码（`docs/ops_format.md:201-207`）。

### 5.3 VGA 解码管线要点

| 环节 | 事实 | 出处 |
|---|---|---|
| 客户机分辨率 | GRUB `set gfxmode=1024x768x32` + `gfxpayload=keep`；term 通过 `SYS_FB_MAP` 拿到线性帧缓冲 | `boot/grub.cfg:11-14`、`README.md:375` |
| 单元网格 | 8×16 字形排在 9×20 px 单元里（1 px 右间距 + 4 px 下间距） | `user/services/term/term.c:37`、`tools/vga_decode.py:15-27` |
| 屏幕尺寸 | 1024×768 → 113 列 × 38 行 | `tools/vga_decode.py:101-112` |
| PPM 大小 | 1024×768×3 + 头 ≈ 2.36 MB；脚本按"文件大小连续两次相同且 ≥ 1 MiB"判断写盘完成 | `scripts/smoke_test.py:176-186` |
| 颜色约定 | 字形像素白（765）、背景深蓝（144），阈值 400；光标格反色 | `tools/vga_decode.py:20-21`、`:35` |
| 字体表 | `parse_font` 只认 `s_font[95][16] = {`（`user/services/term/font.h:26`），共 95 个 ASCII 字形；CJK 走 `user/lib/font_cjk.h`（16×16），**解码器不认识 CJK**（会落成空白或误配） | `tools/vga_decode.py:39-55`、`user/services/term/term.c:109`、`:434` |
| 面板标签含非 ASCII | Powerbox label 里的中文与 em-dash 在 VGA 上按 ASCII 字体渲染（非 ASCII 字符不显示），所以脚本锚定 `Allow? (y/n)`、`Result: ALLOWED`/`DENIED` 这类纯 ASCII 行 | `user/services/perm/perm-manager.c:699-717`、`user/services/term/term.c:1782-1811` |

### 5.4 Powerbox 面板抢键盘焦点：原因与"输入-应答-重输"循环

**原因（代码事实）**：键盘服务只有单一焦点持有者 `s_focus_owner`；持有焦点时，解码后的按键**只**路由给焦点持有者的 park 槽（`user/services/keyboard/keyboard.c:315-336`），而 park 表只有 4 个槽（`KBD_PARK_MAX = 4`，`:135`）。term 的 Powerbox UI 线程在显示面板前执行 `TAKE_FOCUS`（`KBD_OP_TAKE_FOCUS = 3`，`user/services/term/term.c:1625-1626`、`:1904`），面板消失（裁决到达）后再 `RELEASE_FOCUS`。因此在面板可见期间：

- shell 根本没在收键（它的 park 槽不是焦点持有者），键入的字符丢失或落进面板的 `y/n` 读取路径；
- 被权限拦截的命令早已返回 `-105 (EACCES)` 并打印错误（默认拒绝路径 `user/services/perm/perm-manager.c:830-844`），**授权不会自动重试**，必须在应答面板后重新输入同一条命令。

**脚本的循环模式**（`scripts/smoke_test.py:240-278`）：

```text
for attempt in 0..4:                     # retries=4
    type_command(text)                   # 逐组 sendkey + ret
    deadline = now + min(timeout, 15s)
    while now < deadline:
        txt = vga_joined()               # screendump + 解码
        if re.search(expected, txt):  return True          # 成功
        if "Type y to confirm" in txt:                     # TUI 确认框：命令仍在运行
            sendkey y; sleep 2; continue                   #   → 应答后继续等，不重输
        if "Allow? (y/n)" in txt:                          # Powerbox 面板：命令已失败返回
            sendkey y; sleep 3; break                      #   → 应答后退出内层，重输命令
        sleep 0.5
# 超时：打印 TIMEOUT[vga] + dump_screen()（当前 VGA 屏），返回 False
```

**时序图（授权前 → 面板 → 授权后重试成功）**：

```text
t0  脚本: sendkey "tee /Users/a.txt hello" + ret
t1  shell 执行 → vfs → perm CHECK → 默认拒绝（无 grant、无链规则）
t2  perm 建立 PENDING 查询 + UI_SHOW 推送到 term("perm.ui")
t3  shell 打印 "tee: FAILED (-105) (EACCES)"（VGA），命令结束
t4  term UI 线程 TAKE_FOCUS 并渲染面板：label + "Access: W" + "Allow? (y/n)"
      ── 此刻 shell 不再收到任何按键 ──
t5  脚本解码到 "Allow? (y/n)" → sendkey y → term 回复 perm ANSWER(allow)
t6  perm: grant upsert + 把裁决编码成原子能力签发给调用者；面板渲染 "Result: ALLOWED"
t7  面板 restore + RELEASE_FOCUS（约 3 s 的等待覆盖 verdict hold）
t8  脚本**重新** sendkey "tee /Users/a.txt hello" + ret → 这次 grant 命中 → 成功打印
```

启动期有一个例外说明：init 的 P1 套件用 `PERM_OP_SET_QUIET` 抑制面板（`user/services/init/main.c:1487-1505`），所以**正常情况下开机后不应看到权限面板**；脚本里的 `answer_panels()` 是防御性调用（若真出现面板，它会连按 y 直到没有面板或到达轮数上限）。

### 5.5 Powerbox 面板与 TUI 确认框必须区分

| 特征 | Powerbox 面板（授权） | TUI 确认框（破坏性操作） |
|---|---|---|
| 触发 | `perm` 的默认拒绝路径（无 grant、无链规则） | `rm`/`mv`/`fm`/`stop` 等命令内部 |
| 屏上文本 | `Access: <掩码>` + label + `Allow? (y/n)`，裁决后变 `Result: ALLOWED`/`DENIED` | `Type y to confirm` / `y = delete, n = cancel` / `Confirm Stop` |
| 命令状态 | 已经返回错误（`-105`），**必须重输** | 仍在阻塞等待，**不能重输**（否则会排队第二次调用） |
| 脚本动作 | `sendkey y` → `sleep 3` → `break`（重输） | `sendkey y` → `sleep 2` → 继续轮询 |
| 出处 | `scripts/smoke_test.py:254-275` | 同左；`user/services/shell/shell.c:4200`、`user/services/term/term.c:1803` |

## 六、如何新增一个测试

### 6.1 在 init 里加断言

最小模板（放进 `user/services/init/main.c`，并在对应套件的 runner 里调用）：

```c
/* 说明：测试标题会成为串口 anchor，请保持稳定、唯一、ASCII */
static void TestP2vNewGate(void) {
    P2V_TEST("NEW_OP: unauthorized denied, granted ok");  /* 打印 "  P2V: <标题> ... " 并计数 */
    int vfs_port = P1PortGet("vfs");                      /* 服务端口用轮询，避免启动竞态 */
    P2V_ASSERT(vfs_port > 0, "vfs port unavailable");

    /* 大结构体必须 static：init 用户栈只有 1 页（main.c:821 注释） */
    static vfs_req_open_t  req;
    static vfs_resp_open_t resp;
    memset(&req, 0, sizeof(req));
    req.op = VFS_OP_OPEN_ITEM;
    strncpy(req.path, P1_ITEM_PATH, sizeof(req.path) - 1);

    int rlen = (int)sizeof(resp);
    P2V_ASSERT(IpcCall(vfs_port, &req, (int)sizeof(req), &resp, &rlen) == 0,
               "transport failed");
    printf("(ret=%d) ", resp.ret);                        /* 可选：把关键数值打进日志 */
    P2V_ASSERT(resp.ret == VFS_ERR_ACCESS, "unauthorized op allowed");  /* 失败即 return */
    P2V_PASS();
}

static void RunP2VfsTests(void) {
    printf("\n=== P2 VFS Authorization (vfs+perm) ===\n");
    TestP2vOpenGate();
    TestP2vCapabilityTrim();
    TestP2vP3p4Ifaces();
    TestP2vOpsGate();
    TestP2vNewGate();                    /* ← 新增 */
    printf("=== P2 VFS: %d/%d passed ===\n", p2v_pass, p2v_run);
}
```

要点清单：

| 要点 | 说明 |
|---|---|
| 用对宏前缀 | 经典套件 `TEST/PASS/FAIL/ASSERT`；P1 `P1_*`；P2 `P2_*`；P2V `P2V_*`；KBD `KBD_*`；P3 `P3_*`；P4 `P4_*`（变参）；P5 `P5_*`（变参）。跨套件复用会污染计数 |
| 断言失败即返回 | `ASSERT` 直接 `return`，因此一个测试项只可能"通过了才 PASS"；不要写成 `FAIL()` 之后继续往下跑（那样会同时打印 FAIL 与 PASS，计数虚高） |
| 大对象放 static | init 栈只有 1 页；P2V 的协议结构体（`path[1024]`、`data[4032]`…）全部用文件级 static 暂存（`:1697-1730`），P3 的 `proc_info_t list[64]` 亦然（`:2244-2246`） |
| 等端口要轮询 | 服务在管理器 spawn 后并行启动，统一用 `P1PortGet()`（200 × `Sleep(10)`，`:1143-1151`）而不是裸 `PortGet` |
| 计数变了要同步脚本 | 经典套件项数变化必须同步 `scripts/smoke_test.py:350` 的 `=== Results: N/N passed ===`，否则冒烟会报 "regression classic" 失败 |
| 需要界面的断言放宿主机 | 涉及物理按键、屏幕渲染、跨复位的验证只能放到脚本层（init 无法自注入扫描码，见 §2.7 的覆盖分工说明） |

### 6.2 在 `smoke_test.py` 里加场景

**纯文本场景（无输入）**——直接往 `SERIAL_ANCHORS` 加一项（`scripts/smoke_test.py:349-383`）：

```python
SERIAL_ANCHORS = [
    # ... 既有项
    # 名称（打印用）、正则（对 build/serial.log 全文 search）
    ("svc net",  r"net started \(PID=\d+\)"),
]
```

**shell 场景（需要键入）**——往 `VGA_SCENARIOS` 加一项三元组（`scripts/smoke_test.py:333-346`）：

```python
VGA_SCENARIOS = [
    # ... 既有项
    # (名称, 命令文本, 期望出现在 VGA 解码文本里的正则)
    ("disk list", "disk list", r"/Volumes/Disk:\s+\d+ KB total"),
]
```

**多步流程**——写一个返回 `bool` 的函数，并在 `main()` 的流程段登记（`scripts/smoke_test.py:699-711`）：

```python
def run_new_flow():
    """一句话说明覆盖哪条需求路径（锚点来自哪个源码）。"""
    print("[flow] new feature")
    ok = run_vga_cmd("mycmd arg", r"mycmd: ok \(\d+\)", 20, "mycmd ok")
    print("  %s mycmd" % ("OK" if ok else "FAIL"))
    if ok:
        ok = wait_serial(r"mycmd: kernel side done", 20, "mycmd serial side")
        print("  %s mycmd serial" % ("OK" if ok else "FAIL"))
    return ok

# main() 中：
    if not run_new_flow():
        failed += 1
```

新增场景时的硬约束：

| 约束 | 说明 |
|---|---|
| 命令字符必须在 `KEYMAP` 里 | `smoke_test.py:61-70` 只映射 `a-z 0-9 空格 / . - _ = ,` 与大写（shift）；出现其它字符会 `FATAL: no KEYMAP entry` 并 `exit(2)`。需要新字符就先扩 `KEYMAP`（对照 `verify_wm.py:39-45` 里冒号的写法） |
| 锚点要躲开被吞的输出 | P2/P2V/KBD 这类"一 tick 内爆发式输出"的套件用"摘要行 \| 首个测试行"交替正则（`:358-366`） |
| 期望文本必须在**当前屏** | `screendump` 只反映当前屏幕；若命令输出很长或后面又清屏（`clear`/`users`/`fm`），锚点会被滚掉 |
| 面板/TUI 需要额外按键 | 若是破坏性操作（`rm`/`mv`/`fm`/`stop`）要用 `run_vga_cmd` 并确认它走的是"确认框"分支（§5.5），必要时自己发 `sendkey y` |
| 依赖磁盘的命令 | 只有 `--drive` 分支才附加 `disk.img`；否则 `/Volumes/Disk` 不存在 |
| 写盘会污染后续步骤 | `run_round3_flow` 的 `fallocate` 会打满 32 MiB Users 卷，所以它放在 R3 且注释要求"之后不得再写 Users"（`:552-555`） |

### 6.3 新增一个宿主机验证脚本的骨架

复制 `verify_wm.py` 的骨架最省事，但要按 §4.10 的教训修正启动锚点：

```python
SERIAL_LOG = os.path.join(REPO, "build/my-serial.log")   # 产物留在工作区便于取证
MON_SOCK   = os.path.join(REPO, "build/my-mon.sock")     # 避免与 smoke 的 /tmp 路径冲突
SCREEN_PPM = os.path.join(REPO, "build/my-screen.ppm")
FONT_H     = os.path.join(REPO, "user/services/term/font.h")

# 1) 启动 QEMU：-vnc（可截图）/ -serial file: / -monitor unix:；需要持久化时附加 virtio-blk
# 2) 启动门：串口 "proc: CREATE pid=\d+ name=shell" 或 VGA r"opsys:[^$]*\$"
#    （不要用 "init: idle loop" —— 实际字符串是 "init: entering idle loop"）
# 3) answer_panels() 之后再发第一条命令
# 4) 每个检查都 ok &= ...，最后扫描 "KERNEL PANIC" / "Triple fault" / "BOOT:"
# 5) 结尾打印 ALL PASS / SOME FAILED，并以 0/1 退出
```

### 6.4 新增测试的合并前清单

| # | 检查 |
|---|---|
| 1 | `make iso` 无新警告（`-Wall -Wextra`，`Makefile:37`、`:53`） |
| 2 | `make format-check` 通过（排除清单：`fs_mem_driver.c`、`term/font.h`、`kernel/include/kernel/panic_font.h`，`Makefile:364-371`） |
| 3 | 自检项数变化后同步 `smoke_test.py` 的计数锚点 |
| 4 | 串口锚点在真实 `build/serial.log` 上手工核对过 |
| 5 | 新增 shell 命令/服务时，脚本侧命令字符都在 `KEYMAP` 内 |
| 6 | 需要持久化的断言只在 `--drive` 分支里 |
| 7 | 失败路径显式断言了错误码（`ERR_NOMEM(-1)`、`ERR_INVAL(-2)`、`ERR_NOCAP(-3)`、`ERR_NOENT(-4)`、`ERR_DENIED(-9)`，`user/lib/libos/syscalls.h:40-49`） |

## 七、回归判定标准与排障

### 7.1 判定矩阵（硬标准）

| 判据 | 通过条件 | 证据来源 |
|---|---|---|
| 构建 | `make iso` 退出码 0 且无新警告 | 构建输出；历史记法"0 新警告"（`docs/ops_format.md:211`） |
| 风格 | `make format-check` 退出码 0（需已安装 clang-format） | `Makefile:364-371` |
| 启动自检 | 8 个套件全部 `passed == run`；串口出现 `init: ALL SELFTESTS PASSED (34/34)` 与 `init: entering idle loop` | `user/services/init/main.c:2580-2581` |
| 无 fail-fast | 串口**没有** `!!! SELFTEST FAILURE` | `:2495` |
| 冒烟 | `=== SMOKE PASSED (R1 + R2 + R3) ===`，进程退出码 0 | `scripts/smoke_test.py:713-720` |
| 崩溃标记 | 日志中 `KERNEL PANIC`、`Triple fault`、`BOOT:` 出现次数为 0 | `scripts/verify_wm.py:237-242`、`scripts/verify_demos.py:255-260` |
| 持久化（`--drive`） | 复位后 `cat /Volumes/Disk/persist.txt` 仍为 5 字节 | `scripts/smoke_test.py:603-639` |
| `.ops` 互认 | `ops_pack.py check` 通过且 pkg-manager 能安装/运行该包 | `docs/ops_format.md:181-216` |

历史基线参考值（数据来自 `docs/test_report.md`；注意其项数基线较早，已被后续轮次扩充）：R1 44 项 + R2 10 项 + R3 14 项 = 68 项（含 `--drive` 5 项）。

### 7.2 通过/失败输出样例

```text
# 通过
  OK    regression classic
  OK    svc pkg
  ...
=== SMOKE PASSED (R1 + R2 + R3) ===

# 失败
TIMEOUT[vga] ls (/Users/)
--- current VGA screen ---
  opsys:/$ ls /Users/
  ls: FAILED (-4) (ENOENT)
--- serial.log tail ---
  ...
=== SMOKE FAILED: 1 failure(s) ===
```

失败路径会打印 `TIMEOUT[...]` + 当前 VGA 屏 + 串口尾部，见 `scripts/smoke_test.py:276-292`。

### 7.3 常见失败原因排查

| 现象 | 根因 | 处理 |
|---|---|---|
| `BOOT FAILED (no shell process)` / 180 s 超时 | ISO 是旧的或没构建；或缺少 virtio-blk 设备，管理器在块设备端口上等满 2000 tick（`user/services/manager/manager.c:597-611`）后才继续 spawn 后续服务，总启动时间被拉长 | 先 `make iso`；确认脚本真的附加了 `disk.img`（`smoke_test.py` 只在 `--drive` 时附加，`verify_*.py` 无条件附加） |
| 脚本报 `BOOT TIMEOUT`，但串口里明明有 `init: entering idle loop` | 脚本等待的是 `init: idle loop`（陈旧锚点，§4.10） | 修正脚本正则，或先手工确认启动完成再跑后续检查 |
| 所有 VGA 锚点都失败、解码结果整屏空白 | `vga_decode.parse_font()` 没解析到字形（要求文件里存在 `s_font[95][16] = {`）；`FONT_H` 路径变了（每个脚本各自硬编码）；GRUB 分辨率不再是 1024×768×32 | 先手工跑一次 `python3 tools/vga_decode.py build/xxx.ppm`；核对 `user/services/term/font.h:26` 与 `boot/grub.cfg:11-14` |
| VGA 文本"错行/错字" | 屏幕尺寸与网格不匹配（`w//9`、`h//20`）；或字体表格式变化（例如把 95 个 ASCII 字形换成 CJK 16×16） | 同上；必要时改用 PPM 像素采样替代文本解码 |
| 串口里套件汇总行缺失，只剩部分 `TEST:` 行 | `SYS_DEBUG_LOG` 令牌桶在单 tick 内截断（512 B/次、2048 B/tick、4096 B 桶上限） | 锚点改用"首测试行"交替正则；或把断言输出拆到多个 tick |
| 键入的命令"没反应"，屏幕上出现 `opsys:/$ y` | Powerbox 面板或 TUI 菜单正持有键盘焦点，键入被面板吃掉、落进 shell 行缓冲 | 只发 `y` 后重新输入命令（§5.4）；确认没有未应答的面板（`perm_query` 可列出待决查询） |
| `pkg remove`/`mv`/`rm` 场景卡住 | 把 TUI 确认框误判成 Powerbox 面板而重输命令，导致第二次调用排队 | 按 §5.5 区分两类对话框；确认框应答后**不要**重输 |
| 持久化断言失败（复位后读不到 `persist.txt`） | `disk.img` 缺失或状态残留：`Disk` 卷持久、`Users` 是 32 MiB 内存卷（重启即丢）；`Disk` 卷在首次启动时格式化（`user/services/vfs/fs_virtio_blk_driver.c:687-744`）；`cache=writethrough` 未生效 | 确认 `--drive` 与 `cache=writethrough`；必要时重建 `disk.img`（`qemu-img create disk.img 8M`）或先 `disk format Disk` |
| P3（kill pkg → 自动重启）失败 | 无 virtio-blk 设备时 `pkg` 尚未 spawn，`FindPidByName("pkg")` 返回 -1 | 附加 `disk.img`；或先跑一次带盘的冒烟 |
| P4 线程表耗尽项失败 | `MAX_THREADS=2048`，但内存不足时 init 会走"降级/跳过"路径（仍算 PASS）；反之若把 `-m` 调得过大/过小都可能偏离脚本的默认假设 | 使用脚本默认的 `-m 256M` |
| 同一份 ISO 两次结果不同 | 宿主负载抖动：`docs/test_report.md` 第八轮补充 3/4 记录过 virtio-blk DMA 超时（`ERR_AGAIN`，驱动自带 reset 自愈）与"隔离测试全通过"的假失败 | 复跑确认；提高 `--step-timeout`；避免与高负载任务并行 |
| `make format-check` 直接报错 | 宿主机没装 clang-format（`command -v clang-format` 为空） | 安装 `clang-format`，或把该步留到 CI |

## 八、硬件与环境要求

### 8.1 宿主工具链

| 组件 | 要求 | 说明 |
|---|---|---|
| GCC（C11） | 必需 | 内核与用户态统一 `-std=c11 -Wall -Wextra -O2`（`Makefile:32-54`） |
| NASM | 必需 | 汇编 `.asm`/`.S`（`Makefile:13`、`:40`） |
| ld.lld 或 GNU ld | 必需 | `LD := ld.lld 优先，回退 ld`（`Makefile:18`） |
| grub2-mkrescue + xorriso | 必需 | 生成 ISO（`Makefile:322`） |
| Python 3 | 必需 | 全部脚本；本机实测 3.14.7 |
| QEMU（`qemu-system-x86_64`） | 必需 | 本机实测 10.2.2；仓库未锁定版本 |
| clang-format | 可选（仅风格检查） | 本机当前**未安装**，`make format-check` 会失败 |
| X11/OpenGL/显示服务 | **不需要** | 所有自动化脚本走 `-vnc` 或 `-display none` |

### 8.2 QEMU 与 CPU/虚拟化

| 项 | 结论 | 依据 |
|---|---|---|
| KVM | **不需要**：仓库中所有 QEMU 调用（`Makefile`、`scripts/run.sh`、全部 Python 脚本）都不带 `-enable-kvm`，程序在 TCG 下运行 | 全仓 grep `kvm` 无命中 |
| 时间基准 | 客户机使用 PIT 100 Hz 作为时间源（`kernel/include/kernel/sched.h:51`、`kernel/arch/x86_64/idt.c:680-694`），`Sleep`/超时断言以 tick 计 | 对宿主 CPU 速度不敏感，但对"客户机被宿主挂起"敏感 |
| 内存 | 脚本统一 `-m 256M`；P4/线程压力依赖空闲页数量（低内存会走降级路径，见 §2.3） | `scripts/smoke_test.py:653-656`、`user/services/init/main.c:829-847` |
| 显示 | 需要 GRUB 提供线性帧缓冲（`gfxmode=1024x768x32` + `gfxpayload=keep`）；`-vnc 127.0.0.1:0` 即可，无需本地显示服务 | `boot/grub.cfg:11-14`、`scripts/smoke_test.py:57` |
| 块设备 | 需要 `virtio-blk-pci,disable-modern=on`（legacy virtio）配合 `user/services/vfs/fs_virtio_blk_driver.c` | `scripts/smoke_test.py:658-659` |
| 串口 | 脚本用**第一条** `-serial file:<path>` 落盘；`make run` 用 `-serial stdio`。注意 `scripts/run.sh --serial` 是在 `-serial mon:stdio` 之后追加第二条，会绑定 **COM2**，该文件恒为 0 字节（已知缺陷） | `Makefile:327-333`、`scripts/run.sh:88-100`、`user/services/serial/serial.c:111-122` |

### 8.3 无图形环境（SSH/CI）下的运行方式

| 场景 | 命令 |
|---|---|
| 只看串口（无 VGA 取证） | `python3 scripts/accept.py`（`-display none`）——注意其锚点陈旧（§4.10） |
| 需要 VGA 取证（推荐） | `python3 scripts/smoke_test.py`（`-vnc 127.0.0.1:0`，截图走 monitor，不依赖 X） |
| 交互式观察 | `make run`（`-serial stdio`）；无 GUI 时同样可用，只是看不到画面 |
| 内核级调试 | `make debug`：`-nographic -serial mon:stdio -s -S`，GDB `target remote :1234`（`Makefile:335-345`） |
| 单次截图取证 | 起 QEMU 后 monitor 执行 `screendump out.ppm`，再 `python3 tools/vga_decode.py out.ppm` |
| 崩溃取证 | 加 `-d int,cpu_reset,guest_errors`（`Makefile:331`、`scripts/run.sh:88-95`） |

### 8.4 资源与时间预算

| 项 | 量级 |
|---|---|
| ISO 产物体积 | 约 21 MB（`build/opsos.iso`） |
| 启动到 shell 提示符 | 十秒级（GRUB 菜单 `timeout=10` 占大头，`boot/grub.cfg:1`） |
| 自检套件 | 经典套件含 10 万次 IPC 往返与 1000 线程压力，墙钟可达数十秒 |
| `smoke_test.py` 总时长 | 分钟级：启动 ≤ 3 分钟 + 15 条串口锚点（每条 ≤ 20 s）+ 10 个 VGA 场景 + 6 个流程；`--drive` 再多一次复位重启 |
| 脚本产物体积 | 串口日志数百 KB 到数 MB；每个 PPM 约 2.36 MB |

## 九、文档与代码差异核对（本文已核实）

| 位置 | 文档说法 | 代码事实 |
|---|---|---|
| `README.md:494` | 经典套件含"3 个基准（10 万次 get_time、**100 万次** yield 往返）" | `BenchYield100k` 实际执行 **1000** 次 yield（`user/services/init/main.c:355-390`）；标题字符串本身写作 `bench: 1k yield round-trips` |
| `scripts/smoke_test.py:355-357` | "`DEBUG_LOG_TICK_BUDGET 512` + 桶上限 1024" | `kernel/syscall/syscall.c:104-107`：512（单次上限）/ 2048（每 tick）/ 4096（桶上限） |
| `scripts/verify_demos.py:186`、`verify_window_demo.py:153` | "P2V 留下一张 init EXEC 待决 Powerbox 面板" | 当前 init 会自行 ANSWER 其查询并抑制 UI_SHOW（§4.10），启动后通常没有待决面板 |
| `docs/test_report.md` 中的 "31/31"、"33/33"、"49 项" | 历史基线（不同轮次） | 当前 HEAD：经典 34 项、总计 58 项；`scripts/smoke_test.py:350` 期望 `=== Results: 34/34 passed ===` |
| `docs/test_report.md:7` | "正常模式 63 项 + `--drive` 追加 5 项" | 这是当时的检查项统计口径，与"init 58 项断言"不是同一层次，勿混用 |

> 返回 [文档索引](README.md)


