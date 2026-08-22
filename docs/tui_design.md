# OpSys TUI (Text User Interface) 设计文档

> 版本：v1.2  
> 日期：2026-08-22  
> 状态：term 服务侧（term.c，TERM_OP_* 协议）已实现并接入构建；客户端库 user/lib/libtui/tui.c 已接入（shell 的 login/stop 交互组件、window_demo 等使用）；v1.1 交互组件（输入行/密码行/确认框）与 v1.2 区域快照/恢复已实现并通过 verify_users.py 验证  
> 关联：user/services/term/term.c、user/lib/libtui/tui.h、user/lib/libtui/tui.c

---

## 一、设计概述

OpSys 的 TUI 模块提供了一套完整的文本用户界面渲染和控制功能，建立在用户态 Framebuffer 服务基础之上。相比传统的简单文本输出，新 TUI 系统支持：

- **状态栏渲染**：实时显示系统状态（进程数、内存、权限提示）
- **盒子/边框绘制**：创建对话框、窗口框架
- **任意位置文本渲染**：不依赖全局光标的文本输出（用于 Powerbox、覆盖层）
- **光标控制**：查询和设置光标位置（支持未来的交互式 TUI）
- **颜色和属性扩展**：VGA 文本模式和 Linear RGB 双模式支持

---

## 二、架构设计

### 2.1 分层架构

```
┌─────────────────────────────────────────────────────────────┐
│ 应用层                                                       │
│ ├─ shell（命令行）                                          │
│ ├─ TUI 演示应用（tui_demo）                                │
│ └─ 其他服务（需要高级 UI）                                 │
├─────────────────────────────────────────────────────────────┤
│ 客户端库层 (user/lib/libtui/)                             │
│ ├─ tui_write / tui_printf                                  │
│ ├─ tui_clear                                               │
│ ├─ tui_render_status / tui_render_box                      │
│ ├─ tui_render_line_at                                      │
│ └─ tui_set_cursor / tui_get_cursor                         │
├─────────────────────────────────────────────────────────────┤
│ IPC 协议层 (term.c 的 TERM_OP_* 操作码，tui.h 镜像)          │
│ ├─ TERM_OP_WRITE(1)           : 光标渲染                   │
│ ├─ TERM_OP_CLEAR(2)           : 清屏                       │
│ ├─ TERM_OP_STATUS(3)          : 状态栏                     │
│ ├─ TERM_OP_BOX(4)             : 盒子                       │
│ ├─ TERM_OP_RENDER_LINE(5)     : 任意位置文本              │
│ ├─ TERM_OP_SET_CURSOR(6)      : 设置光标                   │
│ └─ TERM_OP_GET_CURSOR(7)      : 查询光标                   │
├─────────────────────────────────────────────────────────────┤
│ 服务层 (user/services/term/term.c)                        │
│ ├─ term_service_main()        : 主线程（IPC 服务）        │
│ ├─ perm_ui_main()             : Powerbox 线程             │
│ ├─ term_write()               : 光标渲染引擎              │
│ ├─ term_render_status()       : 状态栏实现                │
│ ├─ term_render_box()          : 边框绘制                   │
│ ├─ term_render_line_at()      : 位置文本渲染              │
│ ├─ term_set_cursor_pos()      : 光标设置                   │
│ ├─ term_get_cursor_pos()      : 光标查询                   │
│ └─ term_draw_cell()           : 单元格渲染 (VGA+Linear)   │
├─────────────────────────────────────────────────────────────┤
│ Framebuffer 驱动层                                          │
│ ├─ fb_get_info() / fb_map()   : 内核原语                  │
│ ├─ s_fb_va (映射虚拟地址)     : 0x400000000              │
│ ├─ VGA 文本模式 (0xB8000)     : 直接写 u16              │
│ └─ Linear RGB 模式 (32/24bpp) : 像素级操作                │
└─────────────────────────────────────────────────────────────┘
```

