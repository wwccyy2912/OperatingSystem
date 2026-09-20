# OpSys 像素 GUI 与合成器设计文档

> 适用版本：OpSys v0.8-dev（git HEAD 105805d）　|　最后更新：2026-09-19
>
> gui 服务（Ring 3 独立进程）以 blob 身份取得 ATOM_SERVICE_MANAGE 门禁映射线性 framebuffer，为每个窗口维护一块 32bpp 离屏像素缓冲，通过 "gui" IPC 端口提供绘制（FILL/TEXT）与输入事件（KEY/鼠标），实现 8 窗口、脏区合成的像素级桌面。

---

## 一、目标与定位

### 1.1 三条显示路径

OpSys 的显示子系统不是单一实现，而是三条并存的路径。三者共享同一块由内核映射的 framebuffer，但渲染模型、所有权与客户端 API 完全不同：

| 路径 | 服务/库 | 渲染模型 | 写入 framebuffer 的主体 | 客户端 API |
|------|---------|----------|--------------------------|------------|
| 文本终端 | `term` 服务 + `user/lib/libtui/tui.c` | 字符单元（cell）+ 8x16 位图字模，按行列定位 | term（显示所有者，映射 fb） | `TERM_OP_*`（term.c:116-130）、libtui 封装 |
| 文本窗口管理器 | `wm` 服务（v0.4）+ `user/lib/libwm/` | 文本窗口：窗口体是 `body[8][44]` 字符行，经 term 渲染 | term（wm 自身不碰 fb） | `WM_OP_*`（user/lib/libwm/wm_proto.h:38-46） |
| 像素 GUI | `gui` 服务 + `user/lib/libgui/gui.c` | 像素：窗口 = 32bpp 离屏画布，合成器逐像素 blit | gui（自己映射 fb） | `GUI_OP_*`（user/services/gui/gui.h:49-59） |

三者的定位差别可以一句话概括：**term 是"字符级"路径，wm 是"文本级窗口"路径，gui 是"像素级"路径**。wm 的窗口边界只能落在字符网格上（窗口位置/尺寸以字符单元为单位，由 `WM_OP_*` 的 box 渲染驱动，见 `docs/tui_design.md` §12.1-12.2），而 gui 的窗口边界是任意像素坐标（`gui_win_t.x/y/w/h`，user/services/gui/main.c:96-106），窗口内容是客户端自己绘制的像素。

### 1.2 gui 与 wm 的边界

| 维度 | wm（v0.4，文本级） | gui（v0.7.1/0.8-dev，像素级） |
|------|--------------------|-------------------------------|
| 窗口上限 | `WM_MAX_WINDOWS` = 16 | `GUI_MAX_WINDOWS` = 8 |
| 窗口内容 | 8 行 × 44 字节文本行 | 客户端自绘像素（`w×h×4` 字节离屏缓冲） |
| 客户端绘制接口 | `WM_OP_WRITE {win_id,row,text}` | `GUI_OP_FILL` / `GUI_OP_TEXT`（任意矩形/坐标） |
| framebuffer | 不映射（`wm` 不持 `ATOM_SERVICE_MANAGE` 用途的 fb 映射，渲染全部经 term IPC） | 由 libgui `GuiFbOpen()` 经 `SYS_FB_GET_INFO`+`SYS_FB_MAP` 直接映射 |
| 合成方式 | `tui_clear` + 逐窗口 `tui_render_box/line_at` + 状态栏 | 脏区矩形合成为像素：背景 fill → 窗口 blit → 任务栏 → 指针 |
| 输入 | 键盘焦点 + `1-9`/移动字母命令 | 键盘（焦点窗口）+ PS/2 鼠标（命中测试/拖动/滚轮/双击） |
| 典型客户端 | `user/services/wm_demo/main.c` | `user/services/gui_demo/main.c` |

两者是**并列而非替代**关系：gui 服务启动后处于 idle（不写 fb、无焦点）直到有客户端调用 `GUI_OP_ACTIVATE`（`user/services/gui/gui.h:57`、main.c:1116-1147），因此文本会话与像素桌面可以在同一台机器上先后使用，互不抢占（见第七章）。

### 1.3 设计目标与不变量

1. **内核不画像素**：内核只初始化 framebuffer、解析 Multiboot2 tag 8、提供 `SYS_FB_GET_INFO`(44)/`SYS_FB_MAP`(45)（kernel/include/kernel/syscall_numbers.h:108-109），所有绘制原语已从内核移除（kernel/gfx/framebuffer.c:21-25）。
2. **fb 是受门禁的系统资源**：只有持 `ATOM_SERVICE_MANAGE` 的主体能查询/映射 fb（kernel/syscall/syscall.c:664-681、700-702），该原子由 blob 内容身份播种（kernel/syscall/process_desc.c:312-330，`"gui"` 在服务 blob 白名单内）。
3. **窗口属主不可伪造**：`gui_win_t.owner` 来自 `IpcRecvFrom` 的内核填充 sender subject（main.c:1190-1196、`DoCreate(..., u64 caller)`），所有窗口变更 op 都做 `w->owner != 0 && w->owner != caller` 判定（destory/move/focus/fill/text/resize 六处）。
4. **帧缓冲写放大要可控**：每次变更只记录一个脏矩形，合成只重绘该矩形（main.c:175-243、405-580）。

---

## 二、分层架构

### 2.1 分层图

```
  Ring 3 客户端（gui_demo 等）
      │  IpcCall("gui", {u32 op; u32 len; u8 data[]}, resp)
      ▼
 ┌──────────────────────────── gui 服务（独立进程，user/services/gui/main.c）───────────────────────────┐
 │  线程 1 GuiServerLoop(port)          "gui" 端口，ipc_recv_from → Do*(token,msg_len,caller) → ipc_reply   │
 │    ├─ 窗口表 s_wins[GUI_MAX_WINDOWS]  {owner,id,title,x,y,w,h,gui_canvas_t buf,maxed,hidden,rx/ry/rw/rh}│
 │    ├─ 合成器 GuiComposite()          脏区矩形 → 背景 fill → 窗口(边框/标题/内容 blit) → 任务栏 → 指针    │
 │    └─ 事件环 s_events[GUI_MAX_EVENTS] {type,code,x,y,win,owner}，由 GUI_OP_POLL 按 owner 取走            │
 │  线程 2 GuiInputMain()               KBD_OP_READ / KBD_OP_MOUSE_READ 轮询 → 命中测试/焦点/拖动 → EvPush   │
 │  两线程共享状态（窗口表/事件环/指针）由 s_lock（内核 mutex）串行化                                        │
 └───────────────────────────────────────────┬──────────────────────────────────────────────────────────┘
                                             │  libgui 像素绘制库（user/lib/libgui/gui.c）
                                             │  GuiFbOpen → fb_map；GuiPixel/Fill/Hline/Vline/Rect/Text/Blit
                                             ▼
 ┌──────────────── framebuffer（线性 xRGB / BGR，由 GRUB 设置 gfxmode=1024x768x32）──────────────────────┐
 │  内核 kernel/gfx/framebuffer.c：FbInit（tag 8）→ 映射 + 清屏；FbGetUserInfo → SYS_FB_GET_INFO 载荷       │
 └───────────────────────────────────────────────────────────────────────────────────────────────────────┘
      ▲                                                          ▲
      │ SYS_FB_GET_INFO(44) / SYS_FB_MAP(45)（ATOM_SERVICE_MANAGE 门禁）
      │
 term 服务（文本屏所有者，映射在同一物理 fb）        keyboard 服务（PS/2 键盘 + 鼠标驱动）
      ▲                                                  ▲
      │ TERM_OP_REDRAW(11)（DEACTIVATE 时由 gui 调用）    │ KBD_OP_READ(1) / KBD_OP_MOUSE_READ(5)
```

### 2.2 各层职责

| 层 | 文件 | 职责 |
|----|------|------|
| 内核 fb 驱动 | `kernel/gfx/framebuffer.c`、`kernel/include/kernel/framebuffer.h` | 解析 Multiboot2 tag 8、判定 VGA 文本/线性模式、映射到内核虚拟空间、开机清屏；只提供查询 API（`fb_get_info`/`FbGetPhys`/`FbGetUserInfo`/`FbIsVgaText`） |
| framebuffer 系统调用 | `kernel/syscall/syscall.c:664-754` | `sys_fb_get_info` 填 `fb_user_info_t`；`sys_fb_map` 把物理页映射进调用者地址空间（`PTE_PRESENT|PTE_USER|PTE_WRITABLE|PTE_NO_EXECUTE`），并对齐/大小做校验与钳制（页对齐判定在 syscall.c:717） |
| 用户态像素库 | `user/lib/libgui/gui.c`（声明 `gui.h`） | `gui_canvas_t` 抽象（w/h/pitch/bpp/buf）+ 无状态绘制原语，全部裁剪到画布边界；32bpp/24bpp 双格式 |
| 合成器服务 | `user/services/gui/main.c` + `gui.h` | 窗口表、Z 序、焦点、离屏缓冲、合成、事件队列、输入线程、生命周期 |
| 客户端 | `user/services/gui_demo/main.c`、shell `gui` 命令（user/services/shell/shell.c:2454-2480） | 直接包含 `../gui/gui.h`，自己封装 RPC |

### 2.3 颜色与像素格式

颜色常量统一用 `0x00RRGGBB`（R 在 bit16、G 在 bit8、B 在 bit0）在源码中书写，例如 `GUI_BG_COLOR 0x00202040`、`GUI_TITLE_BG 0x00204080`（main.c:75-89）。**写进显存的方式随 bpp 不同**：

| fb bpp | libgui 的写入方式 | 显存字节序 | 源码 |
|--------|-------------------|------------|------|
| 32 | `GuiXrgb(color)` = `(B<<24)|(G<<16)|(R<<8)`，整字写入 | 小端下字节序列 = [0, R, G, B]，即 QEMU VGA 32bpp 的 xRGB（byte1=R） | gui.c:92-97、gui.c:105-106 |
| 24 | 拆成 p[0]=B、p[1]=G、p[2]=R 三字节 | BGR 字节序 | gui.c:107-112、gui.c:142-155 |

因此：

- **32bpp 路径需要一次通道重排**（`0x00RRGGBB` → xRGB word），"红色分量落在 word 的 bit 8"是这条路径的核心约定；窗口缓冲与 fb 都使用同一种 xRGB word，所以合成时 `GuiBlit` 可以逐字拷贝（gui.c:91、355-362）。
- **24bpp 路径直接按字节写**，不需要重排（`p[0]=color&0xFF` 就是蓝）。
- 内核开机清屏对该差异也是分开处理的（kernel/gfx/framebuffer.c:76-91：32bpp 写整字 `0x00082860`，24bpp 写 p[0]=0x60/p[1]=0x28/p[2]=0x08）。

