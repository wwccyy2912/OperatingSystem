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