### 2.2 线程模型

**term 服务**是一个用户态独立进程，包含三个线程：

```
term 进程（PID = N）
├─ 线程1: term_service_main()
│  ├─ fb_get_info() / fb_map() 初始化
│  ├─ ipc_port_create() + port_register("term", port)
│  ├─ mutex_create() 创建 s_render_lock
│  └─ term_server_loop()
│     └─ ipc_recv("term") → term_handle_request() → ipc_reply()
│
├─ 线程2: perm_ui_main()
│  ├─ ipc_port_create() + port_register("perm.ui", port)
│  └─ ipc_recv("perm.ui") → PERM_OP_UI_SHOW 处理
│     ├─ Powerbox 面板渲染（perm_ui_render_panel）
│     ├─ mutex_lock(s_render_lock)
│     ├─ 屏幕快照/恢复
│     └─ mutex_unlock(s_render_lock)
│
└─ 线程3: perm_ui_input_main()
   ├─ 轮询 s_ui_await，PENDING 期间持有键盘焦点
   ├─ 读取 y/n 按键
   ├─ ipc_send(PERM_OP_ANSWER)（非 ipc_call，避免与 UI_SHOW 死锁）
   └─ 释放键盘焦点
```

**同步机制**：三线程通过 `s_render_lock` Mutex 同步，防止光标状态机竞态。

---

## 三、API 参考

### 3.1 基础文本输出

#### `int tui_write(const char *text, uint32_t len)`

- **功能**：在当前光标位置渲染文本
- **参数**：
  - `text`：文本缓冲区
  - `len`：字节数（≤256）
- **支持的字符**：
  - `\n`：换行 + CRLF 转换
  - `\r`：回到行首
  - `\b`：退格
  - `\t`：制表符（8格对齐）
  - `0x20-0x7E`：可打印 ASCII
- **返回值**：字节数 on success，负数 on error

#### `int tui_write_str(const char *str)`

- **功能**：写入 NUL 终止字符串
- **返回值**：字符数 on success，负数 on error

#### `int tui_printf(const char *fmt, ...)`

- **功能**：格式化输出（类似 printf）
- **支持的格式**：`%d`、`%x`、`%s`、`%c`、`%%`
- **返回值**：字节数 on success，负数 on error

### 3.2 屏幕控制

#### `int tui_clear(void)`

- **功能**：清空整个屏幕，光标复位到 (0, 0)
- **返回值**：0 on success，负数 on error

### 3.3 高级渲染

#### `int tui_render_status(const char *prefix, const char *msg)`

- **功能**：渲染状态栏（屏幕最后一行）
- **格式**：`"prefix: msg"` 显示在高亮背景上
- **参数**：
  - `prefix`：前缀（如 "System"、"perm"）
  - `msg`：消息内容
- **使用场景**：
  - 系统状态提示：`tui_render_status("System", "Ready")`
  - 权限提示：`tui_render_status("perm", "Powerbox Active")`
  - 内存告警：`tui_render_status("Memory", "Low - 10% free")`
- **返回值**：0 on success，负数 on error

#### `int tui_render_box(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const char *title)`

- **功能**：绘制边框盒子
- **参数**：
  - `(x, y)`：盒子左上角（单元格坐标）
  - `(w, h)`：宽和高（单元格数）
  - `title`：标题行文本（NULL 表示无标题）
- **绘制风格**：
  - 四角：`+`
  - 上下边：`-`
  - 左右边：`|`
  - 标题行：在上边框中央显示
- **使用场景**：
  ```c
  tui_render_box(5, 5, 50, 10, "System Menu");  /* 对话框 */
  tui_render_box(2, 1, 70, 5, "Welcome");       /* 标题框 */
  ```
- **返回值**：0 on success，负数 on error

#### `int tui_render_line_at(uint32_t x, uint32_t y, const char *text, uint32_t len)`