> 注意一处**代码级不一致**（本文如实记录，不视为规范）：term 服务在 32bpp 下把 `0x00RRGGBB` **原样**写入显存，不做 `GuiXrgb` 式重排（`user/services/term/term.c:306-318`，其调色板同样是 `0x00RRGGBB`，term.c:153-169）。也就是说 term 与 libgui 对 32bpp word 的解释差一次 R/B 交换。本文档只描述各自代码的行为，不推断哪种是"正确"的硬件解释；test_report 中 GUI 的验证方式是 PPM 像素采样（`docs/test_report.md` 第十一轮）。

绘制原语（全部带边界裁剪，越界即静默丢弃）：

| 函数 | 语义 |
|------|------|
| `GuiPixel(c,x,y,color)` | 单像素，x/y 越界直接返回（gui.c:99-113） |
| `GuiFill(c,x,y,w,h,color)` | 矩形填充，含左闭右开裁剪与负坐标修正（gui.c:115-156） |
| `GuiHline/GuiVline/GuiRect` | 由 Fill 组合而成；负长度先把起点左移再取绝对值（gui.c:158-181） |
| `GuiText(c,x,y,s,fg,bg)` | UTF-8 文本，`bg==0` 表示**不绘制背景**（透明），并非 alpha 混合 |
| `GuiTextWidth(s)` | 显示宽度（px），与渲染器的推进规则一致 |
| `GuiBlit(dst,dx,dy,src,sx,sy,w,h)` | 同 bpp 区域拷贝；`dst->bpp != src->bpp` 时**直接返回**（gui.c:314-319） |

### 2.4 framebuffer 的取得路径与门禁

```
GuiFbOpen(&s_fb)                          gui.c:46-77
  ├─ FbGetInfo(&info)                     → SYS_FB_GET_INFO(44)，载荷 fb_user_info_t{phys_addr,w,h,pitch,bpp,vga_text}
  │    └─ 内核侧门禁：CapLookupByAtom(ATOM_SERVICE_MANAGE) 否则 ERR_NOCAP（syscall.c:669-674）
  ├─ info.vga_text → 直接返回 ERR_INVAL（像素 GUI 无法渲染 VGA 文本缓冲，gui.c:56-61）
  ├─ fb_size = info.pitch * info.height   （注意：未做页对齐，见第九章 L1）
  └─ fb_map((void*)0x60000000, fb_size)   → SYS_FB_MAP(45)
       └─ 内核侧：virt 必须在用户空间且页对齐、size 页对齐且 ≤ 真实 fb 大小（syscall.c:712-739）
```

`gui_canvas_t` 只是"一块像素缓冲的描述符"（user/lib/libgui/gui.h:41-47），同一套原语既画 fb 也画离屏窗口缓冲：

```c
typedef struct {
    u32 w;     /* 宽（像素） */
    u32 h;     /* 高（像素） */
    u32 pitch; /* 行字节数 */
    u32 bpp;   /* 32 或 24 */
    u8 *buf;   /* 缓冲基址 */
} gui_canvas_t;
```

窗口缓冲永远是 32bpp：`DoCreate` 与 `GuiWinRealloc` 都显式设置 `bpp=32 / pitch=w*4`（main.c:746-750、593-619）。

---

## 三、gui 服务协议

### 3.1 端口与消息封装

- 端口名 `"gui"`（`GUI_PORT_NAME`，gui.h:39）：服务启动时 `IpcPortCreate()` + `PortRegister("gui", port)`（main.c:1617-1630）；客户端 `PortGet("gui")`。
- 请求 = `{ u32 op; u32 len; u8 data[4088]; }`（`gui_req_t`，共 4096 字节）；响应 = `{ i32 ret; u8 data[4092]; }`（`gui_resp_t`，共 4096 字节），由 `_Static_assert` 钉死在 `GUI_IPC_MAX`（gui.h:135-148）。
- 实际发送长度为 `8 + payload_len`（客户端只发头部+有效载荷，见 gui_demo 的 `GuiCall`，gui_demo/main.c:35-44）；服务端**只信 `msg_len`，不读 `req->len`**——所有 `Do*` 的分支判断都用真实收到的字节数（例如 `DoCreate` 的 `msg_len < 8 + sizeof(gui_req_create_t)`）。
- 响应长度固定：每个 op 都 `IpcReply(token, resp, sizeof(gui_resp_t))`，即无论内容多少都会回 4096 字节（见第九章 L7）。

### 3.2 opcode 总表

| op | 名称 | 请求载荷 | 成功响应 | 服务端入口 |
|----|------|----------|----------|------------|
| 1 | `GUI_OP_CREATE` | `gui_req_create_t{char title[64]; i32 w; i32 h}`（72B） | `ret` = 新窗口 id（≥1） | main.c:659 |
| 2 | `GUI_OP_DESTROY` | `i32 id`（4B） | `ret` = 0 | main.c:772 |
| 3 | `GUI_OP_MOVE` | `gui_req_move_t{i32 id,x,y}`（12B） | `ret` = 0 | main.c:818 |
| 4 | `GUI_OP_FOCUS` | `gui_req_id_t{i32 id}`（4B） | `ret` = 0 | main.c:927 |
| 5 | `GUI_OP_FILL` | `gui_req_fill_t{i32 id,x,y,w,h; u32 color}`（24B） | `ret` = 0 | main.c:985 |
| 6 | `GUI_OP_TEXT` | `gui_req_text_t{i32 id,x,y; u32 fg,bg; char text[256]}`，`text` 在偏移 16 | `ret` = 0 | main.c:1015 |
| 7 | `GUI_OP_POLL` | 无 | `gui_resp_poll_t{ret,count,events[32]}`（1032B） | main.c:1070 |
| 8 | `GUI_OP_POINTER` | 无 | `data` = 3×i32 `{x, y, buttons}` | main.c:1102 |
| 9 | `GUI_OP_ACTIVATE` | 无 | `ret` = 0 | main.c:1116 |
| 10 | `GUI_OP_DEACTIVATE` | 无 | `ret` = 0 | main.c:1175 |
| 11 | `GUI_OP_RESIZE` | `gui_req_resize_t{i32 id,w,h}`（12B） | `ret` = 0 | main.c:867 |

最小消息长度校验（含 8 字节头）：CREATE ≥ 80、DESTROY/FOCUS ≥ 12、MOVE/RESIZE ≥ 20、FILL ≥ 32、TEXT ≥ 28；POLL/POINTER/ACTIVATE/DEACTIVATE 只受服务器循环的 `msg_len < 8` 总检查约束（main.c:1196-1200）。不满足即 `ret = -2`（`ERR_INVAL`）。

### 3.3 逐 op 语义

**CREATE（1）** `{title[64]; w; h}` → 窗口 id

- `w/h` 是**内容区**尺寸，不含边框与标题栏；服务端按 `max_w = fb.w - 2*GUI_BORDER`、`max_h = fb.h - 2*GUI_BORDER - GUI_TITLE_H` 校验，且要求 `w,h ≥ 16`（main.c:667-682）。1024x768 下即内容区最大 1022×750（`768 − 2 − 16`）。
- 取第一个空闲槽位（main.c:687-694），`owner = caller`，`id = s_next_id++`（从 1 起、单调递增、不复用，main.c:714）。
- 分配 `w*h*4` 字节离屏缓冲并填 `0x00000000`（不透明黑，main.c:706-712、750）。
- **自动放置**：从 (20,20) 起以步进 (+24,+18) 搜索第一个不与既有窗口相交的位置，最多 400 次；`px + ww + 20 > fb.w` 时换行（`px=20, py+=130`）；`py + wh > fb.h` 时退回 `py=20`（宁可重叠也不失败），main.c:722-758。
- 失败：槽位满 → `-1`（`ERR_NOMEM`）；malloc 失败 → `-1`；参数非法 → `-2`。

**DESTROY（2）** `{id}`：仅属主可销毁（`w->owner != 0 && w->owner != caller` → `-4`）。释放缓冲、清零槽位、若被销毁窗口持有焦点则清焦点，然后 `EvPush(GUI_EV_CLOSE, 0,0,0, closed_id)`、标脏旧区域；**若销毁后活动窗口数为 0 则调用 `GuiShutdown()`**（归还键盘焦点 + 让 term 重绘文本屏），否则只重合成（main.c:772-816）。

**MOVE（3）** `{id,x,y}`：越界钳制到 `[0, fb.w-边框-2*GUI_BORDER]`/对应高度（保证窗口不会被推出屏幕），标脏旧位置 ∪ 新位置后重合成（main.c:836-862）。

**FOCUS（4）** `{id}`：核心约束是**最小化窗口不得获得焦点**，否则键盘会送给一个不可见窗口：`w->hidden` 时返回 `-2`（main.c:946-953）。焦点变化时同时标脏旧焦点窗口与新焦点窗口（标题栏高亮要换），再重合成（main.c:954-980）。

**FILL（5）** `{id; x,y,w,h; color}`：坐标是**窗口内容区局部坐标**，调 `GuiFill(&w->buf, ...)`；标脏矩形 = 内容区原点 `(w->x+GUI_BORDER+f->x, w->y+GUI_BORDER+GUI_TITLE_H+f->y)` 起的 `f->w × f->h`（main.c:1009-1010）。

**TEXT（6）** `{id; x,y; fg; bg; text[N]}`：

- 文本长度取 `msg_len - (8 + offsetof(gui_req_text_t, text))` = `msg_len - 24`，钳到 255 后补 `'\0'`；源码明确警告**不能 `strlen`**——`s_req` 是静态缓冲，会保留上一次请求的尾部（main.c:1027-1036）。
- `bg == 0` 表示透明（只画前景像元）；标脏矩形宽取 `GuiTextWidth(t->text)`、高固定 16（main.c:1048-1049）。
- 约定载荷长度应为 `offsetof(text) + strlen + 1 = 17 + strlen`；gui_demo 实际发送 `20 + strlen + 1`（gui_demo/main.c:109），多出的 5 字节是零填充，因为服务端渲染到第一个 `'\0'` 即停止，所以无害。

**POLL（7）**：非阻塞，返回属于本客户端的事件并就地压缩事件环（见 3.5）。

**POINTER（8）**：返回当前指针位置与按键位掩码（`MOUSE_BTN_LEFT 0x01 / RIGHT 0x02 / MID 0x04`，keyboard.c:138-140）——这是协议里**唯一能区分左/右/中键**的入口。

**ACTIVATE（9）/ DEACTIVATE（10）**：见第七章。