- **功能**：在任意位置渲染文本，不改变全局光标
- **参数**：
  - `(x, y)`：渲染起始位置（单元格坐标）
  - `text`：文本内容
  - `len`：字节数
- **特点**：
  - 不影响后续 `tui_write()` 的光标位置
  - 用于 Powerbox 提示行、状态显示、覆盖层
- **使用场景**：
  ```c
  /* Shell 在读输入，Powerbox 需要显示提示 */
  tui_render_line_at(0, 23, "perm: init requests /file (R) - perm_answer 1 y/n", 50);
  ```
- **返回值**：0 on success，负数 on error

### 3.4 光标控制

#### `int tui_set_cursor(uint32_t x, uint32_t y)`

- **功能**：设置光标位置
- **参数**：
  - `(x, y)`：新光标位置（单元格坐标）
- **返回值**：0 on success，负数 on error

#### `int tui_get_cursor(uint32_t *x, uint32_t *y)`

- **功能**：查询当前光标位置
- **参数**：
  - `x, y`：输出指针（NULL 表示不需要）
- **返回值**：0 on success，负数 on error

### 3.5 工具函数

#### `int tui_port_get(void)`

- **功能**：获取 Terminal 服务端口号
- **返回值**：端口号 on success，负数 on error
- **说明**：自动缓存，无需重复调用

### 3.6 区域快照/恢复（v1.2）

#### `int tui_region_save(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint8_t *cells)`

- **功能**：保存 (x,y) 起 w×h 矩形区域的字符单元格
- **参数**：
  - `cells`：输出缓冲，至少 w*h 字节
- **返回值**：0 on success，负数 on error
- **限制**：w*h ≤ TUI_MAX_REGION_CELLS (2048)

#### `int tui_region_restore(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint8_t *cells)`

- **功能**：把之前保存的单元格重绘回 (x,y)
- **返回值**：0 on success，负数 on error

### 3.7 交互组件（v1.1，键盘驱动）

#### `int tui_input_line(int x, int y, const char *prompt, char *buf, int maxlen, int mask)`

- **功能**：在 (x,y) 渲染带提示的输入行，从键盘读一行
- **参数**：
  - `mask`：非 0 时每个字符回显为 `*`（密码输入）
  - `buf`：接收 NUL 结尾字符串
- **返回值**：读入字符数（≥0），负数 on error
- **说明**：内部自动保存/恢复被覆盖区域与光标（非破坏性）

#### `int tui_confirm(int x, int y, int w, const char *title, const char *msg, const char *hint)`

- **功能**：渲染带标题/消息/提示行的确认框，读 y/n
- **返回值**：1（yes）/ 0（no），负数 on error
- **说明**：内部自动保存/恢复被覆盖区域与光标（非破坏性）

---

## 四、协议定义

### 4.1 消息结构

**所有请求**遵循统一格式：

```c
typedef struct {
    u32  op;      /* 操作码 (TERM_OP_*) */
    u32  len;     /* 负载长度 */
    u8   data[];  /* 负载内容 */
} term_req_t;

typedef struct {
    i32  ret;     /* 返回值 (0=success, <0=error) */
    /* 可选：响应数据 (如 GET_CURSOR) */
} term_resp_t;
```

### 4.2 操作码详解

| Op  | 名称        | 请求负载                                              | 响应                  | 说明               |
| --- | ----------- | ----------------------------------------------------- | --------------------- | ------------------ |
| 1   | WRITE       | 长度 + UTF-8 文本                                     | ret                   | 从光标渲染文本     |
| 2   | CLEAR       | 无                                                    | ret                   | 清屏 + 复位光标    |
| 3   | STATUS      | len1(u32) + len2(u32) + prefix + msg                  | ret                   | 渲染状态栏         |
| 4   | BOX         | x(u32) + y(u32) + w(u32) + h(u32) + tlen(u32) + title | ret                   | 绘边框             |
| 5   | RENDER_LINE | x(u32) + y(u32) + text                                | ret                   | 位置渲染（无光标） |
| 6   | SET_CURSOR  | x(u32) + y(u32)                                       | ret                   | 设光标             |
| 7   | GET_CURSOR  | 无                                                    | ret + x(u32) + y(u32) | 查光标             |
| 8   | SNAPSHOT    | x(u32) + y(u32) + w(u32) + h(u32)                    | ret + cells[w*h]      | 保存单元格区域     |
| 9   | RESTORE     | x(u32) + y(u32) + w(u32) + h(u32) + cells[w*h]       | ret                   | 恢复单元格区域     |

> **SNAPSHOT/RESTORE（v1.2）**：`w*h ≤ TERM_MAX_REGION_CELLS (2048)`。SNAPSHOT
> 把矩形区域内的字符单元格拷回客户端；RESTORE 把之前保存的单元格重绘到原位置
> （非 0x20–0x7E 的字节按空格处理）。这是**非破坏性对话框覆盖**的基础：弹框前
> 保存区域，关闭后恢复，底层 shell 文本不受影响。

### 4.3 错误码

继承自 libos/syscalls.h：

```c
#define OK               0    /* 成功 */
#define ERR_NOMEM       -1    /* 内存不足 */
#define ERR_INVAL       -2    /* 无效参数 */
#define ERR_NOCAP       -3    /* 能力不足 */
#define ERR_FAULT       -7    /* 内存错误 */
```

### 4.4 交互组件（v1.1/v1.2，客户端键盘驱动）

`tui_input_line` / `tui_confirm` 直接对 `keyboard` 服务发 `KBD_OP_READ_BLOCK`
（阻塞读）。两者都遵循**非破坏性覆盖**协议：

1. `tui_region_save` 保存将被覆盖的区域（输入行 / 对话框矩形）＋ 当前光标；
2. 渲染组件并循环读键；
3. 结束后 `tui_region_restore` 恢复区域与光标，屏幕回到弹框前状态。

因此调用方（如 shell 的 `stop` 命令）在弹框结束后可以继续正常输出，屏幕上
不会残留对话框边框或输入行。

---

## 五、实现细节

### 5.1 Framebuffer 映射

- **虚拟地址**：`TERM_FB_VA = 0x400000000`（16 GiB，不与堆/栈冲突）
- **VGA 文本模式**：
  - 物理地址 `0xB8000`
  - 每单元格 2 字节：`(attr << 8) | ch`
  - 属性字节：`(bg_color << 4) | fg_color`
- **Linear RGB 模式**：
  - 32bpp ARGB：直接写 `u32`
  - 24bpp BGR：写 3 字节 (B, G, R)
  - 每行间距：`pitch` 字节（可能包含 padding）

### 5.2 字体渲染

- **字库**：`s_font[95][16]`（8×16 位图）
- **覆盖范围**：ASCII 0x20-0x7E（95 个可打印字符）
- **单元格**：9×20 像素（8×16 字形 + 间距）
- **颜色**：
  - 前景色（文本）：默认白色 `0x00FFFFFF`
  - 背景色：默认深蓝 `0x00082860`
  - 状态栏背景：`0x004B6EA6`（浅蓝）

### 5.3 光标管理

**字符缓冲区** `s_cells[TERM_MAX_ROWS][TERM_MAX_COLS]`：

- 持有整个屏幕的 ASCII 字符
- 光标显示为反色单元（前景/背景互换）
- 擦除光标时直接使用 `s_cells` 的存储值，无需像素回读

**光标位置**：`(s_cursor_x, s_cursor_y)` in cell units

---

## 六、使用示例

### 6.1 基础用法

```c
#include "../lib/libtui/tui.h"

int main(void) {
    /* 清屏 */
    tui_clear();

    /* 绘制标题框 */
    tui_render_box(2, 1, 70, 5, "Application Title");

    /* 光标渲染 */
    tui_set_cursor(5, 7);
    tui_write_str("Enter your name: ");

    /* 状态栏 */
    tui_render_status("App", "Running");

    return 0;
}
```