**RESIZE（11）** `{id,w,h}`：校验 `16 ≤ w ≤ fb.w-2`、`16 ≤ h ≤ fb.h-2-16`；**先分配新缓冲再动状态**，把旧内容左上角重叠区拷贝过去（其余为 0），释放旧缓冲；随后把窗口重新钳制回屏幕内；标脏旧 ∪ 新区域（main.c:867-925、593-619）。客户端必须自己重绘新暴露的区域。

### 3.4 事件模型

事件结构（gui.h:117-126）：

```c
typedef struct {
    u32 type;
    u32 code;
    i32 x;
    i32 y;
    i32 win;   /* 目标窗口 id：KEY=焦点窗口，鼠标=命中窗口（0 = 无） */
    u64 owner; /* 目标窗口的属主 subject；0 = 广播 */
} gui_event_t;   /* 按 C ABI 对齐为 32 字节（20B 头 + 4B 填充 + 8B owner） */
```

| 值 | 类型 | `code` | `x,y` | `win` |
|----|------|---------|--------|--------|
| 1 | `GUI_EV_KEY` | 键盘字节（scancode 解码后的 ASCII/控制码） | 当前指针位置 | **焦点窗口 id**（0 = 无焦点） |
| 2 | `GUI_EV_MOUSEMOVE` | 0 | 指针位置（fb 绝对坐标） | 指针下的窗口 id（0 = 桌面） |
| 3 | `GUI_EV_BUTTON` | **1 = 按下，0 = 释放**（见下方说明） | 指针位置 | 按下：任务栏按钮或命中窗口；释放：隐式 grab 目标 |
| 4 | `GUI_EV_WHEEL` | 有符号滚动增量（`u32` 载体内是有符号值） | 指针位置 | 指针下的窗口 |
| 5 | `GUI_EV_CLOSE` | 0 | 0,0 | 被销毁的窗口 id |

> `gui.h:111` 的注释写 `code = 1 left, 2 right, 3 middle`，但实现里按下事件硬编码 `code = 1`、释放硬编码 `code = 0`（main.c:1556、1569），并不区分按键。要区分左右中键必须用 `GUI_OP_POINTER` 的 `buttons`。这是**注释与实现不一致**，本文以实现为准。

事件环：`s_events[32]`（`GUI_MAX_EVENTS`）环形队列 + `s_ev_head`/`s_ev_count`（main.c:132-134）；`EvPush` 在队满时**直接丢弃**新事件（main.c:161-162，注释 "ring full: drop"）。

```c
/* main.c:154-171（要点） */
static void EvPush(u32 type, u32 code, i32 x, i32 y, i32 win) {
    u64 owner = 0;
    if (win != 0)
        owner = WinOwner(win);      /* 解析目标窗口属主；0 = 广播 */
    if (s_ev_count >= GUI_MAX_EVENTS)
        return;                     /* 队满丢弃 */
    u32 idx = (s_ev_head + s_ev_count) % GUI_MAX_EVENTS;
    s_events[idx] = (gui_event_t){ type, code, x, y, win, owner };
    s_ev_count++;
}
```

### 3.5 事件按 owner 隔离

`GUI_OP_POLL` 不只是"取走全部事件"，而是**按属主过滤 + 就地压缩**（main.c:1070-1100）：

```c
u32 n = s_ev_count, src = s_ev_head, dst = s_ev_head;
for (u32 i = 0; i < n; i++) {
    gui_event_t e = s_events[src];
    src = (src + 1) % GUI_MAX_EVENTS;
    if (e.owner != 0 && e.owner != caller) {
        s_events[dst] = e;                /* 不是我的：留在环里 */
        dst = (dst + 1) % GUI_MAX_EVENTS;
    } else {
        resp->events[resp->count++] = e;   /* 我的或广播：交付 */
    }
}
s_ev_head  = dst;
s_ev_count = n - resp->count;
```

语义要点：

1. **`owner == 0` 是广播**：无窗口目标的事件（`win == 0`）以及**无法解析属主**的事件，所有轮询客户端都能看到。
2. **`GUI_EV_CLOSE` 实际上是广播**：`DoDestroy` 先释放槽位并 `memset` 清零（main.c:797-799），之后才 `EvPush(GUI_EV_CLOSE, ..., closed_id)`（main.c:806）；此时 `WinOwner(closed_id)` 已经查不到该窗口，`owner` 落到 0。因此收到 CLOSE 的客户端必须自己比对 `event.win`（gui_demo 就是这么做的，gui_demo/main.c:185-191）。
3. **KEY 事件在无焦点时也是广播**：`s_focus_id == 0` 时 `win = 0` → `owner = 0`，所有客户端都会看到按键。
4. 一个客户端最多一次拿到 32 个事件（响应结构上限），`DoPoll` 的注释也把这一点作为"响应不会溢出"的理由。

### 3.6 错误码

| 值 | 符号 | 出现位置 |
|----|------|----------|
| 0 | `OK` | 成功（CREATE 例外，返回窗口 id） |
| -1 | `ERR_NOMEM` | 窗口槽位满、`malloc` 失败、RESIZE 分配失败 |
| -2 | `ERR_INVAL` | 载荷过短、尺寸越界、对最小化窗口 FOCUS、未知 op |
| -4 | `ERR_NOENT` | 窗口不存在或调用者不是属主 |
| ≥1 | 窗口 id | 仅 CREATE 的 `ret` |

（常量定义：`user/lib/libos/syscalls.h:40-47`。）

---

## 四、合成器实现

### 4.1 窗口几何

窗口矩形由三部分构成（main.c:96-106、265-403、418-580）：

```
      w->x                                    w->x + w->w + 2*GUI_BORDER - 1
        │                                                          │
 w->y → ┌──────────────────────────────────────────────────────────┐ ← 上边框（GUI_BORDER=1px）
        │ 标题文本（x+1+3, y+1，8x16 ASCII / 16x16 CJK）  [x][□][–] │ ← 标题栏（GUI_TITLE_H=16px）
 w->y+17├──────────────────────────────────────────────────────────┤
        │                                                          │
        │        内容区（客户端绘制，离屏 32bpp 缓冲 w×h）          │
        │                                                          │
        └──────────────────────────────────────────────────────────┘ ← 下边框
        总外框 = (w->w + 2) × (w->h + 2 + 16)
```

- 外框尺寸：`ww = w->w + 2*GUI_BORDER`，`wh = w->h + 2*GUI_BORDER + GUI_TITLE_H`（main.c:428-431）。
- 标题栏填充矩形：`(w->x+GUI_BORDER, w->y+GUI_BORDER, w->w, GUI_TITLE_H)`（main.c:492-500）。
- 内容区原点：`(w->x + GUI_BORDER, w->y + GUI_BORDER + GUI_TITLE_H)`（main.c:510-512）。
- 颜色：背景 `0x00202040`、边框 `0x00A0A0A0`、标题前景 `0x00FFFFFF`、焦点标题底 `0x00204080`、非焦点标题底 `0x00404040`、指针 `0x000000FF`、任务栏 `0x00202028`/`0x003C3C4C`（main.c:75-89）。

> `GUI_PTR_COLOR = 0x000000FF` 按 `0x00RRGGBB` 解读是**纯蓝**（R=0,G=0,B=0xFF），指针是 5×5 的实心方块（main.c:568-573）。源码行内注释 "xRGB word: red byte at bit 8" 描述的是 `GuiXrgb` 把**红色分量**放到 word 的 bit 8 这个布局事实，并非说该颜色是红色。

### 4.2 窗口表

```c
/* main.c:96-106 */
typedef struct {
    int  in_use;
    u64  owner;   /* 创建者 subject（内核填充，不可伪造） */
    int  id;
    char title[GUI_MAX_TITLE];       /* 64 字节，`DoCreate` 用 strncpy 截断 */
    i32  x, y;                       /* 边框左上角在 fb 中的位置 */
    i32  w, h;                       /* 内容区尺寸（不含边框与标题栏） */
    gui_canvas_t buf;                /* 离屏内容缓冲（32bpp） */
    int  maxed;                      /* 已最大化；还原矩形存 rx/ry/rw/rh */
    int  hidden;                     /* 已最小化：只以任务栏按钮存在 */
    i32  rx, ry, rw, rh;
} gui_win_t;
```

槽位选择是"第一个空闲槽"（`in_use == 0`），因此窗口在 `s_wins[]` 中的下标顺序反映创建顺序，Z 序依赖这一点（见 4.3）。

### 4.3 Z 序与焦点

- **没有显式的 stacking 列表**：Z 序 = 槽位顺序（后创建者在上），并且**焦点窗口被单独提升到最上层**。
- 合成分两趟（main.c:420-426）：第 0 趟按槽位顺序画所有"非焦点"窗口，第 1 趟画焦点窗口。因此焦点窗口永远盖在其他窗口之上（只被任务栏与指针盖住）。
- 焦点由三处修改：`DoFocus`（客户端显式设置）、输入线程的按下命中测试（`s_focus_id = hit`）、窗口销毁/最小化时清 0（main.c:796-798、1518-1519）。
- 视觉反馈：焦点窗口标题栏底色 `GUI_TITLE_BG`，非焦点用 `GUI_TITLE_BG_IDLE`（main.c:496-499）；焦点窗口才绘制三个标题栏按钮（main.c:367）。

### 4.4 脏区合成 vs 全量合成

脏区是**单个并集包围盒**（`s_dirty` + `s_dirty_valid`，main.c:185-193），不是矩形链表：

```c
/* GuiDirtyAddNolock：裁剪到 fb 边界后并入包围盒（main.c:195-235） */
if (!s_dirty_valid) { s_dirty = {x,y,w,h}; s_dirty_valid = 1; return; }
/* 否则取两者的最小外接矩形 */
```

- `GuiDirtyAdd()` 自己加锁；`GuiDirtyAddNolock()` 供**已持锁**的调用者使用——因为内核 mutex 不可重入（main.c:183-187 的注释），拖动等路径在持锁时继续调用 `GuiDirtyAdd` 会释放自己仍持有的锁。
- `GuiComposite()` 在 `!s_active || !s_dirty_valid` 时直接返回，然后**取走并清零** `s_dirty_valid`，只重绘这一个矩形（main.c:405-412）。

| 操作 | 记录的脏矩形 | 源码 |
|------|--------------|------|
| CREATE | 新窗口整框 | main.c:762-764 |
| DESTROY | 被销毁窗口整框 | main.c:807 |
| MOVE | 旧位置 ∪ 新位置 | main.c:859-861 |
| RESIZE | 旧框 ∪ 新框 | main.c:917-920 |
| FOCUS | 旧焦点窗口 ∪ 新焦点窗口（标题栏换色） | main.c:966-978 |
| FILL | 内容区上对应矩形 | main.c:1009-1010 |
| TEXT | `GuiTextWidth(text) × 16` | main.c:1048-1049 |
| 鼠标移动 | 旧指针 ∪ 新指针的 7×7 方框 | main.c:1323-1324 |
| 拖动 | 每次移动的旧框 ∪ 新框 | main.c:1338-1346 |
| 任务栏变化 | 底部 22px 整条 | main.c:655-657 |
| ACTIVATE | 整个屏幕 | main.c:1143 |