### 6.2 Powerbox 集成（term.c 内部）

```c
/* perm-manager 通知：新的权限查询 */
perm_req_ui_t *req = ...;

/* 构造提示行 */
char prompt[256];
snprintf(prompt, sizeof(prompt),
    "perm: proc %s requests %s (%s) - perm_answer %d y/n",
    req->name, req->url, perm_access_str(req->access), req->query_id);

/* 渲染到最后一行（不干扰 shell 的 prompt） */
tui_render_line_at(0, s_rows - 1, prompt, strlen(prompt));
```

> 注：以上为先期设计示例。实际实现为居中面板 `perm_ui_render_panel()`（term.c:893-1005）：含标题（Permission Request）、请求方 `<name> (PID <pid>)`、资源 URL、访问类型、描述标签，以及 `Allow? (y/n)` / `Result: ALLOWED/DENIED` 提示行。

### 6.3 系统状态监控

```c
void update_status_bar(void) {
    int free_pages = get_free_pages();
    int free_mb = (free_pages * 4) / 1024;

    char status[128];
    snprintf(status, sizeof(status), "Memory: %d MB free | PID=%d",
             free_mb, get_pid());

    tui_render_status("System", status);
}
```

---

## 七、性能考量

### 7.1 优化策略

1. **字符缓冲区缓存**：`s_cells` 避免像素回读
2. **Mutex 同步**：`s_render_lock` 保护状态机，而非逐像素锁
3. **批量操作**：`term_write()` 处理整个字符串，不逐字调用
4. **VGA 文本快速路径**：直接 `memset` 而非 `fill_rect`

### 7.2 已知限制

- **单线程 shell**：输入阻塞时无法响应 Powerbox 提示
  - 解决方案（Phase 3）：Shell 异步 readline（线程池）
- **无 Z-order**：覆盖层简单覆盖，无透明度
  - 解决方案（Phase 3）：分层组合
- **颜色有限**：VGA 16 色（Linear RGB 全色但需更复杂的渲染）

---

## 八、未来扩展（Phase 3+）

1. **文本属性**：粗体、斜体、下划线
2. **鼠标支持**：点击事件、拖拽
3. **异步输入**：非阻塞 readline
4. **滚动缓冲**：超出屏幕高度时缓存历史
5. **多窗口**：独立的渲染区域（✅ v0.4 已落地为 wm 窗口管理器，见 §十二）
6. **颜色调色板**：用户定义的配色方案

---

## 九、文件清单

| 文件                            | 说明                 | 行数  |
| ------------------------------- | -------------------- | ----- |
| `user/services/term/term.c`     | Framebuffer 终端服务 | 1000+ |
| `user/lib/libtui/tui.h`         | TUI 客户端库头文件   | 129   |
| `user/lib/libtui/tui.c`         | TUI 客户端库实现     | 270   |
| `user/services/tui_demo/main.c` | TUI 演示应用         | 60+   |
| `user/services/wm/main.c`       | 窗口管理器服务（v0.4） | 490+ |
| `user/lib/libwm/wm.h/.c`        | wm 客户端库（v0.4）  | 60+/180+ |
| `user/services/wm_demo/main.c`  | 桌面演示（v0.4）     | 90+   |
| `docs/tui_design.md`            | 本文档               | —     |

---

## 十、测试计划

### 10.1 单元测试

- [ ] 每个 TUI*OP*\* 操作码路径
- [ ] 边界条件：屏幕满、超长字符串、无效坐标
- [ ] 并发：两线程同时渲染（Mutex 正确）

### 10.2 集成测试

- [ ] Shell + Powerbox：权限查询提示不被 shell 淹没
- [ ] tui_demo：演示所有功能，无图形损坏
- [ ] 文件系统：通过 VFS 读取文本配置文件，显示列表

### 10.3 压力测试

- [ ] 1000 行换行输出：性能可接受
- [ ] 快速切换 BOX/STATUS/LINE 操作：无闪烁

---

## 十一、已知问题

### Bug/限制

1. **SET_CURSOR 后越界检查**
   - 当前代码：`if (x >= s_cols) x = s_cols - 1;`
   - 建议：抛出错误而非默默截断

2. **TERM_STATUS_ROW 硬编码**
   - 当前：`#define TERM_STATUS_ROW (TERM_MAX_ROWS - 1)`
   - 问题：实际行数由 framebuffer 高度决定
   - 修复：从 `s_rows` 动态计算

3. **Powerbox 提示中的 subject_id**（已修复）
   - 面板现渲染 `<name> (PID <pid>)`（term.c:943-961），不再显示 subject_id

---

## 十二、v0.4 窗口管理器（wm）

> 版本：v0.4，2026-08-22  
> 关联：`user/services/wm/main.c`（服务）、`user/lib/libwm/`（客户端库）、
> `user/services/wm_demo/main.c`（桌面演示）、`scripts/verify_wm.py`（验证）

### 12.1 架构

```
wm_demo (桌面客户端)         其他应用 (libwm)
      │  ipc_call("wm")            │
      ▼                           ▼
┌──────────────────────── wm 服务（Ring 3 独立进程）───────────────┐
│  线程1: server（ipc_recv_from → 注册表操作 → 合成 → ipc_reply）  │
│  线程2: input（激活时 TAKE_FOCUS → 读键 → 焦点/移动/退出）       │
│  窗口注册表: WM_MAX_WINDOWS=16 个 {id, title, x, y, w, h, owner, │
│             body[8][44]}；owner = 创建者 subject（不可伪造）     │
│  合成器:   tui_clear + 逐窗口 tui_render_box/line_at + 状态栏；  │
│            焦点窗口最后绘制（置顶）+ 标题 `*` 标记               │
└──────────────────────────────────────────────────────────────────┘
      │  ipc_call("term")   （wm 不直接碰 framebuffer）
      ▼
 term 服务（显示所有者，ATOM_SERVICE_MANAGE 门控 fb）
```

### 12.2 协议（wm.h / libwm wm_proto.h）

| Op | 名称 | 请求 → 响应 | 说明 |
|----|------|------------|------|
| 1 | CREATE | {title,x,y,w,h} → {win_id} | 建窗口（自动聚焦，置顶） |
| 2 | DESTROY | {win_id} → {ret} | 仅 owner/管理面 |
| 3 | LIST | {} → {count, lines[]} | 列出 "id title" |
| 4 | FOCUS | {win_id} → {ret} | 设焦点（0=无） |
| 5 | MOVE | {win_id,mx,my} → {ret} | 仅 owner/管理面，越界钳制 |
| 6 | WRITE | {win_id,row,text} → {ret} | 写内容行，仅 owner/管理面 |
| 7 | ACTIVATE | {} → {ret} | 启动桌面会话（取键盘焦点） |
| 8 | DEACTIVATE | {} → {ret} | 结束会话（释放焦点） |
| 9 | GET_STATE | {} → {active,focus,count} | 会话/焦点/窗口数查询 |

**安全**：DESTROY/MOVE/WRITE 用 `ipc_recv_from` 的内核 subject 与窗口
owner 比对；管理面（`ATOM_SERVICE_MANAGE`）可跨窗口操作。wm 自身不持
framebuffer 能力——渲染全部经 term IPC，与 window_demo 相同的安全模型。

### 12.3 输入路由

会话激活时 wm `TAKE_FOCUS`（keyboard 服务焦点路由）：1-9 聚焦第 N 个
窗口，h/j/k/l 移动焦点窗口，q 结束会话（释放焦点 + 清屏）。期间 shell
的 read_line 停在 park 表不被打扰；会话结束后 shell 立即恢复输入。