全量合成只在 `ACTIVATE` 时发生一次；此后一切重绘都受脏区约束。这套机制是"第十一轮补充 1"的产物：此前每次事件都重绘整个 1024×768（约 3 MB 写）导致 VNC 每帧全屏 dirty、刷新率过低（`docs/test_report.md`）。

代价与注意点：

1. 包围盒语义意味着**两次相距很远的小改动会退化为一次大矩形重绘**（例如先动左上角窗口、再动右下角窗口）。
2. 脏区只影响"重绘哪些像素"，**不影响"画什么"**：`GuiComposite` 对每个与该矩形相交的窗口都执行一次完整语义的绘制（边框 4 条边逐边裁剪、标题栏 fill 裁剪、内容 blit 裁剪），只是被裁剪到矩形内。

### 4.5 合成顺序

```
1. 背景：dirty 矩形内填 GUI_BG_COLOR                       (main.c:415-416)
2. 窗口（两趟）：
   pass 0：非焦点窗口，按槽位升序
   pass 1：焦点窗口（置顶）
   每个窗口内：边框(4 条边，逐边与 dirty 求交) → 标题栏 fill(dirty 裁剪) + 标题文本(逐行/逐列裁剪)
               → 内容 blit(dirty ∩ 内容矩形)                 (main.c:418-518)
3. 任务栏：屏幕底部 GUI_TASKBAR_H=22px 一条，画在所有窗口之上    (main.c:521-567)
4. 指针：5×5 实心方块，画在最上面                                (main.c:568-575)
```

任务栏必须最后画，理由是它承载"最小化窗口的唯一恢复入口"——即使某个窗口被最大化到几乎铺满屏幕，任务栏也不能被盖住（main.c:84-85、521-523）。最大化高度正是按此推导：`nh = fb.h - GUI_TASKBAR_H - 2*GUI_BORDER - GUI_TITLE_H`，1024×768 下为 728，加框后总高 746 = 768−22，正好停在任务栏上沿（main.c:1427-1428）。

### 4.6 标题栏与三个按钮

标题文本（`DrawTitlebar`，main.c:265-403）：

1. 起点 `(w->x + GUI_BORDER + 3, w->y + GUI_BORDER)`，右侧预留 42px（`max_px = xr - 42 - tx`，注释 "3 buttons * 14px"）给按钮（main.c:266-271）。
2. 按**显示宽度**截断：ASCII 8px、CJK（`cp > 0x7F && font_cjk_lookup(cp)` 命中）16px，保证不把一个 UTF-8 字符切成两半（main.c:272-294）。
3. 追加 `" *"`（空格 + 星号）：`t[n++] = ' '; t[n++] = '*'; /* focus marker */ `（main.c:293-294）。**该标记是无条件追加的**，并非按焦点门控——真正的焦点视觉信号是标题栏底色（见第九章 L3）。缓冲区大小为 `GUI_MAX_TITLE + 2`。
4. 逐行渲染，只画与 dirty 矩形相交的行（y 方向裁剪）与列（x 方向裁剪，main.c:305-360）。这两级裁剪是为修复"悬停在下层窗口标题栏时下层标题穿透到上层窗口"的缺陷（`docs/test_report.md` 第十一轮补充 5）。
5. CJK 字模按行读取两个字节（`glyph[r*2]` 高位 8 列、`glyph[r*2+1]` 低位 8 列）；字模缺失时把码点替换成 `'?'`。

标题栏按钮（main.c:363-402 绘制、1055-1068 命中、1462-1541 处理）：

| 按钮 | 位置 | 图形 | 行为 |
|------|------|------|------|
| 关闭（`x`） | `cx = xr - 7`（最右） | 两条对角线 | 释放窗口缓冲、清槽位、若为焦点则清焦点；**若这是最后一个窗口则自动退出桌面** |
| 最大化（`□`） | `cx = xr - 21` | 方框轮廓 | 记下还原矩形后重分配为全屏内容区并移到 (0,0)；再点一次还原 |
| 最小化（`–`） | `cx = xr - 35` | 一条横杠 | `hidden = 1`、清焦点、任务栏标脏 |

其中 `xr = w->x + w->w + GUI_BORDER - 1`，按钮底 `by = w->y + GUI_BORDER + 3`，绘制块 13×10 px（`xx ∈ [-6, +6]`）、命中区 `px ∈ [cx-6, cx+6)` 即 12px（main.c:365-380、1055-1068）——**最右 1 列画得出但点不中**（见第九章 L3）。

按钮**只在焦点窗口上绘制，也只对焦点窗口生效**：非焦点窗口上点击按钮区域会退化成"聚焦该窗口"（main.c:1462-1465 的注释与判定）。这一门控来自提交 `6ff36da`（gui: 修复标题栏按钮焦点门控…）。

### 4.7 拖动、双击最大化、最小化

**标题栏拖动**（main.c:1251-1600 的输入线程）：

- 按下落点满足 `in_title`（`py ≥ w->y` 且 `py < w->y + GUI_BORDER + GUI_TITLE_H`，即含 1px 上边框的标题条带）时启动拖动，记录 `s_drag_offx/offy = 指针 − 窗口原点`（main.c:1528-1533）。
- 拖动中指针移动直接改写 `dw->x/y`，并把窗口钳制在屏幕内（`nx + ww > fb.w` 等分支，main.c:1332-1352），脏区为旧框 ∪ 新框。
- 释放按钮结束拖动（`s_drag_id = 0`，main.c:1560-1561）。

**双击最大化**（main.c:1400-1445）：判定条件全部满足才触发——落点在标题条带、**目标就是当前焦点窗口**、不在按钮上、与前一次按下的时间差 `< 30` 个 tick、坐标差 `< 8px`。时间基准来自 `GetTime()`，即调度器 tick；PIT 被初始化为 100 Hz（`kernel/kernel_main.c:125` `PitInit(100)`、`kernel/sched/sched.c:339-344`），所以阈值约为 **300 ms**。触发后与"最大化"按钮走同一段代码（保存/恢复矩形 + `GuiWinRealloc`）。

**最小化与任务栏**：

- 任务栏固定高 22px、底色 `0x00202028`，最小化窗口在 `bx` 从 4 开始的横向排布中占一个按钮：宽 `12 + GuiTextWidth(title)`（上限 `GUI_TASKBAR_MAX_W` = 200），间距 4px，高 12px（`GUI_TASKBAR_H - 10`）（main.c:521-567）。
- 命中测试 `TaskbarButtonAt`（main.c:632-653）用**完全相同的排布算式**重算按钮范围，任何改一处都必须同步改另一处（源码注释明确说明）。
- 点击任务栏按钮：恢复 `hidden = 0`、标脏窗口与任务栏、**并夺取焦点**，同时推送一个 `GUI_EV_BUTTON code=1`、`win = 该窗口` 的事件（main.c:1368-1391）。任务栏优先于普通命中测试，因为最小化窗口不参与命中测试。

### 4.8 命中测试与穿透修复

`HitTest(px, py)`（main.c:1234-1249）必须**与合成顺序完全一致**：先测焦点窗口（它画在最上层），再按槽位**降序**测其余窗口；`hidden` 窗口跳过。

```c
static int HitTest(i32 px, i32 py) {
    gui_win_t *f = win_find(s_focus_id);
    if (f && f->in_use && !f->hidden &&
        px >= f->x && px < f->x + f->w + 2*GUI_BORDER &&
        py >= f->y && py < f->y + f->h + 2*GUI_BORDER + GUI_TITLE_H)
        return f->id;
    for (int i = GUI_MAX_WINDOWS - 1; i >= 0; i--) { /* 降序 = 自上而下 */
        ...
    }
    return 0; /* 桌面 */
}
```

被修复的三类"穿透"缺陷（提交 `6970f45`、`f0c1261`，记录于 `docs/test_report.md` 第十一轮补充 4/5）：

| 现象 | 根因 | 修复 |
|------|------|------|
| 两个窗口重叠时下层边框/标题"透"到上层 | 合成时按整窗绘制边框/标题，未裁剪到脏区 | 4 条边框逐边与脏区求交后用 `GuiHline/GuiVline` 画；标题栏 fill 裁剪 |
| 悬停在下层标题栏时下层标题文字透出 | 字符按整 16px 高、整 8px 宽绘制 | `DrawTitlebar` 改为**逐字符、逐行 y clip、逐列 x clip**，只画与脏区相交的像元 |
| 点击落在被上层遮挡的下层窗口上 | 命中测试顺序与绘制顺序不一致 | 命中测试改为"焦点优先 + 槽位降序"，与合成顺序对齐 |

### 4.9 缓冲重分配

`GuiWinRealloc(w, nw, nh)`（main.c:593-619）被 RESIZE、最大化、还原三处共用：

1. `malloc(nw*nh*4)` + `memset` 清零；失败返回 -1（不破坏旧缓冲）。
2. 拷贝左上角重叠区 `min(nw,old_w) × min(nh,old_h)`，按各自 pitch 逐像素搬运。
3. `free` 旧缓冲，更新 `buf.buf/w/h/pitch/bpp` 与 `w->w/h`。

关键不变量：**离屏缓冲尺寸必须始终等于 `w/h`**。源码注释指出，最大化时若先改 `w/h` 再分配（或分配失败仍继续），合成时的 blit 会越界读取，窗口只渲染出一部分（main.c:586-590）。因此最大化代码显式"先 realloc 再改坐标与 `maxed` 标志"（main.c:1433-1444）。

### 4.10 线程模型与加锁

| 线程 | 入口 | 职责 |
|------|------|------|
| 服务线程 | `GuiServerLoop`（main.c:1186-1213），优先级由 `ThreadCreate(``GuiInputMain`, NULL, 10)` 之外的默认 | `IpcRecvFrom` 阻塞接收 → `Do*` → `IpcReply` |
| 输入线程 | `GuiInputMain`（main.c:1251-1598），优先级 10（main.c:1634-1636） | 轮询键盘与鼠标、维护指针/焦点/拖动，`Sleep(1)` 节流 |

- 共享状态 = 窗口表 + 事件环 + 指针 + 拖动状态，统一由 `s_lock`（`MutexCreate()`，main.c:1631-1633）保护。
- `s_lock` 是**内核 mutex，不可重入**，所以写了两套脏区入口（`GuiDirtyAdd` / `GuiDirtyAddNolock`）。
- `GuiShutdown()`（main.c:1149-1173）会在内部 IPC 调用 keyboard（`KBD_OP_RELEASE_FOCUS`）与 term（`TERM_OP_REDRAW`），因此**必须在未持 `s_lock` 时调用**（main.c:1145-1148 的注释）。
- 输入循环在 `!s_active` 时 `Sleep(5)` 空转，激活后每轮 `Sleep(1)`（main.c:1255-1259、1595-1596）。1 tick = 10 ms（PIT 100 Hz），因此光标跟随的最坏延迟量级为 10 ms 加上两次 IPC 往返。

---

## 五、输入路径

### 5.1 设备与驱动归属

**鼠标驱动不在 gui 内部，而在 keyboard 服务**：PS/2 鼠标与键盘共用同一个控制器（I/O 端口 0x60/0x64），因此由 `user/services/keyboard/keyboard.c` 同时拥有 IRQ1 与 IRQ12（keyboard.c:120-126、822-842）。

```
  PS/2 控制器（0x60 数据 / 0x64 状态）
     │  IRQ1（键盘，PIC 主片）/ IRQ12（鼠标，PIC 从片）
     ▼
 keyboard 服务 IRQ 线程（KbdIrqMain，keyboard.c:771-846）
     ├─ WaitNotification(KBD_IRQ_MASK | MOUSE_IRQ_MASK)  两个 IRQ 共用一个通知掩码
     ├─ KbdRxDrain()：循环读状态口，OBF 未置位即停
     │     状态 bit5 = 1 → MouseParse(byte)     ← 同一个端口区分设备
     │     状态 bit5 = 0 → KbdDecodeByte(scancode)：scancode set-1 → ASCII（shift/caps/ctrl/alt 状态机）
     └─ 路由：焦点属主的 park 槽（阻塞读）优先，否则推入 KX 环（keyboard.c:321-338、452-462）
```

鼠标初始化序列 `MouseInit()`（keyboard.c:677-766）：`0xA8`（启用辅助接口）→ 读命令字节并置 bit1（辅助口 IRQ）+ bit5（辅助口时钟）写回 → `0xD4 0xF6`（设备默认值）→ IntelliMouse 采样率魔术 `200 → 100 → 80` 切换到 4 字节包 → `0xD4 0xF4`（开启流模式）。每一步都等 ACK，并在结束后把包相位复位，避免 ACK 混进包状态机。

包解析 `MouseParse()`（keyboard.c:474-494）：3 字节标准包（4 字节模式追加滚轮字节），byte0 的 bit3 必须为 1（否则丢包重新同步），bit4/bit5 是 X/Y 的符号位（9 位二补码），低 3 位是左/右/中键，第 4 字节是有符号滚轮量；结果**累加**到 `s_mouse_dx/dy/wheel` 与 `s_mouse_buttons`。

### 5.2 keyboard 服务的相关协议

| op | 名称 | 请求 | 响应 | gui 的用法 |
|----|------|------|------|-----------|
| 1 | `KBD_OP_READ` | `len` = 最大字节数 | `{i32 ret; u8 data[n]}`，非阻塞，空环返回 0 | 输入线程每轮取最多 8 字节（main.c:1262-1277） |
| 2 | `KBD_OP_READ_BLOCK` | 同上 | 空环时挂入 park 表，由 IRQ 线程完成 | gui 不用 |
| 3 | `KBD_OP_TAKE_FOCUS` | 忽略 | 0 | `ACTIVATE` 时调用（main.c:1129-1136） |
| 4 | `KBD_OP_RELEASE_FOCUS` | 忽略 | 0 / `ERR_NOCAP` | `DEACTIVATE` / 末窗退出时调用（main.c:1151-1158） |
| 5 | `KBD_OP_MOUSE_READ` | `len` = 16 | 4×i32 `{dx, dy, buttons, wheel}`，**读取即清零** | 输入线程每轮取一次（main.c:1281-1296） |

gui 侧只镜像了用到的两个 op 号常量（`KBD_OP_READ 1`、`KBD_OP_MOUSE_READ 5`，main.c:71-73），并在源码注释里声明这是**协议镜像而非共享头文件**——改 keyboard 协议时必须同步改这里。

### 5.3 键盘焦点路由

keyboard 服务维护 `s_focus_owner`（keyboard.c:196-201）：

- `TAKE_FOCUS` 把调用者设为唯一键盘属主；`RELEASE_FOCUS` 只接受当前属主的释放（否则 `ERR_NOCAP`）。
- 按键路由 `kbd_park_target()`（keyboard.c:321-338）：**有焦点属主时只服务该属主的 park 槽；该属主没有 park（gui 用的就是非阻塞 `KBD_OP_READ`）则返回 NULL → 按键落到 RX 环**，绝不回退给其他 park 者（否则 shell 的 `read_line` 会把按键吞掉）。
- 因此 GUI 桌面的键盘链路是：`TAKE_FOCUS` 之后按键进 RX 环 → gui 输入线程用非阻塞读轮询取走 → 打包成 `GUI_EV_KEY`。shell 的 `read_line` 全程停在 park 表不受打扰，焦点释放后立即恢复（与 `docs/tui_design.md` §12.3 描述的 wm 模型一致）。

按键字节的取值区间（keyboard.c:126-135、355-395）：

| 区间 | 含义 |
|------|------|
| `0x20..0x7E` | 可打印 ASCII（受 shift/caps 影响） |
| `\n`、`\b`、`\t` | Enter / Backspace / Tab |
| `0x01..0x14` | 扩展键映射：Home=0x01、PgUp=0x02、End=0x05、PgDn=0x06、Up=0x0B、Down=0x0C、Left=0x10、Right=0x14（不采用多字节转义序列） |
| `0x80..0x9F` | Ctrl 组合（Ctrl-A=0x81 … Ctrl-Z=0x9A，Ctrl-空格=0x80） |
| `0xE0..0xEF` | Alt+字母 |

GUI 对这些字节**不做任何过滤或转换**，原样放进 `event.code`。

### 5.4 gui 输入线程的事件生成

```
每轮（~1 tick）：
  1. KBD_OP_READ(8) → n 个字节 → 每个字节 EvPush(KEY, byte, ptr_x, ptr_y, s_focus_id)
       （n 被钳到 8；多余的留在 keyboard 的 RX 环里下一轮取）
  2. KBD_OP_MOUSE_READ(16) → {dx, dy, buttons, wheel}
       wheel != 0 → EvPush(WHEEL, wheel, ptr, HitTest(ptr))
       dx|dy != 0 → s_ptr_x += dx; s_ptr_y -= dy   /* PS/2 的 Y 向上为正 */
                    钳制到 [0, fb.w-1] × [0, fb.h-1]
                    EvPush(MOUSEMOVE, 0, ptr, HitTest(ptr))
                    若在拖动 → 移动窗口（钳制 + 脏区）
                    标脏 旧指针 7x7 ∪ 新指针 7x7
       buttons 变化 → 按下：任务栏 → 命中测试 → 双击判定 → 标题栏按钮 → 拖动启动 → 焦点切换
                               → EvPush(BUTTON, 1, ptr, 目标窗口)；记录 s_press_grab
                     释放：EvPush(BUTTON, 0, ptr, s_press_grab ? : HitTest(ptr))；s_press_grab = 0；结束拖动
  3. changed → GuiComposite()
```

**隐式 grab（implicit pointer grab）**：按下时把目标窗口记进 `s_press_grab`，释放事件**送给该窗口而不是当前指针下的窗口**——否则一次跨过邻居窗口结束的拖动会误点邻居（main.c:1563-1569 的注释）。

### 5.5 时序图

```
 物理设备        keyboard 服务                gui 服务                     客户端
    │  IRQ1/12       │                            │                            │
    ├───────────────►│ 解码/组包 → 焦点属主 park 或 RX 环                          │
    │                │◄──── KBD_OP_READ / MOUSE_READ（gui 输入线程，~10ms 一轮）──┤
    │                ├───────────────────────────►│ 命中测试/焦点/拖动          │
    │                │                            │ EvPush(type,code,x,y,win)   │
    │                │                            │   └─ owner = 目标窗口属主    │
    │                │                            │◄──── GUI_OP_POLL ───────────┤
    │                │                            ├──── events[owner∈{caller,0}]►│
    │                │                            │                            │
    │                │                            │◄──── GUI_OP_FILL/TEXT ──────┤
    │                │                            │ 写离屏缓冲 → 标脏 → 合成      │
```

---

## 六、CJK/UTF-8 文本渲染

### 6.1 双字模路径

| 字模 | 定义 | 覆盖范围 | 单元尺寸 | 查找方式 |
|------|------|----------|----------|----------|
| ASCII | `user/lib/libgui/font.h` 的 `gui_font[95][16]` | `0x20..0x7E`（95 个可打印字符） | 8×16，每行 1 字节，MSB 在左 | `gui_font[cp - 0x20][row]` |
| CJK | `user/lib/font_cjk.h` 的 `cjk_font[CJK_FONT_COUNT]` | `CJK_FONT_COUNT` = 6844 个码点（GB2312 6763 汉字 + 符号，含制表符/箭头） | 16×16，每行 2 字节（高 8 列 + 低 8 列） | `font_cjk_lookup(cp)` 二分查找，未命中返回 NULL |

字模以 `static const` 表形式直接编进最终 ELF：`gui.c` 用相对路径 `#include "../../lib/font_cjk.h"`（gui.c:186），`gui/main.c` 同样包含它用于标题栏（main.c:68）。**代价是每个用户 ELF 都带上一份约 220 KB 的 CJK 字模表**——因为 Makefile 的 `USER_SHARED_OBJ` 把 `user/lib/libgui/gui.c` 链进了所有用户进程（见第九章 L8）。

### 6.2 GuiText 的解码与绘制

`GuiText`（gui.c:235-271）是一个**增量 UTF-8 解码器**，不依赖 libc 的 `utf8.c`：

| 首字节 | 处理 | 后续字节数 |
|--------|------|-----------|
| `0x00..0x7F` | 直接作为码点 | 0 |
| `0xC2..0xDF` | `cp = b & 0x1F` | 1 |
| `0xE0..0xEF` | `cp = b & 0x0F` | 2 |
| `0xF0..0xF4` | `cp = b & 0x07` | 3 |
| 其他 `≥ 0x80` | 视作非法，直接画 `'?'` | — |
| 续字节非 `10xxxxxx` | 丢弃当前序列（`left = 0`） | — |

单码点绘制 `GuiTextCodepoint`（gui.c:189-233）：

- `cp > 0x7F` 且 `font_cjk_lookup` 命中 → 16×16 双宽字形，前景/背景各画两半；**找不到字模时降级为 `'?'`（8px）**，而不是画空白。
- `0x20..0x7E` → `gui_font` 的 8×16 字形。
- 其余（控制字符、未命中且非 CJK 的码点）→ **推进 8px 但不画任何像素**。
- `bg == 0` 时只写前景像元，实现"透明文本"（标题栏靠它保留底色，main.c:264-266 的注释也说明标题栏填完底色后只画文字）。

### 6.3 GuiTextWidth

`GuiTextWidth`（gui.c:273-310）用与渲染器相同的状态机遍历字符串并累加宽度：

| 码点类别 | 宽度 |
|----------|------|
| `cp > 0x7F` 且在 CJK 字模中 | 16 |
| `cp > 0x7F` 但字模缺失（渲染成 `'?'`） | 8 |
| `0x20..0x7E` | 8 |
| 残留的单个 `≥ 0x80` 字节 | 8 |
| 控制字符（`< 0x20`） | **0** |

这张表解释了它为什么必须查字模（"宽度必须与实际渲染一致"，gui.c:286-288 的注释）。一处**已知不一致**：控制字符渲染时推进 8px 而 `GuiTextWidth` 计 0（见第九章 L4）。

任务栏按钮宽度、`DoText` 的脏区宽度、标题栏截断三处都用它作为"显示宽度"的唯一权威（main.c:538、1049、277-288）。

### 6.4 标题栏的裁剪渲染

标题栏不走 `GuiText`，而是自己光栅化（main.c:265-403），原因有两个：一是需要**透明背景**（底色已经填过，直接 `GuiPixel(&s_fb,...)` 写前景像元即可，不需要经过窗口缓冲）；二是需要**逐行/逐列裁剪到脏区**，避免 4.6 节所述的"下层标题穿透"。CJK 行取字节的方式与 `GuiTextCodepoint` 完全一致（`glyph[r*2]` / `glyph[r*2+1]`），保证两处渲染结果一致。

---

## 七、ACTIVATE / DEACTIVATE 与 term 文本屏的切换

### 7.1 framebuffer 的所有权

framebuffer 没有"锁"，只有**约定 + 门禁**：

| 主体 | 是否映射 fb | 何时写 | 依据 |
|------|-------------|--------|------|
| 内核 | 映射（`KERNEL_VIRT_BASE` 偏移） | 仅开机清屏 | kernel/gfx/framebuffer.c:70-92、149-172 |
| term 服务 | 映射（`TERM_FB_VA = 0x400000000`，term.c:149、2089） | 响应 `TERM_OP_*` 时 | tui_design §12.1 称其为"显示所有者" |
| gui 服务 | 映射（`0x60000000`，libgui gui.c:66-67） | **仅在 `s_active` 为真时** | main.c:405-406 的早退、1116-1147 |
| 其他进程 | 不可映射（`SYS_FB_GET_INFO` 返回 `ERR_NOCAP`） | — | kernel/syscall/syscall.c:669-674 |

两个服务可以**同时持有**有效映射；"谁拥有屏幕"完全由"谁在写"决定。gui 选择在非激活期完全静默（`GuiComposite` 直接返回，输入线程空转），从而把屏幕让给 term。

### 7.2 切换时序

```
客户端                gui 服务                       keyboard 服务        term 服务
  │ GUI_OP_ACTIVATE      │                              │                  │
  ├─────────────────────►│ s_active=1；指针移到屏幕中心；│                  │
  │                      │ s_focus_id=0；buttons=0      │                  │
  │                      ├── KBD_OP_TAKE_FOCUS(3) ──────►│ s_focus_owner=gui│
  │                      │ 标脏整屏 → GuiComposite()     │                  │
  │◄──── ret=0 ──────────┤                              │                  │
  │                      │                              │                  │
  │ GUI_OP_DEACTIVATE    │                              │                  │
  ├─────────────────────►│ GuiShutdown():               │                  │
  │                      ├── KBD_OP_RELEASE_FOCUS(4) ──►│ 属主比对通过→清0  │
  │                      │ s_active=0；丢弃待处理脏区；focus=0             │
  │                      ├── TERM_OP_REDRAW(11) ──────────────────────────►│ 从 s_cells 全屏重绘
  │◄──── ret=0 ──────────┤                              │                  │
```

细节与依据：

- `DoActivate` 忽略 `caller`（main.c:1119 `(void)caller;`）——**任何客户端都能激活/关闭桌面**，没有"桌面会话所有权"概念。
- 指针位置在激活时被强制设为屏幕中心（`fb.w/2, fb.h/2`），因为合成器不读鼠标绝对位置，只累加相对增量，需要一个起点（main.c:1124-1129）。
- `GuiShutdown` 先释放键盘焦点、再置 `s_active=0` 并**丢弃待处理脏区**（`s_dirty_valid = 0`，注释："屏幕已交还"），最后才让 term 重绘（main.c:1149-1173）。顺序很重要：如果先重绘 term，gui 输入线程可能在 `s_active` 仍为真的窗口里再标脏并覆盖文本屏。
- `GuiShutdown` 必须在不持 `s_lock` 时调用（内部会做两次 IPC 往返）。
- **末窗自动退出**：两条路径都会走 `GuiShutdown`——`DoDestroy` 销毁最后一个窗口（main.c:808-812），以及输入线程的标题栏关闭按钮关掉最后一个窗口（`shutdown` 标志，main.c:1470-1487、1573-1580）。后者显式跳过本次合成，因为屏幕已经交还 term。
- `DEACTIVATE` **不销毁窗口**：`s_wins[]` 与离屏缓冲原样保留，再次 `ACTIVATE` 会整屏标脏重合成，窗口全部回来（前提是期间没有客户端调 `DESTROY`）。

### 7.3 边界与风险

| 场景 | 行为 |
|------|------|
| gui 已激活时，term 被要求渲染（例如 shell 打印） | term 会按自己的字符网格直接覆盖像素画面；gui 不会察觉，只会在下次脏区重绘时覆盖其中的一部分，可能留下文本残影。协议层没有仲裁 |
| `ACTIVATE` 时 framebuffer 是 VGA 文本模式 | `GuiFbOpen` 直接返回 `ERR_INVAL`，服务启动即 `ThreadExit(1)`（gui.c:56-61、main.c:1605-1609） |
| 启动时 `PortGet("keyboard")` 失败 | 输入线程仍运行但两个分支都被 `s_kbd_port >= 0` 挡住 → 桌面只有画面、没有输入；源码注释写 "lazily retried" 但实现**没有重试**（main.c:1611-1616） |
| 启动时 `PortGet("term")` 失败 | `DEACTIVATE` 无法恢复文本屏（`s_term_port < 0` 时跳过 REDRAW），画面停留在最后合成的像素 |
| 客户端异常退出（崩溃）而复位的窗口仍在 | `gui` 没有任何"属主死亡清理"逻辑：窗口会永久留在窗口表里（占满 8 个槽位后新窗口全部 `ERR_NOMEM`），且因为 owner subject 已失效，其他客户端也无法销毁它 |

---

## 八、客户端开发

### 8.1 最小可编译骨架

客户端**没有独立的协议头**，需要按相对路径包含服务端头文件（gui_demo 就是这么做的，gui_demo/main.c:27）。以下骨架与 `gui_demo/main.c` 使用完全相同的 API 与调用约定，可直接放入 `user/services/gui_hello/main.c`：

```c
/*
 * gui_hello - 最小 GUI 客户端骨架（OpSys v0.8-dev）
 * 依赖：user/services/gui/gui.h（协议）、libc/stdio.h、libc/string.h、libos/syscalls.h
 */
#include "../gui/gui.h"

#include <libc/stdio.h>
#include <libc/string.h>
#include <libos/syscalls.h>

#define WIN_W 320
#define WIN_H 200

static int s_port = -1;

/* 统一 RPC：请求 = {u32 op; u32 len; u8 data[]}，实际发送 8 + payload_len 字节 */
static int Rpc(u32 op, const void *payload, u32 payload_len, void *resp, int resp_cap)
{
    static gui_req_t req;                     /* 静态：避免 4 KiB 栈帧 */
    memset(&req, 0, sizeof(req));
    req.op  = op;
    req.len = payload_len;
    if (payload_len && payload)
        memcpy(req.data, payload, payload_len);
    int rlen = resp_cap;
    return IpcCall(s_port, &req, (int)(8 + payload_len), resp, &rlen);
}

static int Activate(void)
{
    gui_resp_t r;
    memset(&r, 0, sizeof(r));
    if (Rpc(GUI_OP_ACTIVATE, NULL, 0, &r, sizeof(r)) < 0)
        return -1;
    return r.ret;
}

static int Deactivate(void)
{
    gui_resp_t r;
    memset(&r, 0, sizeof(r));
    if (Rpc(GUI_OP_DEACTIVATE, NULL, 0, &r, sizeof(r)) < 0)
        return -1;
    return r.ret;
}

static int WinCreate(const char *title, int w, int h)
{
    gui_req_create_t c;
    gui_resp_t       r;
    memset(&c, 0, sizeof(c));
    memset(&r, 0, sizeof(r));
    strncpy(c.title, title, sizeof(c.title) - 1);
    c.w = w;
    c.h = h;
    /* CREATE 成功时 ret = 窗口 id（>= 1），失败为负 */
    if (Rpc(GUI_OP_CREATE, &c, sizeof(c), &r, sizeof(r)) < 0 || r.ret < 0)
        return -1;
    return (int)r.ret;
}

static int WinDestroy(int id)   { gui_resp_t r; memset(&r, 0, sizeof(r));
                                  return Rpc(GUI_OP_DESTROY, &id, 4, &r, sizeof(r)); }

static int WinMove(int id, int x, int y)
{
    i32        a[3] = { id, x, y };
    gui_resp_t r;
    memset(&r, 0, sizeof(r));
    return Rpc(GUI_OP_MOVE, a, sizeof(a), &r, sizeof(r));
}

/* GUI_OP_RESIZE / GUI_OP_FOCUS 的封装与 WinMove 同构：
 *   gui_req_resize_t s = { id, w, h };  Rpc(GUI_OP_RESIZE, &s, sizeof(s), &r, sizeof(r));
 *   i32 id = win;                       Rpc(GUI_OP_FOCUS,  &id, 4, &r, sizeof(r));
 * 注意 RESIZE 后必须自己重绘新暴露的区域。 */

static int WinFill(int id, int x, int y, int w, int h, u32 color)
{
    gui_req_fill_t f;
    gui_resp_t     r;
    memset(&f, 0, sizeof(f));
    memset(&r, 0, sizeof(r));
    f.id = id; f.x = x; f.y = y; f.w = w; f.h = h; f.color = color;
    return Rpc(GUI_OP_FILL, &f, sizeof(f), &r, sizeof(r));
}

/* 注意载荷长度：offsetof(text)=16，因此 17 + strlen 即够；
 * 这里沿用 gui_demo 的 20 + strlen + 1（多出的字节为零，服务端渲染到 NUL 即停）。 */
static int WinText(int id, int x, int y, const char *s, u32 fg, u32 bg)
{
    gui_req_text_t t;
    gui_resp_t     r;
    memset(&t, 0, sizeof(t));
    memset(&r, 0, sizeof(r));
    t.id = id; t.x = x; t.y = y; t.fg = fg; t.bg = bg;
    strncpy(t.text, s, sizeof(t.text) - 1);
    return Rpc(GUI_OP_TEXT, &t, (u32)(20 + strlen(t.text) + 1), &r, sizeof(r));
}

static int Poll(gui_resp_poll_t *ev, int *count)
{
    memset(ev, 0, sizeof(*ev));
    if (Rpc(GUI_OP_POLL, NULL, 0, ev, sizeof(*ev)) < 0)
        return -1;
    *count = (int)ev->count;
    return 0;
}

/* GUI_OP_POINTER：i32 *p = (i32 *)r.data; p[0]=x, p[1]=y, p[2]=buttons（1/2/4 位掩码） */

int main(void)
{
    s_port = PortGet(GUI_PORT_NAME);
    if (s_port < 0) {
        printf("gui_hello: 'gui' port unavailable (%d)\n", s_port);
        return 1;
    }
    if (Activate() < 0) {
        printf("gui_hello: ACTIVATE failed\n");
        return 1;
    }

    const int wx = 40, wy = 40;               /* 自己记住窗口位置，用于坐标换算 */
    int win = WinCreate("Hello GUI", WIN_W, WIN_H);
    if (win < 0) {
        printf("gui_hello: CREATE failed\n");
        Deactivate();
        return 1;
    }
    WinMove(win, wx, wy);
    WinFill(win, 0, 0, WIN_W, WIN_H, 0x00101018);
    WinText(win, 8, 8, "q quits; click or wheel to draw", 0x00E0E0E0, 0);

    for (;;) {
        gui_resp_poll_t ev;
        int             n = 0;
        if (Poll(&ev, &n) < 0)
            break;
        for (int i = 0; i < n; i++) {
            gui_event_t *e = &ev.events[i];
            switch (e->type) {
            case GUI_EV_KEY: {
                int ch = (int)(u8)e->code;          /* 单字节 ASCII/控制码 */
                if (ch == 'q' || ch == 27)
                    goto done;
                if (e->win != win)                  /* 键盘跟随焦点窗口 */
                    break;
                printf("gui_hello: key %d\n", ch);
                break;
            }
            case GUI_EV_BUTTON: {
                int lx, ly;
                if (e->code != 1)                   /* 1 = 按下, 0 = 释放 */
                    break;
                /* 绝对坐标 -> 窗口内容区局部坐标 */
                lx = e->x - (wx + GUI_BORDER);
                ly = e->y - (wy + GUI_BORDER + GUI_TITLE_H);
                if (lx >= 0 && ly >= 0 && lx < WIN_W && ly < WIN_H)
                    WinFill(win, lx - 3, ly - 3, 7, 7, 0x00FF8040);
                break;
            }
            case GUI_EV_WHEEL:
                printf("gui_hello: wheel %d at %d,%d win=%d\n",
                       (int)e->code, e->x, e->y, e->win);
                break;
            case GUI_EV_CLOSE:
                if (e->win == win)                  /* 合成器关了我们的窗口 */
                    goto done;
                break;
            case GUI_EV_MOUSEMOVE:
            default:
                break;
            }
        }
        (void)Sleep(1);                              /* 轮询节流：1 tick = 10ms */
    }

done:
    WinDestroy(win);   /* 已被合成器关闭时返回 -4（ERR_NOENT），可忽略 */
    Deactivate();
    printf("gui_hello: bye\n");
    return 0;
}
```

### 8.2 接入构建

新增一个 GUI 客户端需要改四处（以 `gui_hello` 为例）：

| 位置 | 改动 |
|------|------|
| `Makefile` `USER_C` 列表 | 加 `user/services/gui_hello/main.c`（Makefile:144-149 附近） |
| `Makefile` `USER_SVC_ENTRY_OBJ` | 加 `build/user/services/gui_hello/main.c.o`（它定义自己的 `main()`，必须从共享对象里排除，Makefile:174-208） |
| `Makefile` `SVC_NAMES` + 链接规则 | 加 `gui_hello`；加 `$(eval $(call SVC_LINK_RULE,gui_hello,gui_hello/main.c.o))`（Makefile:206、287-288） |
| `kernel/blob/blob.c` | 加 `extern char gui_hello_elf_*[]` 与 `BLOB_REG("gui_hello", ...)`（blob.c:60-115） |

之后可用 shell 的 `exec`/`process_create` 路径启动，或仿照 `CmdGui`（user/services/shell/shell.c:2454-2480）注册一条新命令——它做三件事：`BlobGet("gui_demo")` 取 ELF、`ProcessCreate` 拉起、`ProcessWait` 等它退出。

> `gui_demo` **不在** `s_svc_blobs[]` 白名单里（kernel/syscall/process_desc.c:314-318），所以它拿不到 `ATOM_SERVICE_MANAGE`，也就无法映射 framebuffer（`SYS_FB_GET_INFO` 返回 `ERR_NOCAP`）。这是有意的安全边界：客户端只能通过 IPC 画自己的窗口，绝不可能全屏乱写。因此客户端**不能**直接调用 libgui 的 `GuiFbOpen`。

### 8.3 坐标与事件语义速查

| 概念 | 坐标/取值 |
|------|-----------|
| `FILL/TEXT/RESIZE` 的 `x,y,w,h` | 窗口**内容区局部**坐标，原点在内容区左上角 |
| 事件 `x,y` | framebuffer **绝对**像素坐标（`MOUSEMOVE/BUTTON/WHEEL` 都是指针位置；`KEY` 是按键时指针位置） |
| 绝对 → 局部 | `lx = e.x - (win_x + GUI_BORDER)`；`ly = e.y - (win_y + GUI_BORDER + GUI_TITLE_H)` |
| `event.win` | `KEY`：焦点窗口 id；鼠标：命中窗口 id；`CLOSE`：被关窗口 id；0 表示无目标 |
| `event.owner` | 目标窗口属主 subject；0 表示广播（务必用 `win` 二次过滤） |
| 按键判定 | `BUTTON` 的 `code` 只有 1/0（按下/释放）；左右中键请查 `GUI_OP_POINTER` 的 `buttons`（1/2/4） |

### 8.4 事件循环建议

1. **轮询 + `Sleep(1)`**：`GUI_OP_POLL` 非阻塞，没有阻塞式等待版本；`Sleep(1)` 把忙等限制在 100 Hz（gui_demo/main.c:234 即此写法）。
2. **先判 `win` 再处理**：键盘事件会随焦点漂移，`q`/`Esc` 之类的全局退出键是 gui_demo 唯一的"焦点无关"处理（gui_demo/main.c:192-203）。
3. **自己维护窗口位置**：协议没有"窗口被拖动/移动"的事件，客户端一旦用 `MOVE` 或用户拖动改变了位置，就必须自己更新用于坐标换算的 `win_x/win_y`（拖动是合成器内部行为，客户端无法得知，见第九章 L5）。
4. **所有 op 的返回值都要看**：窗口被合成器关闭后继续 `FILL/TEXT` 只会持续拿到 `-4`。
5. **退出顺序**：`DESTROY` 所有窗口 → `DEACTIVATE`（让 term 重绘文本屏）→ 进程退出。若最后一个窗口已被合成器关闭，合成器会自动 `GuiShutdown`，此时再调 `DEACTIVATE` 是幂等的（`s_active` 已为 0，`GuiShutdown` 再执行一次同样的清理）。

---

## 九、已知限制

| 编号 | 限制/缺陷 | 依据 |
|------|-----------|------|
| L1 | `GuiFbOpen` 未对 `fb_size` 做页对齐，而 `SYS_FB_MAP` 要求 `size % PAGE_SIZE == 0`；term 则显式向上取整。只有 `pitch * height` 恰为 4096 倍数时才能映射成功（1024x768x32：4096×768 = 3 MiB 恰好整页；例如 800x600x32 的 3200×600 = 1,920,000 字节就不是 4096 的倍数，会 `ERR_INVAL`） | gui.c:62-69 vs kernel/syscall/syscall.c:712-713、user/services/term/term.c:2079-2087 |
| L2 | 24bpp framebuffer 下合成器实际不可用：窗口缓冲恒为 32bpp，而 `GuiBlit` 在 `dst->bpp != src->bpp` 时静默返回 → 窗口内容永远画不出来（只有背景/边框/标题栏可见）。当前 GRUB 配置为 `1024x768x32`，因此未暴露 | gui.c:318-319、main.c:746-750、boot/grub.cfg:15 |
| L3 | 标题栏的 `" *"` 是无条件追加，并非焦点标记（真正的焦点信号是标题栏底色）；按钮绘制宽 13px 而命中宽 12px，最右 1 列画得出、点不中 | main.c:293-294、365-380 vs 1055-1068 |
| L4 | 控制字符的渲染与宽度不一致：`GuiText` 对其推进 8px，`GuiTextWidth` 计 0（`\t` 会画出一个空位但后续脏区宽度少算） | gui.c:217-220 vs gui.c:303-307 |
| L5 | 没有窗口几何变更事件（无 CONFIGURE 类事件）：客户端无法得知窗口被拖动或被合成器最大化/还原，绝对坐标到局部坐标的换算会失效 | 全协议仅 `GUI_EV_{KEY,MOUSEMOVE,BUTTON,WHEEL,CLOSE}`（gui.h:106-116） |
| L6 | 合成器关闭按钮关闭窗口时**不推送 `GUI_EV_CLOSE`**（只有客户端自己 `DESTROY` 才推，且因槽位已清零而变成广播）；客户端只能靠后续 op 返回 `-4` 发现窗口没了。末窗自动退出后客户端也收不到通知，会在 `GUI_OP_POLL` 上空转（gui_demo 的 `q` 依赖 KEY 事件，此时不会到达） | main.c:806（唯一 CLOSE 推送点）vs main.c:1467-1487（关闭按钮路径无 EvPush）、main.c:1255-1257（非激活时输入线程停摆） |
| L7 | 每个响应固定 4096 字节（`IpcReply(sizeof(gui_resp_t))`），`POLL` 之外的 op 有 4 KiB 的无用拷贝；`gui.h` 的 `req->len` 字段服务端从不读取 | main.c 各 `Do*` 的 `IpcReply` 调用 |
| L8 | `user/lib/libgui/gui.c` 被链接进**所有**用户进程（`USER_SHARED_OBJ`），包括约 220 KB 的 CJK 字模表，而只有 gui 服务能用 fb 路径 | Makefile:130-210、gui.c:185-186 |
| L9 | 任何客户端都能 `ACTIVATE`/`DEACTIVATE`（`DoActivate` 忽略 caller），没有桌面会话所有权；也没有属主死亡清理，崩溃客户端的窗口会永久占用槽位 | main.c:1116-1147、772-816 |
| L10 | 事件环仅 32 项且队满即丢；`POLL` 无阻塞版本，客户端只能轮询（`Sleep(1)` ≈ 10 ms 延迟） | main.c:161-162、1070-1100 |
| L11 | 滚轮分支在未持 `s_lock` 的情况下调用 `EvPush`/`HitTest`（对比键盘、按键路径均加锁） | main.c:1291-1294 |
| L12 | `DrawTitlebar` 的栈缓冲 `t[GUI_MAX_TITLE+2]` 在极端标题下最多越界 3 字节：循环判据是 `n < sizeof(t)-3`，但单次 `memcpy` 最多写 4 字节（4 字节 UTF-8 序列），随后无条件写 `' '`、`'*'`、`'\0'`。触发需要长且含 4 字节序列的标题（例如 16 个不在 CJK 字模中的 4 字节码点，逐字宽 8px，仅需窗口内容区宽 ≥ 约 174px） | main.c:272-295（`t` 的定义、判据、`memcpy` 与随后两次无条件写） |
| L13 | 键盘/term 端口只在启动时解析一次，注释声称 "lazily retried" 但实现没有重试；两服务未就绪时对应功能永久失效 | main.c:1611-1616 |
| L14 | 窗口上限 8、无窗口策略（无吸附/无层/无约束）；任务栏只在存在最小化窗口时才有按钮；窗口不能全屏无边框（最大化仍保留标题栏） | gui.h:40、main.c:521-567、1427-1428 |
| L15 | 没有 alpha 混合与合成效果（无阴影/圆角/半透明）：`bg == 0` 只是"不画背景"，不是透明度；字体在合成器中未缓存，每次标题重绘都要重新逐位光栅化 | gui.c:217-230、main.c:265-403 |

---

## 十、后续计划

以下方向都直接对应第九章的具体限制，按"协议层 → 渲染层 → 生态"排列：

**协议与语义**

1. 把协议头从服务目录抽出为共享客户端头（`user/lib/libgui/gui_proto.h`），对齐 `wm` 的 `user/lib/libwm/wm_proto.h` 做法，消除 `#include "../gui/gui.h"` 与手抄的 `KBD_OP_*` 常量（main.c:71-73）。
2. 新增 `GUI_OP_GET_STATE`（`{active, focus, count}`）与窗口属性查询，让客户端能感知"桌面被关掉/窗口被合成器关闭"（修 L6、L9）。
3. 新增窗口几何事件（`GUI_EV_CONFIGURE`：拖动、最大化、RESIZE 后通知属主），彻底解决客户端局部坐标换算（修 L5）。
4. 补齐 CLOSE 语义：输入线程关闭路径也推 `GUI_EV_CLOSE` 并保留 owner（修 L6）；`GUI_EV_BUTTON` 的 `code` 按 `gui.h` 注释实现左右中键区分（修 L3 的注释不一致）。
5. `GUI_OP_POLL` 增加阻塞/超时变体（或 `GUI_OP_WAIT`），把客户端从轮询里解放出来（修 L10）；响应按实际长度回复而非固定 4096 字节（修 L7）。
6. 事件环扩容（`GUI_MAX_EVENTS`）或改为每窗口队列，避免队满丢弃（修 L10）。
7. 属主死亡清理：合成器订阅/查询 subject 存活状态，自动回收崩溃客户端的窗口（修 L9）。
8. 加桌面会话所有权（`ACTIVATE` 绑定首个调用者/管理面），避免任意客户端抢占屏幕（修 L9）。

**渲染与合成**

9. 窗口缓冲按 framebuffer 的实际 bpp 分配，或为 `GuiBlit` 增加 32↔24 转换路径（修 L2）。
10. `GuiFbOpen` 按 term 的写法把 `fb_size` 向上取整到页边界（修 L1）。
11. 脏区从"单一包围盒"升级为矩形链表或多矩形区域，减少大范围重绘（4.4 节的代价）。
12. 共享内存 Damage 模型：客户端把窗口缓冲放进共享内存并只上报脏矩形，替代当前"每个 FILL/TEXT 一次 IPC 往返"（当前每次绘制都要走一次完整 RPC）。
13. 字体缓存与位图缓存（标题栏/任务栏文本），以及 alpha 混合、圆角、阴影（修 L15）。
14. 修正 `DrawTitlebar` 的 `" *"` 门控与缓冲区越界判据（修 L3、L12）。

**功能与生态**

15. 窗口管理策略：层（always-on-top）、吸附、最小尺寸约束、无边框全屏模式、任务栏常驻按钮（修 L14）。
16. 把 `user/lib/libime/` 的拼音 IME 引入 GUI 客户端（目前只有 shell 使用，GUI 客户端拿到的只是单字节 ASCII/控制码，见 5.3 节的字节区间表），并定义"文本框"级组件。
17. 提取一层轻量 widget 库（按钮/输入框/列表）到 `user/lib/libgui`，让客户端不再每次手工 `WinFill/WinText`。

---

## 附录 A 常量速查

| 常量 | 值 | 定义位置 | 含义 |
|------|----|----------|------|
| `GUI_PORT_NAME` | `"gui"` | gui.h:39 | IPC 端口名 |
| `GUI_MAX_WINDOWS` | 8 | gui.h:40 | 窗口槽位上限 |
| `GUI_MAX_TITLE` | 64 | gui.h:41 | 标题字节数（含 NUL） |
| `GUI_TITLE_H` | 16 | gui.h:42 | 标题栏高度（像素，等于 ASCII 字模高） |
| `GUI_BORDER` | 1 | gui.h:43 | 边框厚度（像素） |
| `GUI_IPC_MAX` | 4096 | gui.h:44 | 请求/响应缓冲上限 |
| `GUI_MAX_EVENTS` | 32 | gui.h:45 | 事件环容量 |
| `GUI_TASKBAR_H` | 22 | main.c:86 | 任务栏高度 |
| `GUI_TASKBAR_MAX_W` | 200 | main.c:89 | 任务栏按钮最大宽度 |
| `KBD_OP_READ` | 1 | main.c:72 | 非阻塞键盘读 |
| `KBD_OP_MOUSE_READ` | 5 | main.c:73 | 鼠标增量读（读取即清零） |
| `KBD_OP_TAKE_FOCUS` | 3 | main.c:1132 | 夺取键盘焦点 |
| `KBD_OP_RELEASE_FOCUS` | 4 | main.c:1153 | 释放键盘焦点 |
| `TERM_OP_REDRAW` | 11 | main.c:1167 | 让 term 全屏重绘文本 |
| `SYS_FB_GET_INFO` | 44 | kernel/include/kernel/syscall_numbers.h:108 | 查询 fb 描述符（门禁） |
| `SYS_FB_MAP` | 45 | kernel/include/kernel/syscall_numbers.h:109 | 映射 fb（门禁） |
| `CJK_FONT_COUNT` | 6844 | user/lib/font_cjk.h:22 | CJK 字模数量 |
| PIT 频率 | 100 Hz | kernel/kernel_main.c:125 | `GetTime()`/`Sleep()` 的 tick = 10 ms |

交互几何速查（1024x768x32 为例）：

| 项 | 数值 |
|----|------|
| 屏幕 | 1024×768，pitch 4096（3 MiB） |
| 最大内容区 | 1022×750（= `fb.w − 2` × `fb.h − 2 − 16`） |
| 最大化内容区 | 1022×728（加框后总高 746，正好停在 22px 任务栏上方） |
| 单窗口最大缓冲 | 1022×728×4 ≈ 2.84 MiB（用户堆 256 MiB，见 kernel/include/kernel/vmm.h:33-38） |
| 双击判定窗口 | 30 tick ≈ 300 ms，位移 < 8px |

---

## 附录 B 事实来源与核对说明

本文所有结论均来自以下文件（行号以本仓库当前工作树为准）：

| 主题 | 文件 |
|------|------|
| 协议定义、常量、结构 | `user/services/gui/gui.h` |
| 合成器、窗口表、输入线程、生命周期 | `user/services/gui/main.c` |
| 像素库、颜色转换、字模路径 | `user/lib/libgui/gui.c`、`user/lib/libgui/gui.h`、`user/lib/libgui/font.h` |
| CJK 字模表与查找 | `user/lib/font_cjk.h` |
| 内核 framebuffer 驱动与用户描述符 | `kernel/gfx/framebuffer.c`、`kernel/include/kernel/framebuffer.h` |
| fb 系统调用与能力门禁 | `kernel/syscall/syscall.c:664-754`、`kernel/include/kernel/syscall_numbers.h:108-109` |
| blob 身份播种（`ATOM_SERVICE_MANAGE`） | `kernel/syscall/process_desc.c:294-332` |
| blob 注册表 | `kernel/blob/blob.c:60-118` |
| PS/2 键盘 + 鼠标驱动 | `user/services/keyboard/keyboard.c` |
| term 的 fb 映射与 REDRAW | `user/services/term/term.c` |
| 参考客户端 | `user/services/gui_demo/main.c` |
| 文本级对照实现 | `user/services/wm/main.c`、`user/lib/libwm/wm_proto.h`、`docs/tui_design.md` §12 |
| 服务启动顺序 | `user/services/manager/manager.c:120-150、531-541` |
| shell `gui` 命令 | `user/services/shell/shell.c:2454-2480` |
| 构建接入 | `Makefile:130-300` |
| GRUB 显示模式 | `boot/grub.cfg:15` |
| 历史验证记录 | `docs/test_report.md` 第十一轮及其补充 1-5 |
| 相关提交 | `7732671`(P1+P2)、`812e7ca`(P3+P4)、`0f0a328`(脏区)、`6bab0cb`(拖动)、`fd3a0f0`(焦点跟随)、`6970f45`/`f0c1261`(穿透修复)、`3ee600c`(CJK/RESIZE/grab/滚轮)、`5a9f416`(owner 隔离)、`8bb979c`(标题栏按钮)、`6ff36da`(按钮门控/任务栏/末窗退出) |

**版本说明**：本文描述的是**当前工作树**的代码。相对 git HEAD `105805d`，工作树中 `user/services/gui/` 与 `gui_demo/` 有未提交修改，本文已按工作树如实记录，具体包括：`GUI_EV_CLOSE`(5) 的定义与 `DoDestroy` 推送（gui.h:115、main.c:806）、`DoCreate` 改为按 framebuffer 推导内容区上限（main.c:667-682）、`DoFocus` 拒绝最小化窗口（main.c:946-953）、gui_demo 增加 CLOSE 处理与按键焦点过滤。

> 返回 [文档索引](README.md)
