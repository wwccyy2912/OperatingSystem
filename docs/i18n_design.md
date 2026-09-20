# OpSys 国际化（i18n）设计文档

> 适用版本：OpSys v0.8-dev（git HEAD 105805d）　|　最后更新：2026-09-19
>
> 本文记录 OpSys 从 ASCII-only 到全链路 UTF-8 的迁移设计：字符编码层（libc）、点阵字体与字形查表、TUI/GUI 渲染层（CELL 模型 + ANSI/VT100）、行编辑器（按码点编辑）、拼音输入法（libime）与 VFS 文件名存储，以及当前的已知限制与验证方法。

---

## 一、动机与范围

### 1.1 迁移前的事实

在提交 `ed9f082`（utf8: 全系统 UTF-8 支持——输入（拼音 IME）、编辑、渲染与存储）之前，系统各层都是 ASCII-only：

| 层 | 迁移前的行为 | 出处 |
| --- | --- | --- |
| 输入 | 键盘服务把 set-1 扫描码映射为 US-ASCII，**没有任何途径产生非 ASCII 字节** | `user/services/keyboard/keyboard.c:425-446` |
| 编辑 | 行编辑器按字节移动/删除光标，退格删 1 字节 | `user/services/shell/shell.c:1068-1101` |
| 渲染 | term 的 cell 只存 `u8` 字符，逐字节输出；`>0x7E` 一律画空格 | `user/services/term/term.c:425-431` |
| 渲染 | `cat` 把任何 `>= 0x80` 的字节替换成点号 | `ed9f082` 提交说明、`shell.c:3142-3181` |
| 存储 | 磁盘卷（virtio-blk）名字段 64 字节（63 可用） | `user/services/vfs/fs_virtio_blk_driver.c:762`、`ed9f082` 提交说明 |

因此“显示中文”与“输入中文”在迁移前都不可能，而不是仅仅“显示不好看”。

### 1.2 迁移范围：四条链路

代码事实上的 UTF-8 责任链是**四条**，任何一条断裂用户都会看到乱码或缺字：

```text
输入链                编辑链                 渲染链                存储链
──────────────────────────────────────────────────────────────────────────────
keyboard: ASCII        shell 行编辑器         term (TUI)            VFS 名字字段
+ Ctrl/Alt 波段          pos = 码点边界         32-bit CELL          name[256]
      │                 Utf8Prev/Next          + font_cjk.h             │
      ▼                       │                + ANSI 调色板           ▼
libime (拼音) ──提交──▶ 行缓冲(UTF-8 字节) ──▶ WRITE(逐字节) ──▶ 文件内容
      │                       │                增量 UTF-8 解码      (tee/cat 透传)
      └──TERM_OP_STATUS──▶ 状态栏候选          9x20 px cell 网格
                             │
                        libgui: GuiText/GuiTextWidth (8px ASCII / 16px CJK)
```

逐层的设计约束（均由代码决定，不是可选风格）：

1. **缓冲区是字节串**：行缓冲、VFS 名字、文件内容全是 `char *`，UTF-8 是唯一的执行字符集；没有 `wchar_t` 中转（`user/lib/libc/utf8.h:17-25` 的文件头注释即为这一设计声明）。
2. **位置必须是码点边界**：所有移动/删除/截断都要经过 `Utf8Prev`/`Utf8Next`，或“回退到字符边界”的截断算法。
3. **宽度必须一致**：渲染推进宽度与编辑器/状态栏计算的列宽必须一致，否则光标错位（见 §8.1 第 5 条）。
4. **候选必须可渲染**：IME 候选集是 `font_cjk.h` 字形集的子集（`user/lib/libime/ime_tab.h:19-21`）。

### 1.3 相关提交

| 提交 | 主题 | 与 i18n 的关系 |
| --- | --- | --- |
| `9f1171b` | tui: 阶段2 国际标准升级——ANSI 转义 + 颜色 cell + UTF-8/CJK 渲染 | cell 由 `u8` 升为 `u32`；term 增量 UTF-8 解码 + 6844 字形 16x16 CJK 双宽渲染 |
| `3ee600c` | gui: 阶段3 国际标准——CJK 渲染 + RESIZE + 隐式 grab + 滚轮 | `GuiText` 升 UTF-8 + 16x16 双宽；`GuiTextWidth` 返回真实像素宽 |
| `ed9f082` | utf8: 全系统 UTF-8 支持——输入（拼音 IME）、编辑、渲染与存储 | 本文主要对象：`libc/utf8.c`、`libime`、行编辑器、VFS 名字上限 64B→255B |

### 1.4 非目标

- 不做 locale 切换（`setlocale` 无实现，见 §2.8）；UTF-8 是硬编码的执行编码。
- 不做 Unicode 归一化（NFC/NFD）、双向文本（bidi）、复杂文本整形。
- 不做字形反走样或矢量字体：全部是点阵位图。
- 不做输入法联想、词组、云候选：`libime` 是单音节拼音查表。

---

## 二、字符编码层（libc）

### 2.1 分层

```text
应用（shell / term / gui / vfs）
        │
        ├── utf8.h        ← 字节串世界的码点边界与显示宽度（系统内真正被广泛调用的层）
        │
        └── wchar.h / uchar.h / wctype.h   ← C11 宽字符接口（兼容层，多为“声明齐备”）
                │
                └── wchar.c 内部再调用 utf8.h（wcwidth → Utf8CharWidth）
```

编译单元清单见 `Makefile:105-115`：`stdio.c stdlib.c string.c ctype.c inttypes.c time.c math.c threads.c wchar.c utf8.c wctype.c`。**注意没有 `locale.c`**。

### 2.2 utf8.h / utf8.c：逐函数

声明位于 `user/lib/libc/utf8.h:33-64`，实现位于 `user/lib/libc/utf8.c`。

| 函数 | 签名 | 行为（代码事实） | 位置 |
| --- | --- | --- | --- |
| `Utf8SeqLen` | `int Utf8SeqLen(const char *s)` | 返回 `s[0]` 起始序列的字节数：小于 0x80 得 1；0xC2..0xDF 得 2；0xE0..0xEF 得 3；0xF0..0xF4 得 4；否则 **0**（NUL / NULL / 游离续字节 / 非法首字节，含 0xC0、0xC1 与 0xF5..0xFF） | `utf8.c:36-49` |
| `Utf8Decode` | `int Utf8Decode(const char *s, uint32_t *cp)` | 解码一个码点写入 `*cp`（`cp` 可为 NULL），返回消耗字节数 1..4，非法返回 0。**只校验续字节 `(b & 0xC0) == 0x80`，不拒绝过长编码（overlong）与代理区** | `utf8.c:51-90` |
| `Utf8CharWidth` | `int Utf8CharWidth(uint32_t cp)` | 终端列宽：控制字符与 0x7F..0x9F 得 **0**；组合记号（0300-036F、1AB0-1AFF、20D0-20FF、FE00-FE0F）得 0；CJK 宽区间得 2；其余得 1 | `utf8.c:92-130` |
| `Utf8Prev` | `int Utf8Prev(const char *s, int pos)` | 返回 `pos` 之前那个字符的起始字节偏移：先回退所有续字节，再退 1 字节；`pos <= 0` 或无前驱返回 0 | `utf8.c:132-140` |
| `Utf8Next` | `int Utf8Next(const char *s, int pos, int len)` | 返回 `pos` 处（或之后）字符的下一个边界；`pos >= len` 返回 `len`；`Utf8SeqLen <= 0` 时按 1 字节前进；结果被钳到 `len` | `utf8.c:142-150` |
| `Utf8Advance` | `int Utf8Advance(const char *s, int pos, int len, int delta)` | 以码点为单位前后移动 `delta` 步（负数为向左），内部循环调用 `Utf8Prev`/`Utf8Next`。**当前仓库内无调用点**（只有定义） | `utf8.c:152-161` |
| `Utf8StrWidth` | `int Utf8StrWidth(const char *s, int len)` | 累加 `len` 字节内的显示列宽；解码失败的单字节记 **1** 列后前进 1 字节 | `utf8.c:163-178` |

调用点（全树检索结果）：`shell.c`（行编辑、`ShellRedrawLine`、补全与路径截断、`CatEmitText`）、`term.c`（`TermPutsUtf8`、`TermRenderLineAt`、BOX 标题居中）、`gui/main.c`（标题栏）、`libfs/fs.c`、`vfs_server.c`、`ime.c`、`wchar.c`。

### 2.3 非法输入策略：降级而不失败

`utf8.c` 头部注释明确了策略（`utf8.c:28-31`）：malformed sequences degrade to single-byte passthrough rather than failing hard。具体到调用方：

| 场景 | 行为 | 出处 |
| --- | --- | --- |
| term 收到游离续字节 | 渲染问号占位 | `term.c:1090-1094` |
| term 交叉 WRITE 的半个字符 | 解码状态保留，等下一个 WRITE 补齐 | `term.c:1066-1080` |
| term 序列中途出现非续字节 | 丢弃已积累的部分（`s_utf8_left = 0`） | `term.c:1076-1078` |
| `TermPutsUtf8` 解码失败 | 该字节自身按 1 列渲染 | `term.c:508-512` |
| gui `GuiText` | 游离字节渲染问号占位；解码失败丢弃 | `user/lib/libgui/gui.c:255-270` |
| `cat` | 非法序列打印等量点号；跨 1 KiB 块保持状态 | `shell.c:3142-3181` |

### 2.4 显示宽度表

`Utf8CharWidth` 判定为 **2 列**的区间（`utf8.c:108-126`）：

| 区间 | 名称（取自代码注释） | 字体是否有字形 |
| --- | --- | --- |
| 1100-115F | Hangul Jamo | 无（实测 0） |
| 2E80-303E | CJK Radicals..CJK Symbols | 部分（实测 9） |
| 3041-33FF | Hiragana..CJK Compatibility | 无（实测 0） |
| 3400-4DBF | CJK Ext A | 无（实测 0） |
| 4E00-9FFF | CJK Unified | **6763** |
| A000-A4CF | Yi | 无 |
| AC00-D7A3 | Hangul Syllables | 无 |
| F900-FAFF | CJK Compat Ideographs | 无 |
| FE30-FE4F | CJK Compatibility Forms | 无 |
| FF00-FF60 | Fullwidth Forms | 21 |
| FFE0-FFE6 | 全角符号 | 1 |
| 20000-2FFFD、30000-3FFFD | CJK Ext B 及以上 | 无，且 `cjk_glyph_t.u` 是 `uint16_t`，结构上不可能表示 |

“宽度等于 2 但没有字形”的区间是当前光标错位的根因，见 §8.1。

### 2.5 wchar.c：宽字符与多字节

`wchar.h` 声明了 C11 §7.29 的完整接口；`ed9f082` 之前 `mbs*`/`wcrtomb`/`wcwidth` 只有声明。当前**已实现**的部分：

| 分类 | 函数 | 行为要点 | 位置 |
| --- | --- | --- | --- |
| 宽串长度与比较 | `wcslen wcscmp wcsncmp` | 与窄串同构；`wchar_t` 为 32 位 | `wchar.c:49-87` |
| 宽串复制与连接 | `wcscpy wcsncpy wcscat wcsncat` | 标准语义，`wcsncpy` 补 NUL | `wchar.c:89-124` |
| 宽串查找 | `wcschr wcsrchr wcsstr wcspbrk wcsspn wcscspn wcstok` | `wcstok` 用 `ptr` 保存续扫位置 | `wchar.c:126-238` |
| 宽内存 | `wmemchr wmemcmp wmemcpy wmemmove wmemset` | `wmemcpy`/`wmemmove` 委托 `memcpy`/`memmove`（长度为 `n * sizeof(wchar_t)`） | `wchar.c:240-271` |
| 宽数字 | `wcstol wcstoul wcstoll wcstoull` | 先把宽串逐字符截成窄缓冲（非 ASCII 被压成低字节），再调用 `strto*`，最后把结束偏移映射回 `wchar_t *` | `wchar.c:282-358` |
| 多字节状态 | `mbsinit` | `ps == NULL` 或 `__len == 0` 即初始态 | `wchar.c:367-369` |
| 解码 | `mbrtowc` | 见下方状态机 | `wchar.c:395-470` |
| 长度 | `mbrlen` | 等价于 `mbrtowc(NULL, s, n, ps)` | `wchar.c:472-474` |
| 编码 | `wcrtomb` | 码点大于 0x10FFFF 或为孤立代理返回 `(size_t)-1`；否则输出 1..4 字节 | `wchar.c:476-503` |
| 串转换 | `mbsrtowcs wcsrtombs` | 失败返回 `(size_t)-1` 并回写 `*src`；成功置 `*src = NULL` | `wchar.c:505-573` |
| 宽度 | `wcwidth` | 控制字符与 0x7F..0x9F 返回 **-1**（与 `Utf8CharWidth` 的 0 不同），其余委托 `Utf8CharWidth` | `wchar.c:577-582` |
| char32 | `mbrtoc32 c32rtomb` | 直接委托 `mbrtowc`/`wcrtomb` | `wchar.c:589-599` |
| char16 | `mbrtoc16 c16rtomb` | UTF-16 代理对，见下 | `wchar.c:601-656` |

`mbstate_t` 的形状（`wchar.h:38-41`）：

```c
typedef struct {
    char   __buf[8]; /* pending multibyte bytes */
    size_t __len;    /* valid bytes stored in __buf */
} mbstate_t;
```

```text
mbrtowc 状态机（wchar.c:395-470）
  s == NULL            -> 复位 __len = 0，返回 0
  n == 0               -> 返回 (size_t)-2
  __len > 0            -> 取出上次挂起的字节，与本次输入拼接
  首字节分类: 小于 0x80 得 1 | 小于 0xC2 得 -1 | 至多 0xDF 得 2
              至多 0xEF 得 3 | 至多 0xF4 得 4 | 其余得 -1
  字节不足 need        -> 存入 __buf，返回 (size_t)-2（状态跨调用保持）
  续字节校验失败       -> __len = 0，返回 (size_t)-1
  过长编码 / 代理区 / 大于 0x10FFFF -> (size_t)-1
  成功                 -> 写 *pwc，返回本次从 s 消耗的字节数 used
```

`mbrtoc16` 的返回值约定与标准一致：`(size_t)-3` 表示“本次不消耗字节，返回上一个星形字符的**低位代理**”（`wchar.c:609-615`）；`c16rtomb` 收到高位代理时把状态挂起并返回 0（`wchar.c:640-643`），孤立低位代理返回 `(size_t)-1`（`wchar.c:653-654`）。

**声明了但未实现**（全树检索无定义）：`wcsxfrm wcscoll wcstod`、宽格式化 I/O（`Fwprintf Fwscanf Wprintf Wscanf Swprintf Swscanf Vfwprintf Vwprintf Vswprintf`）、宽字符 I/O（`Fgetwc fgetws Fputwc Fputws Getwc Getwchar Putwc Putwchar Ungetwc`）、`Wcsftime`。`wchar.h` 在对应小节标注 “declarations only”。

命名上存在**混合风格**：经典函数保持小写（`wcslen`、`mbrtowc`、`iswalnum`、`towlower`），而部分接口按仓库的 PascalCase 约定改写（`Iswblank`、`Iswctype`、`Wctype`、`Wctrans`、`Towctrans`、`Fwprintf`、`Wcsftime`）——见 `wctype.h:41-52`、`wchar.h:113-124`。调用这些名字时不要照抄 C 标准头。

### 2.6 wctype.c：只有 7-bit ASCII

每个分类函数都是“范围检查加委托 `ctype.h`”：

```c
int iswalpha(wint_t wc) { return (wc <= 127) ? isalpha((int)wc) : 0; }
wint_t towlower(wint_t wc) { return (wc <= 127) ? (wint_t)tolower((int)wc) : wc; }
```

- 12 个分类函数（`iswalnum iswalpha Iswblank iswcntrl iswdigit iswgraph iswlower iswprint iswpunct iswspace iswupper iswxdigit`）对大于 127 的值一律返回 0（`wctype.c:32-67`）。
- 大小写映射对大于 127 的值**原样返回**（`wctype.c:73-78`）——中文没有大小写，这个行为是安全的。
- `Wctype("alnum".."xdigit")` 返回 1..12 的枚举值，`Iswctype` 分发（`wctype.c:84-162`）；`Wctrans("tolower"/"toupper")` 返回 1/2，`Towctrans` 分发（`wctype.c:164-186`）；未知属性返回 0。

结论：**宽字符分类不是 Unicode 分类**，只有 ASCII 子集可用。

### 2.7 uchar.h

`user/lib/libc/uchar.h:32-42`：

| 定义 | 值或行为 |
| --- | --- |
| `MB_LEN_MAX` | 4 |
| `char16_t` | `uint_least16_t` |
| `char32_t` | `uint_least32_t` |
| `mbstate_t` | 复用 `wchar.h` 中的定义 |

### 2.8 locale.h：接口齐备、实现缺失

`user/lib/libc/locale.h` 定义了 `LC_ALL/LC_COLLATE/LC_CTYPE/LC_MONETARY/LC_NUMERIC/LC_TIME`（0..5）、完整的 `struct lconv`（字段顺序对齐 C11 §7.11.2.1）与两个函数原型；文件头注释写明 “v0.1 ships only the "C" locale — setlocale() accepts "C" or "" …”（`locale.h:15-18`）。

但**代码事实是**：

| 检查项 | 结果 |
| --- | --- |
| 全树检索 `setlocale` / `localeconv` / `struct lconv` | 只命中 `user/lib/libc/locale.h` 自身 |
| 是否有源码包含 `locale.h` | 无 |
| libc 编译单元列表（`Makefile:105-115`） | 无 `locale.c` |

即**当前没有任何 `setlocale()` 或 `localeconv()` 实现**，任何程序调用它都会链接失败。系统实际行为等价于始终处于 C locale，UTF-8 支持不依赖 locale 设置。这是有意的设计（`utf8.c:28-31` 声明“无归一化、无 locale 切换”），但也是与 POSIX 的显式差异，见 §8.1 第 1、2 条。

### 2.9 覆盖度总表

| 能力 | 状态 | 证据 |
| --- | --- | --- |
| UTF-8 码点解码与编码（字节串层） | 已实现并在生产路径使用 | `utf8.c` 全文 |
| 显示宽度（列宽） | 已实现（近似 wcwidth） | `utf8.c:92-130`、`wchar.c:577-582` |
| 有状态多字节转换 | 已实现（跨调用状态、拒绝过长编码与代理） | `wchar.c:395-573` |
| UTF-16 / UTF-32 转换 | 已实现 | `wchar.c:589-656` |
| 宽字符串、宽内存、宽数字 | 已实现（数字仅适用于 ASCII 数字串） | `wchar.c:49-358` |
| 宽字符分类与大小写 | 仅 ASCII | `wctype.c` |
| 宽格式化 I/O、宽字符 I/O、`wcstod`、`Wcsftime` | 未实现（仅声明） | `wchar.h:113-150` |
| `setlocale` / `localeconv` | 未实现（无编译单元） | §2.8 |

---

## 三、字体与字形

### 3.1 三份点阵字模

| 文件 | 数组 | 字形尺寸 | 覆盖码位 | 用途 |
| --- | --- | --- | --- | --- |
| `user/services/term/font.h:22` | `static const uint8_t s_font[95][16]` | 8x16，1 字节一行 | 0x20..0x7E（95 个可打印 ASCII） | term 服务像素路径；也是 `tools/vga_decode.py` 解析的对象 |
| `user/lib/libgui/font.h:26` | `static const uint8_t gui_font[95][16]` | 8x16，1 字节一行 | 0x20..0x7E | `GuiText` 的 ASCII 分支 |
| `user/lib/font_cjk.h:28` | `static const cjk_glyph_t cjk_font[6844]` | 16x16，2 字节一行 | U+00A8..U+FFE5（BMP 子集） | term、libgui、gui 服务的 CJK 分支 |

前两个文件头部都注明 “Extracted from kernel/gfx/framebuffer.c（single source of truth is the kernel copy）”，即存在同源副本，改字模需同步。

### 3.2 font_cjk.h 数据规模与字形格式

```c
typedef struct { uint16_t u; uint8_t g[32]; } cjk_glyph_t;
#define CJK_FONT_COUNT 6844
```

| 属性 | 值 | 说明 |
| --- | --- | --- |
| 表项结构 | `{ uint16_t u; uint8_t g[32]; }` | 码位加 32 字节位图 |
| 单条大小 | 34 字节 | 实测 `sizeof(cjk_glyph_t) == 34`，无填充 |
| 总大小 | 6844 乘 34 等于 **232,696 字节**（约 227 KiB） | `9f1171b` 因此把 manager blob 缓冲 256 KB 提升到 512 KB |
| 位图布局 | 16 行乘 2 字节，行主序，MSB 在左 | 渲染取 `glyph[row*2]`（左 8 列）与 `glyph[row*2+1]`（右 8 列）逐位判断 |
| 排序 | 按码位**严格升序**（实测） | 二分查找的前提 |
| 存储类 | `static const` 定义在头文件中 | 每个包含它的编译单元各得一份副本（`term.c`、`libgui/gui.c`、`gui/main.c`） |
| 生成来源 | 头注释 “Auto-generated from Noto Sans Mono CJK SC” | `font_cjk.h:15-17` |

### 3.3 覆盖范围实测

对 `font_cjk.h` 的 6844 条表项做统计（本文写作时用脚本实测）：

| 区间或类别 | 条目数 |
| --- | --- |
| 全部 | 6844 |
| CJK Unified（4E00-9FFF） | **6763**（与头注释 “GB2312 6763 hanzi” 一致） |
| U+3000 以下的符号与标点 | 50（00A8 00B7 00D7 00F7 2014 2018 2019 201C 201D 2022 2025 2026 2190-2193） |
| 制表符（2500-257F） | 22（含 ─ │ ┌ ┐ └ ┘ ├ ┤ ┬ ┴ ┼ ═ ║ ╔ ╗ ╚ ╝ ╠ ╣ ╦ ╩ ╬） |
| 几何图形（25A0 起） | 10（■ □ ▲ △ ▶ ◀ ◆ ◇ ● ★ ☆ 中的 10 个） |
| 3000-303F（CJK 标点） | 9 |
| 全角（FF00-FF60） | 21 |
| 全角符号（FFE0-FFE6） | 1（FFE5 全角人民币符号） |
| 最大码位 | U+FFE5 |
| 最小码位 | U+00A8 |
| 3400-4DBF（Ext A）、平假名与片假名、Hangul、Yi、F900-FAFF | 0 |

两点结论：

1. 字体是 **BMP-only 且以 GB2312 简体中文为主**：日文假名、韩文、扩展区汉字都没有字形。
2. `cjk_glyph_t.u` 是 `uint16_t`，即使补数据也**无法**表示 U+10000 以上的码位（结构性限制，不是数据缺失）。

### 3.4 按码位查表：font_cjk_lookup

```c
static inline const uint8_t *font_cjk_lookup(uint32_t u) {  /* font_cjk.h:6876-6885 */
    int lo = 0, hi = CJK_FONT_COUNT - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        if (cjk_font[mid].u == u) return cjk_font[mid].g;
        if (cjk_font[mid].u < u) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;   /* 无字形 */
}
```

- 复杂度为 `O(log2 6844)`，约 13 次比较一个字符。
- 返回 `NULL` 是**所有渲染方统一的“缺字形”信号**：term 画问号占位（`term.c:439`）、gui 画问号并按 8px 推进（`gui.c:239`、`gui.c:281`）、`GuiTextWidth` 同步按 8px 计宽（`gui.c:290-300`）。

### 3.5 宽度推进：ASCII 8px / CJK 16px

两条渲染路径的网格与推进规则不同，但“宽是窄的两倍”是共同的：

| 维度 | term（TUI） | libgui（GUI） |
| --- | --- | --- |
| 网格 | cell 9x20 px | 无网格，逐像素 |
| ASCII 字形 | 8x16，占 1 个 cell（`term.c:369-384`） | 8x16，推进 8px（`gui.c:271-283`） |
| CJK 字形 | 16x16，跨 2 个 cell（18px），**居中偏移 1px**：画在 `cx*9+1`（`term.c:386-407`） | 16x16，推进 16px（`gui.c:239-270`） |
| 判宽依据 | `font_cjk_lookup(cp) != NULL` 才按宽处理（`term.c:513`、`term.c:671-676`） | 同上（`gui.c:242`、`gui.c:281`） |
| 列宽（供光标计算） | `Utf8CharWidth`，**按码位区间**，与字形无关 | `GuiTextWidth`，**按字形是否存在** |

term 的“居中偏移 1px”来自源码注释：两个 cell 覆盖 18px，而字形只有 16px，故 `x = cx*9 + (18-16)/2 = cx*9+1`（`term.c:386-390`）。

---

## 四、渲染层

### 4.1 TUI（term 服务）的 CELL 模型

`9f1171b` 把 cell 从 `u8` 字符升级为 32 位复合值（`term.c:179-192`）：

```text
 31        24 23        16 15                     0
┌────────────┬────────────┬────────────────────────┐
│   bg (8)   │   fg (8)   │      ch / 码位 (16)    │
└────────────┴────────────┴────────────────────────┘
   ANSI 索引     ANSI 索引     Unicode 码点（BMP）
                               0xFFFE = CELL_WIDE_CONT（宽字符续接格）
```

```c
#define CELL_CH(c)   ((c) & 0xFFFF)
#define CELL_FG(c)   (((c) >> 16) & 0xFF)
#define CELL_BG(c)   (((c) >> 24) & 0xFF)
#define CELL_SET(c, ch, fg, bg) \
    ((c) = ((u32)(ch) & 0xFFFF) | (((u32)(fg) & 0xFF) << 16) | (((u32)(bg) & 0xFF) << 24))
#define CELL_WIDE_CONT  0xFFFE
#define CELL_DEFAULT_FG 7      /* white */
#define CELL_DEFAULT_BG 0      /* black */
```

关键常量与缓冲：

| 项 | 值 | 出处 |
| --- | --- | --- |
| 屏幕缓冲 | `static u32 s_cells[128][256]`，128 KiB | `term.c:233` |
| 几何 | 列数等于宽度除以 9，行数等于高度除以 20（1024x768 得 113x38） | `term.c:231-232` |
| ANSI 调色板 | 16 项 XTerm 风格 RGB（首个为 0x00000000，末个为 0x00FFFFFF） | `term.c:157-162` |
| 默认前景与背景 | `TERM_FG 0x00FFFFFF`、`TERM_BG 0x00000000` | `term.c:153-154` |
| 状态栏行 | `TERM_STATUS_ROW = TERM_MAX_ROWS-1`，使用时取 `min(127, rows-1)` | `term.c:176`、`term.c:1153-1155` |
| 请求负载上限 | `TERM_MAX_DATA 256` 字节每次 WRITE | `term.c:132` |
| 快照与恢复上限 | `TERM_MAX_REGION_CELLS 2036` 格（u16 码点，受内核 4096 字节消息上限约束） | `term.c:134-140` |

码点存进 cell 时**不带宽度信息**：宽字符用“后续一格填 `CELL_WIDE_CONT`”表示，这是整个渲染层的核心不变式。

### 4.2 宽字符对不变式

```text
  列:        c         c+1          c+2
        ┌──────────┬────────────┬────────┐
汉字中: │ 0x4E2D   │ 0xFFFE     │   ...  │  0xFFFE 是续接格，TermDrawCell 直接 return
        └──────────┴────────────┴────────┘
              ▲
    16px 字形画在 px = c*9+1 .. c*9+16，正好覆盖 18px 的整对

维护规则（全部在 term.c 中）：
  TermClearCell(473-493)      清续接格时连带清 lead，反之亦然（不留半个字形）
  TermPutCodePoint(689-708)   写入前若目标格或其右邻构成宽字符对，先清整对
  退格处理(628-645)           落在续接格上时退到 lead 并清整对
  制表处理(646-655)           制表停靠落在续接格上时清整对
  CSI K 与 J(836-873)         擦除走 TermClearCell，故不会留半个字形
  TermCursorAnchorX(547-556)  光标在续接格时锚定 lead 列绘制与擦除
```

光标绘制用“反色重绘同一 cell”实现（`term.c:528-542` 的注释），因此“锚定 lead”这一处修正同时保证了绘制与擦除的对称。

### 4.3 ANSI/VT100 支持矩阵

`s_ansi_state` 是三态机：`ANSI_STATE_TEXT`、`ANSI_STATE_ESC`、`ANSI_STATE_CSI`（`term.c:243-252`）。CSI 参数最多 16 个，单参数钳到 9999，支持问号私有前缀与中间字符（`term.c:959-985`）。

| 序列 | 支持 | 行为 | 出处 |
| --- | --- | --- | --- |
| `ESC[m` 与 `ESC[0m` | 是 | 重置 fg 与 bg 到默认 | `term.c:723-727`、`term.c:731-734` |
| `ESC[1m`（bold） | 解析但不建模 | 保持 fg 不变 | `term.c:735-736` |
| `ESC[7m`（反显） | 是 | 交换 fg 与 bg | `term.c:737-743` |
| 30-37 与 90-97 | 是 | fg 取 0..7 或 8..15 | `term.c:744-761` |
| 39 | 是 | fg 回默认 | `term.c:748-750` |
| 40-47 与 100-107 | 是 | bg 取 0..7 或 8..15 | `term.c:751-765` |
| 49 | 是 | bg 回默认 | `term.c:755-757` |
| `38;5;n`（256 色与真彩） | **否** | 忽略（注释为 ignore for now） | `term.c:766-767` |
| A / B / C / D（CUU/CUD/CUF/CUB） | 是 | 缺省参数按 1，结果钳到屏内 | `term.c:783-812` |
| H 与 f（CUP/HVP） | 是 | 1-based，钳到 rows-1 与 cols-1 | `term.c:813-828` |
| G（CHA） | 是 | 列绝对定位，1-based | `term.c:829-835` |
| J（ED） | 是 | mode 0/1/2；0 与 1 逐格走 `TermClearCell` 后整屏重绘 | `term.c:836-862` |
| K（EL） | 是 | mode 0/1/2 | `term.c:863-874` |
| `?25h` 与 `?25l` | 是 | 光标显隐（`s_cursor_visible`） | `term.c:875-886` |
| 其它 CSI | 丢弃 | 默认分支直接 break | `term.c:887-888` |
| `ESC ] ...`（OSC） | 跳过 | 复用 ESC 态，忽略到 BEL | `term.c:940-944` |
| `ESC 7` 与 `ESC 8`（存/取光标） | 解析为空操作 | 注释为 keep simple: no saved state | `term.c:945-955` |
| `ESC c`（复位） | 忽略 | 两字节转义兜底分支 | `term.c:956` |

shell 直接使用的序列：`"\033[2J\033[H"`（Ctrl-L 清屏，`shell.c:1248`）、回车、退格、以及 CJK 双列擦除串 `"\b  \b"`（`shell.c:1094`）。

### 4.4 输出状态机与跨 WRITE 的 UTF-8 状态

```text
TermWrite(data, len)   term.c:1024-1099
   │  引导画面清除；若正翻历史则拉回实时并整屏重绘
   ▼
 逐字节循环
   ├─ s_utf8_left > 0（上一字节是 UTF-8 首字节）
   │     续字节 -> 累积码点；计数归零时，若 ANSI 处于 TEXT 态则 TermEmitCp(cp)
   │     非续字节 -> 丢弃已积累的部分（malformed）
   └─ 否则
         0xC2..0xDF -> left = 1 ; 0xE0..0xEF -> left = 2 ; 0xF0..0xF4 -> left = 3
         小于 0x80  -> TermPutc(b)，进入 ANSI 状态机
         其它       -> TermPutc('?')（游离续字节）
```

要点：

1. **状态跨 WRITE 调用保持**（`s_utf8_cp` 与 `s_utf8_left` 是文件级静态量）。这不是理论问题：shell 每次 WRITE 最多发 32 字节（`shell.c:128` 的 `TERM_CHUNK`），而 32 不是 3 的倍数，所以一个 3 字节汉字**确实会**跨两次 WRITE，靠这个状态拼接。
2. **UTF-8 文本不会出现在转义序列内部**：解码完成时若 ANSI 非 TEXT 态，码点被丢弃（`term.c:1070-1074`）。
3. 解码器**不做**过长编码与代理区校验（与 `mbrtowc` 不同），属于“宽容渲染”的选择。
4. `TERM_OP_CLEAR` 会复位 `s_utf8_left = 0` 与 `s_ansi_state = ANSI_STATE_TEXT`（`term.c:1112-1113`），避免清屏后残留半个字符或半条转义。

### 4.5 滚动历史（scrollback）

```c
#define SCROLLBACK_ROWS 200      /* term.c:263 */
static u32 s_sb[200][256];       /* 200 KiB —— 整行 cell 快照，保留颜色与 CJK 对 */
static u32 s_sb_next, s_sb_count, s_sb_view;
```

- `TermScroll()`（`term.c:580-611`）先把第 0 行 `memcpy` 进环形缓冲，再整体上移 `s_cells` 与帧缓冲。**整行 cell 拷贝**意味着翻回去的历史行仍然带着 ANSI 颜色与宽字符对。
- `TERM_OP_SCROLLVIEW`（`term.c:1508-1539`）：delta 大于 0 向历史翻页（钳到 `s_sb_count`）、小于 0 返回、等于 0 回实时并整屏重绘；delta 的绝对值先钳到 `SCROLLBACK_ROWS` 以防溢出。
- **任何 WRITE 都会把视图拉回实时**（`term.c:1033-1037`），所以 shell 的 `scroll` 分页器期间刻意不输出任何文本（`shell.c:1770-1830`，注释为 the pager is silent）。
- 历史渲染 `TermShowScrollback`（`term.c:1007-1020`）不修改 `s_cells`，退出视图靠 `TermRedrawScreen`。

### 4.6 TERM_OP 协议

`term.c:116-130` 定义，`user/lib/libtui/tui.h:39-49` 镜像为 `TUI_OP_*`（必须保持一致）。

| op | 名称 | 载荷与返回 | 与文本或 UTF-8 的关系 |
| --- | --- | --- | --- |
| 1 | `TERM_OP_WRITE` | `{len, data[len]}`，len 不超过 256 | UTF-8 字节流入口；状态机加增量解码 |
| 2 | `TERM_OP_CLEAR` | 无 | 清屏并**复位 UTF-8 与 ANSI 状态** |
| 3 | `TERM_OP_STATUS` | `{prefix_len, msg_len, prefix, msg}` | `TermRenderStatus` 走 `TermPutsUtf8`（CJK 双宽）；prefix 与 msg 分别钳到 63 与 127 字节 |
| 4 | `TERM_OP_BOX` | `{x,y,w,h,title_len,title}` | 边框是 ASCII 加号减号竖线（`term.c:1210-1226`）；标题按 `Utf8StrWidth` 居中（`term.c:1242-1246`），且钳到 63 字节 |
| 5 | `TERM_OP_RENDER_LINE` | `{x,y,text}` | `TermRenderLineAt` 先**回退到字符边界**再 `TermPutsUtf8`（`term.c:1265-1279`） |
| 6 | `TERM_OP_SET_CURSOR` | `{x,y}`（格坐标） | 光标是**列**坐标，编辑器据此对齐宽字符 |
| 7 | `TERM_OP_GET_CURSOR` | `{ret,x,y}` | shell 重绘时用于读取当前行号 |
| 8 | `TERM_OP_SNAPSHOT` | `{x,y,w,h}` 返回 `u16 cells[]` | `ed9f082` 之后按 **16 位码点**传输，CJK 经面板覆盖后不再退化为问号（`CELL_WIDE_CONT` 以 0xFFFE 往返） |
| 9 | `TERM_OP_RESTORE` | `{x,y,w,h,cells[]}` | 同上；每格 2 字节 |
| 10 | `TERM_OP_SCROLLVIEW` | `{i32 delta}` | 历史行含颜色与宽字符对 |
| 11 | `TERM_OP_REDRAW` | 无 | 从 `s_cells` 整屏重绘（GUI 合成器交还帧缓冲后调用） |
| 12 | `TERM_OP_GET_SIZE` | 返回 `{cols, rows}` | 供客户端做列宽与布局计算 |

`libtui` 侧的上限与 term 一致：`TUI_MAX_TEXT 256`、`TUI_MAX_REGION_CELLS 2036`（`tui.h:51-56`）。

### 4.7 光标与宽字符

- 光标是 (x, y) 格坐标；绘制方式是把该 cell 反色重绘（`term.c:528-542`）。
- 光标落在续接格时，矩形锚定到 lead 列；否则 16px 字形会被整体右移一格并覆盖下一个字符（源码注释给出的正是这个理由）。
- shell 的 `ShellRedrawLine` 用 `TERM_OP_SET_CURSOR` 把光标放到列 `pcols + pwidth`，两者都是**显示列**而非字节数（`shell.c:604-654`）。

### 4.8 libgui：GuiText 与 GuiTextWidth

`user/lib/libgui/gui.c`：

| 函数 | 行为 |
| --- | --- |
| `GuiText` | 自建增量 UTF-8 解码器（`left` 状态），逐码点调用 `GuiTextCodepoint`；`bg == 0` 时跳过背景像素（标题栏透明文本，`gui.h:68-71`） |
| `GuiTextCodepoint` | 码点大于 0x7F 时查 `font_cjk_lookup`：命中则 16x16 双宽、推进 16px（`bg` 非 0 时两半都刷背景）；未命中替换成问号并按 8px；码点在 0x20..0x7E 用 `gui_font` 画 8x16、推进 8px；其余（控制字符等）只推进 8px |
| `GuiTextWidth` | 与渲染**同一判宽规则**：仅当 `font_cjk_lookup` 命中才 16px，否则 8px；游离散字节按 8px |

“宽度函数与渲染器一致”是 `3ee600c` 明确修的点（提交说明：gui_text_width 与缺字形回退一致），目的是让脏区（dirty rect）宽度正确。

### 4.9 GUI 标题栏的 CJK 处理

`user/services/gui/main.c:262-330` 的 `DrawTitlebar`：

1. 逐码点按**显示宽度**截断到 `max_px`（右边界减 42px 再减起点，42px 是 3 个按钮乘 14px），保证 UTF-8 标题不会在字符中间被切断。
2. 追加焦点标记（空格加星号）。
3. 逐码点绘制：`font_cjk_lookup` 命中则 16px，未命中替换为问号并按 8px。
4. 按**脏区做 X 裁剪**（`cx0` 与 `cx1`）：只画落在脏区内的字形列，避免半覆盖的字形把尾部留在上层窗口上。

窗口标题字段是 `char title[GUI_MAX_TITLE]`，而 `GUI_MAX_TITLE` 等于 64（`user/services/gui/gui.h:41`、`gui.h:65`），即中文标题最多 21 个汉字（63 字节）。任务栏标签同理（`main.c:539-558`）。

---

## 五、编辑器与行输入

### 5.1 数据模型

```text
char buf[LINE_BUF_SIZE = 256]   ← 始终 NUL 结尾的 UTF-8 字节串
int  pos                        ← **字节**下标，但恒为码点边界
```

不变式：`pos` 永远指向某个字符的首字节（由 `Utf8Prev` 与 `Utf8Next` 保证）。这使得所有“按字节的 `memmove` 与移位”在语义上等价于“按码点编辑”。

相关常量（均在 `shell.c`）：`LINE_BUF_SIZE 256`（第 93 行）、`HIST_MAX 16` 与 `HIST_LEN 256`（第 111-112 行）、`TERM_CHUNK 32`（第 128 行）、`KBD_CHUNK 32`（第 133 行）、`COMPLETE_MAX_MATCHES 64`（第 98 行）、`FM_MAX_ITEMS 64`（第 99 行）、`PN_SEG_MAX 256`（第 1622 行）。

### 5.2 按键表

按键字节来自键盘服务（单字节：ASCII、控制码，以及 0x80 波段与 0xE0 波段，`keyboard.c:154-161`）：

| 按键字节 | 含义 | 处理 | 位置 |
| --- | --- | --- | --- |
| 回车 | Enter | 若正在拼拼音则**先提交候选**，再按一次才执行命令 | `shell.c:1038-1066` |
| 退格与 0x7F | Backspace | 见 §5.4（整码点删除加宽字符双列擦除） | `shell.c:1068-1101` |
| 制表符 | Tab | 补全（命令名或路径） | `shell.c:1103-1106` |
| 0x10 | 左箭头 | `pos = Utf8Prev(buf, pos)` 后重绘 | `shell.c:1194-1200` |
| 0x14 | 右箭头 | `pos = Utf8Next(buf, pos, strlen)` | `shell.c:1202-1208` |
| 0x0B 与 0x0C | 上箭头与下箭头 | 历史回退与前进 | `shell.c:1108-1162` |
| 0x01 与 0x05 | Home 与 End | `pos = 0` 或 `pos = strlen` | `shell.c:1164-1172` |
| 0x02 与 0x06 | PgUp 与 PgDn | 最旧与最新历史 | `shell.c:1174-1192` |
| 0x81 与 0x85 | Ctrl-A 与 Ctrl-E | 行首与行尾 | `shell.c:1211-1218` |
| 0x91 与 0x8B | Ctrl-U 与 Ctrl-K | 删到行首或行尾（`pos` 是码点边界，字节移位安全） | `shell.c:1219-1231` |
| 0x80 | **Ctrl+Space** | 切换拼音 IME | `shell.c:1232-1237` |
| 0x97 | Ctrl-W | 删前一个词（`ShellWordStart`，码点安全） | `shell.c:1238-1246` |
| 0x8C | Ctrl-L | 发送 `"\033[2J\033[H"` 后重绘 | `shell.c:1247-1250` |
| 0x83 与 0x84 | Ctrl-C 与 Ctrl-D | 取消当前行；空行 EOF 退出 | `shell.c:1251-1262` |
| 0xE2 与 0xE6 与 0xE4 | Alt-B、Alt-F、Alt-D | 按词左移、右移、删词（`ShellWordEnd`） | `shell.c:1263-1281` |
| 0x20..0x7E | 可打印 | IME 拦截判定后按普通插入（行中间插入会整行重绘） | `shell.c:1283-1358` |

注意：**没有任何按键能直接产生大于等于 0x80 的“文本字节”**（0x80 与 0xE0 波段被控制码占用），因此行缓冲里的 UTF-8 只能来自 IME 提交、历史回放、Tab 补全，或 `PS1` 与 cwd 中的中文。

### 5.3 重绘与光标列

`ShellRedrawLine(line, pos)`（`shell.c:604-654`）的步骤：

```text
1. pcols  = Utf8StrWidth(prompt)        ← 提示符的显示列数（cwd 可能是中文）
   lwidth = Utf8StrWidth(line)          ← 整行的显示宽度
   pwidth = Utf8StrWidth(line, pos)     ← 光标之前那一段的显示宽度
2. TERM_OP_GET_CURSOR（op 7）读取当前行号 row
3. 回车 + (pcols + lwidth + 4) 个空格 + 回车    ← 抹掉整行，erase_margin = 4
4. 重新打印 prompt 与 line
5. TERM_OP_SET_CURSOR（op 6）设为 (pcols + pwidth, row)   ← 用列坐标放置光标
```

若 term 端口不可用，退化为按字节数发退格（`shell.c:649-653`）——该回退路径对 CJK 不正确，但只在没有终端时触发。

### 5.4 退格：整码点删除与按显示宽度擦除

`shell.c:1068-1101`：

```c
int prev = Utf8Prev(buf, pos);   /* 整个码点的起始字节 */
int del  = pos - prev;           /* 1、2、3 或 4 字节 */
if (prev < (int)strlen(buf)) {   /* 行中间删除：尾部左移 del 字节后整行重绘 */
    for (int k = prev; buf[k] != '\0'; k++) buf[k] = buf[k + del];
    pos = prev; ShellRedrawLine(buf, pos);
} else {                         /* 行尾删除：按显示宽度擦列 */
    uint32_t cp; (void)Utf8Decode(buf + prev, &cp);
    ShellWrite(Utf8CharWidth(cp) == 2 ? "\b  \b"   /* 宽字符：退 1 列、清 2 列、再退 2 列 */
                                      : "\b \b");  /* 窄字符：退 1 列、清 1 列、再退 1 列 */
    buf[prev] = '\0'; pos = prev;
}
```

行尾分支的擦除宽度取自 `Utf8CharWidth`（按码位区间判定），而 term 的实际渲染宽度取自 `font_cjk_lookup`（按字形是否存在），两者在“宽区间但无字形”的码位上不一致（§8.1 第 5 条）。

### 5.5 词操作

`ShellWordStart`（`shell.c:778-785`）与 `ShellWordEnd`（`shell.c:788-795`）以空格字节为分隔，但每一步都走 `Utf8Prev` 或 `Utf8Next`，所以 CJK 词不会被从中间切开。源码注释原文：both bounds move by whole UTF-8 characters so CJK words are never split。Ctrl-W、Alt-B、Alt-F、Alt-D 都基于这两个函数。

### 5.6 历史记录与 Tab 补全

| 机制 | UTF-8 相关行为 | 出处 |
| --- | --- | --- |
| 历史入栈 | `strncpy` 整行字节拷贝加去重 `strcmp` | `shell.c:1500-1510` |
| 历史回放 | `strncpy(buf, s_history[v], maxlen-1)`，然后 `pos = strlen(buf)`（整串回放，落在串尾即边界） | `shell.c:1131-1134`、`1155-1157`、`1177-1190` |
| 文件名捕获 | `FmStrncpyUtf8`：截断后**回退到字符边界**，绝不产生半个字符 | `shell.c:2031-2047`（定义）、`927` 与 `2065`（调用） |
| 补全目录与片段 | 目录与片段分别经 `ShellResolvePath` 解析；最长公共前缀按字节比较 | `shell.c:885-957` |
| 补全插入 | 行中间补全用 `memmove` 保护光标后的文本（v1.3 修复的“无法编辑召回命令”缺陷） | `shell.c:841-871`、`942-957` |
| 匹配列表显示 | 直接 `ShellWrite` 名字（UTF-8 原样透传，term 负责双宽） | `shell.c:873-881`、`958-966` |

**不严谨点**：最长公共前缀是**逐字节**比较（`shell.c:833-838`、`930-936`），理论上可能合并出半截多字节字符并写回行缓冲（`shell.c:950-951`）。汉字首字节常相同（如 0xE4），触发需要两个候选的字节前缀恰好错位对齐，属低概率但可达的缺陷（§8.1 第 11 条）。

### 5.7 libtui 的输入行仍是字节级

`TuiInputLine`（`user/lib/libtui/tui.c`，用于 login 与 stop 等交互）：

- 只接受 `key >= ' ' && key < 0x7F` 的按键，退格执行 `pos--`（**按字节**）。
- 因此它**不支持 CJK 编辑**：没有 IME 集成，也没有码点边界处理。
- 渲染侧仍然是 UTF-8 感知的（`TuiRenderLineAt` 走 `TERM_OP_RENDER_LINE`），所以*显示*中文没有问题，只是输入受限。

---

## 六、输入法（拼音 IME）

### 6.1 为什么需要 IME

键盘服务只输出 ASCII（`keyboard.c:425-446`），因此“输入中文”必须由**用户态引擎**补齐：用 ASCII 字母作为拼音组合（composition），由查表引擎产出 UTF-8 汉字，再由行编辑器插入缓冲区。这正是 `user/lib/libime/` 的定位（`ime.h:17-25` 的文件头注释即此说明）。

### 6.2 码表结构

`user/lib/libime/ime_tab.h`：

```c
typedef struct { const char *py; const char *chars; } ime_entry_t;
#define IME_TAB_COUNT 400
static const ime_entry_t ime_tab[IME_TAB_COUNT] = {
  { "a",  "啊嗄锕阿" },
  { "ai", "哀哎唉嗌嗳埃嫒挨捱暧爱瑷癌皑矮砹碍艾蔼锿" },
  /* ... 共 400 条，按拼音升序 ... */
};
```

实测统计（本文写作时统计）：

| 指标 | 值 |
| --- | --- |
| 拼音条目 | 400 |
| 候选字符总数 | 5094（全部互不相同） |
| 单拼音最多候选 | **20**（`ai`、`ao`、`ba`、`ban`、`bao` 等） |
| 是否按拼音升序 | 是（严格递增，无重复键） |
| 候选是否全部可渲染 | 是 —— 5094 个码点全部存在于 `font_cjk.h` 的 6844 条表项中 |
| 生成方式 | 头注释：Auto-generated (gen_ime.py + pypinyin)，按拼音排序 |

`ime.h` 的两个上限：

```c
#define IME_MAX_PINYIN 16   /* 组合串最长 16 字节 */
#define IME_MAX_CAND   20   /* 一次最多返回 20 个候选 */
```

注意 `chars` 是**多个汉字直接拼接**的一个字符串，候选之间**没有 NUL 分隔**；`ImeLookup` 返回的是指向该串内部的指针。因此调用方**不能直接对候选指针用格式化字符串输出或 `strcpy`**（见 §8.2）。

### 6.3 查表算法

```c
int ImeLookup(const char *pinyin, const char **chars_out);  /* ime.c:42-69 */
int ImePrefix(const char *pinyin);                          /* ime.c:71-93 */
```

| 函数 | 算法 | 复杂度 |
| --- | --- | --- |
| `ImeLookup` | 对拼音做 `strcmp` 二分查找；命中后按 `Utf8SeqLen` 逐字符切分候选串写入 `chars_out`，个数上限为 `IME_MAX_CAND`；未命中返回 0 | `O(log 400)` 加 `O(候选数)` |
| `ImePrefix` | 二分查找第一个大于等于给定拼音的条目（下界），再用 `strncmp(entry.py, pinyin, strlen(pinyin)) == 0` 判定是否为前缀 | `O(log 400)` |

源码中写明的两条注意事项（`ime.c:31-34`）：表必须保持按拼音有序，否则二分查找结果错误；`ImePrefix` 只检查“第一个大于等于前缀的条目”，不检查整个前缀区间（在表有序且拼音为 ASCII 的前提下这是充分的）。

### 6.4 Shell 集成状态机

IME 状态是 shell 的静态变量（`shell.c:149-154`）：`s_ime_on`、`s_ime_py[16]`、`s_ime_plen`、`s_ime_cidx`、`s_ime_cands[20]`、`s_ime_ncand`。

```text
         Ctrl+Space（字节 0x80） 或 ime on|off 命令
                   │
                   ▼
   ┌──────────────────────────── s_ime_on ────────────────────────────┐
   │ off：所有可打印键按普通插入                                       │
   │ on ：小写字母 a-z                                                 │
   │       追加到 s_ime_py；若 ImePrefix() 成立则 plen++ 并刷新候选     │
   │       否则清空组合，字母退化为普通文本（tee、txt 这类英文词）      │
   │      空格                                                        │
   │       有候选则提交第 0 个；无候选则空格按字面插入                  │
   │      数字 1-9                                                    │
   │       有第 N 个候选则提交；否则清空组合并按字面插入数字            │
   │      Enter                                                       │
   │       有组合且有候选：先提交 s_ime_cidx（当前恒为 0），继续编辑    │
   │      退格                                                        │
   │       先 plen--（弹出拼音字母），再执行整码点退格                  │
   │      其它可打印键、光标移动、编辑键                                │
   │       ImeDiscardOnEdit()：丢弃组合，按键按原义处理                 │
   └──────────────────────────────────────────────────────────────────┘
```

拦截判据非常具体（`shell.c:1031-1035`）：

```c
/* 组合期间只有这些键不清空组合 */
if (s_ime_plen > 0 && s_ime_on && !mask &&
    !(ch >= 'a' && ch <= 'z') && ch != ' ' &&
    !(ch >= '1' && ch <= '9') && ch != '\b' && ch != 0x7F &&
    ch != 0x80 && ch != '\r' && ch != '\n')
    ImeDiscardOnEdit();
```

`mask` 为真（密码输入）时 IME 完全旁路（`shell.c:1286`），避免密码里混入中文。

“字母既是组合又是文本”这一设计意味着：**组合字母本身就在行缓冲里**（注释 `shell.c:656-662`：The composition letters live IN the line buffer），提交时才被替换掉——这正是下一节不变式的由来。

### 6.5 提交路径 ImeCommit

`shell.c:722-754`：

```c
int plen = s_ime_plen;            /* 拼音字节数 */
const char *txt = s_ime_cands[k];
int tlen = Utf8SeqLen(txt);       /* 单个汉字为 3 字节 */
if (l - plen + tlen >= maxlen) return pos;                      /* 缓冲不足：放弃 */
memmove(buf + (pos - plen) + tlen, buf + pos, (l - pos) + 1);   /* 尾段右移 */
memcpy(buf + (pos - plen), txt, tlen);                          /* 覆盖拼音字母 */
int npos = pos - plen + tlen;
printf("ime: commit '%s' U+%04X\n", tmp, (unsigned)cp);          /* 串口调试锚点 */
```

三点事实：

1. 提交是**直接写 UTF-8 字节**，不经过任何宽字符中间表示，因此后续编辑自动落到码点感知路径上。
2. **调试输出走 `printf` 到 `SYS_DEBUG_LOG` 再到串口**，因为 shell 的屏幕输出不镜像到串口（源码注释原文：the shell's screen is not mirrored to serial, so this is how tests verify …）。这是当前唯一可脚本化的 IME 验证锚点（§9.2）。
3. 候选指针不是以 NUL 结尾的单个字符，因此打印前做了一次**有界拷贝**到 `tmp[5]`。

### 6.6 与行编辑器的不变式

```text
不变式：组合字母恒位于 buf[pos - s_ime_plen, pos)
   ├─ 因此退格只需 plen-- 并做一次整码点退格
   ├─ 因此 ImeCommit 可以按字节区间替换
   └─ 任何会破坏该区间的操作（光标移动、补全、Ctrl-U/K/W 等）
       都必须先 ImeDiscardOnEdit() 丢弃组合
```

`ImeDiscardOnEdit`（`shell.c:799-804`）丢弃组合并刷新状态栏；丢弃后**已输入的拼音字母保留为普通文本**（`shell.c:1326-1332` 的注释明确说明这一点）。

### 6.7 ime 命令与状态栏

| 项 | 事实 | 出处 |
| --- | --- | --- |
| 注册 | `ShellRegisterCommand("ime", "Pinyin IME: ime [on|off] (Ctrl+Space)", CmdIme)` | `shell.c:3781` |
| 参数 | 无参数查询；`ime on`；`ime off`（会清空组合） | `shell.c:757-773` |
| 状态栏 | `TERM_OP_STATUS`，前缀固定为 IME；off 时显示 off (Ctrl+Space)；on 且无组合时显示 on (Ctrl+Space) 加 pinyin；有组合时显示拼音与编号候选 | `shell.c:681-718` |
| 消息缓冲 | `char msg[128]`，循环以 `cn < sizeof(msg) - 8` 为界 | `shell.c:684-699` |
| 切换键 | `KBD_CTRL_SPACE 0x80`，等于 `KBD_CTRL_BASE + 0` | `shell.c:135-136`、`keyboard.c:440-441` |

### 6.8 候选可渲染性的保证

`ime_tab.h` 的候选集是从 `font_cjk.h` 的字形集过滤生成的（`ime_tab.h:19-21`）。本文实测确认该约束成立：**5094 个候选码点，0 个缺失**。因此 IME 提交的字符在 term 与 gui 上一定能画出 16x16 字形，不会退化为问号。

---

## 七、VFS 存储与文件名

### 7.1 名字字段一览

| 字段 | 字节 | 可用 | 用途 | 出处 |
| --- | --- | --- | --- | --- |
| `vfs_item_info_t.name` | 256 | 255 | 条目名（**不是路径**） | `user/services/vfs/vfs.h:80` |
| `vfs_enum_item_t.name` | 256 | 255 | 目录枚举项名 | `vfs.h:302` |
| `vfs_enum_batch_t.batch[64][256]` | 16 KiB | — | 客户端枚举批缓冲（每批最多 64 项） | `vfs.h:121-125` |
| `vfs_req_move_t.new_name` | 256 | 255 | 改名（空串表示不变） | `vfs.h:390` |
| `mount_name` 与 `driver_name` | 64 | 63 | 卷名与驱动名（ASCII 标识符） | `vfs.h:425-426`、`443-444`、`460-461`、`482` |
| `VFS_PATH_MAX` | 1024 | — | URL 字符串字段（唯一“看得到路径”的边界） | `vfs.h:182`、`188` |
| `DRV_PATH_MAX` | 256 | — | 驱动协议内的名字字段 | `vfs.h:483`、`522` |
| `VFS_SEG_MAX` | 256 | 255 | vfs_server 解析出的单段路径 | `vfs_server.c:85` |
| `VFS_ENUM_BATCH` | 8 | — | 单次 ENUM_NEXT 响应的项数（驱动协议侧） | `vfs.h:137`、`310` |

两层名字限制并存：**存储层 255 字节**、**挂载与驱动标识 63 字节**。

### 7.2 “64 字节限制”的来历与含义

`ed9f082` 之前，磁盘卷（virtio-blk）的 inode 名字字段是 64 字节（63 可用），即**最多 21 个汉字**，而 inode 结构本身只有 128 字节。`ed9f082` 把它提升到 255 字节（`VBDK_NAME_MAX 256`），inode 变为 512 字节。

| 层 | 现在 | 含义 | 出处 |
| --- | --- | --- | --- |
| mem 卷（RAM） | `char name[256]`；`strlen(name) >= 256` 返回 `ERR_OVERFLOW` | 最多 85 个汉字 | `fs_mem_driver.c:92`、`fs_mem_driver.c:215-241` |
| 磁盘卷（virtio-blk） | `#define VBDK_NAME_MAX 256`（255 可用加 NUL），inode 512 字节 | 代码注释直说：matching the mem volume so a CJK name (up to 85 chars) behaves the same on Disk | `fs_virtio_blk_driver.c:101-108`、`137-152` |
| 旧盘兼容 | 载入时校验几何字段（block_size、inode_table_start 等），不匹配即视为未格式化并**重新初始化** | 注释：the v2 name-limit bump from 64 to 255 bytes；disk.img 是可抛弃的测试产物，不做迁移 | `fs_virtio_blk_driver.c:755-768` |

因此“VFS 名称 64 字节限制”在今天**只对 ASCII 标识符类字段仍然成立**：`mount_name[64]`（例如 System）、`driver_name[64]`（例如 mem），以及 fs 客户端与服务之间的挂载名校验字段。文件与目录名已放宽到 255 字节。

另有一处相关限制在 GUI：`GUI_MAX_TITLE 64`（窗口标题，`user/services/gui/gui.h:41`），且标题还要按像素宽度二次截断（§4.9）。内核侧还有 `PROC_NAME_MAX 64`（`kernel/syscall/process_desc.c:64`）与 `BLOB_NAME_MAX 32`（`kernel/include/kernel/blob.h:30`），二者都是 ASCII 名字。

### 7.3 UTF-8 安全截断的统一算法

同一个约 8 行的算法在 6 处独立实现（各自的缓冲类型不同），语义完全一致：

```c
/* 有界拷贝：截断后若落在字符中间，则回退到上一个字符边界 */
int len = (int)strlen(src);
if (len >= cap) len = cap - 1;
int back = 0;
while (back < 3 && len - 1 - back >= 0 &&
       ((unsigned char)src[len - 1 - back] & 0xC0) == 0x80)
    back++;                                   /* 数出末尾的续字节个数 */
if (back > 0) {
    int need = Utf8SeqLen(src + (len - 1 - back));
    if (need == 0 || need > back + 1) len -= back;   /* 尾巴不完整：丢掉 */
}
memcpy(dst, src, (size_t)len);
dst[len] = '\0';
```

| 位置 | 函数 | 保护对象 |
| --- | --- | --- |
| `user/lib/libfs/fs.c`（`ed9f082` 新增 `fs_strncpy_utf8`） | `FsMoveItem` | `req->src`、`req->dst_dir`、`req->new_name` |
| `user/services/vfs/vfs_server.c` | `VfsParseUrl` | 单段路径切在 `VFS_SEG_MAX` 边界 |
| `user/services/term/term.c:1265-1279` | `TermRenderLineAt` | RENDER_LINE 的 256 字节负载 |
| `user/services/shell/shell.c:2031-2047` | `FmStrncpyUtf8` | fm 与 mv 选择器以及路径归一化的 256 字节项 |
| `user/services/shell/shell.c:1673-1680` | `PathNormalize` | URL 拼接时的段边界 |
| `kernel/syscall/syscall.c`（`ed9f082`） | `sys_debug_log` | **512 字节**串口输出截断处 |

内核那处值得单独点出：调试日志原先会把一个汉字截成两半，串口上出现半截字节的乱码；现在先回退到字符边界再发送（`ed9f082` 中 `sys_debug_log` 的新增代码块）。

**未采用该算法的位置**（见 §8.1）：`TERM_OP_STATUS` 与 `TERM_OP_BOX` 的 `memcpy` 截断、`PermUiLine`、以及 shell 历史回放的 `strncpy`。

### 7.4 ls 与 cat 的多字节行为

```text
ls <url>                                   shell.c:3081-3139
  FsEnumBegin -> FsEnumNext（每批最多 64 项） -> 逐项 ShellWrite(name) 加换行
  名称按 UTF-8 原样透传；term 按字形宽度渲染，列对齐由 cell 网格保证

cat <url>                                  shell.c:3183-3233
  FsGetItem -> 打印 "== <url> (<size> bytes) =="
  FsOpenItem -> 循环 FsRead（每次 1 KiB） -> CatEmitText(buf, got, pend, &pend_n)
  跨块状态：pend[4] 与 pend_n 保存被 1 KiB 边界切开的半个字符
  非法序列：输出等量点号；文件读完仍有挂起字节时补点号收尾
```

`CatEmitText`（`shell.c:3142-3181`）是 `cat` 支持 CJK 的核心：它用 `Utf8SeqLen` 判断“还差几个字节”，把不足的字节存进 `pend`，下次读入时续上。若不这样做，1 KiB 边界上的汉字会被打印成两个点号。

`tee` 侧是纯字节写（`shell.c:3262-3286`）：`FsWrite(h, 0, argv[2], strlen(argv[2]))`，即从命令行输入的中文按 UTF-8 字节落盘。`ed9f082` 的实测结论正是“tee/cat 3 字节 UTF-8 往返”。

---

## 八、已知限制

### 8.1 限制清单

| # | 限制 | 影响 | 证据 |
| --- | --- | --- | --- |
| 1 | `setlocale` 与 `localeconv` 无实现 | 无法切换 locale；调用即链接失败；宽字符行为不受 locale 影响 | §2.8（全树检索加 Makefile 无 `locale.c`） |
| 2 | 宽格式化 I/O、宽字符 I/O、`wcstod`、`wcsxfrm`、`wcscoll`、`Wcsftime` 未实现 | 只能声明不能用 | `wchar.h:113-150` |
| 3 | `isw*` 与 `tow*` 只支持 ASCII | 中文的字母、数字、空白判定全部为假 | `wctype.c:32-78` |
| 4 | 字体仅 BMP 的 6844 个字形 | 假名、韩文、Ext A、Ext B 无字形；`cjk_glyph_t.u` 是 `uint16_t`，结构上排除 U+10000 以上 | §3.3 |
| 5 | **判宽不一致**：`Utf8CharWidth` 按码位区间给 2 列，term 与 gui 按“字形是否存在”给 2 格或 16px | 输入 3400 至 4DBF（Ext A）等“宽区间但无字形”的字符时，shell 按 2 列放光标、term 只画 1 格问号，光标右偏 1 列 | `utf8.c:108-126` 对比 `term.c:513` 与 `term.c:671-676` 以及 `gui.c:281` |
| 6 | IME 组合串最长 16 字节、候选最多 20 个 | 当前表最大候选恰为 20，未发生截断；扩表后会**静默丢弃**超出部分 | `ime.h` 的 `IME_MAX_PINYIN` 与 `IME_MAX_CAND`；实测最大值 20 |
| 7 | `s_ime_cidx` 没有任何按键可以修改（只有清零与钳制） | Enter 提交的“当前候选”恒为第 1 个；数字 1-9 只能直接选前 9 个候选，没有翻页 | `shell.c:152`、`668`、`675-676`、`1043` |
| 8 | 状态栏候选以格式化字符串打印“指向候选串内部的指针” | 会打印该候选**及其后所有候选**（拼音 a 显示成 1啊嗄锕阿 2嗄锕阿 …），既冗余又提前撞上 128 字节上限 | `shell.c:691-699` |
| 9 | `TERM_OP_STATUS`（prefix 63、msg 127 字节）与 `TERM_OP_BOX`（title 63 字节）是**纯字节截断**，`PermUiLine` 也是原样 `memcpy` | 超长中文状态、标题或权限面板文本可能被切成半个字形 | `term.c:1370-1380`、`term.c:1398-1411`、`term.c:1658-1668`，对比 `term.c:1265-1279`（RENDER_LINE 已做边界回退） |
| 10 | 历史回放用 `strncpy(buf, hist, maxlen-1)` | 255 字节截断不保证落在字符边界 | `shell.c:1131`、`1155`、`1177`、`1187` |
| 11 | Tab 补全的最长公共前缀按字节比较 | 理论上可能把半截多字节字符写回行缓冲 | `shell.c:833-838`、`930-936`、`950-951` |
| 12 | `TuiInputLine` 是字节级且无 IME | login 与 stop 等 TUI 输入行不能输入中文（显示可以） | `user/lib/libtui/tui.c` 的 `TuiInputLine` |
| 13 | VGA 文本模式无法渲染 CJK | `TermDrawCell` 在 `s_vga_text` 分支把大于 0x7E 的码点一律写成空格 | `term.c:425-431` |
| 14 | 无 Unicode 归一化、无 bidi、无 CJK 断行与避头尾 | 组合字符与 RTL 文本不保证正确；换行按 cell 边界硬截 | `utf8.c:28-31`，换行逻辑 `term.c:710-718` |
| 15 | 无 AltGr、死键或 Unicode 直接输入 | 除拼音外不能输入任何非 ASCII 字符（含拉丁重音、日文假名） | `keyboard.c:425-446` |
| 16 | shell 横幅刻意保持 ASCII | 启动画面不用制表符，与 TUI 面板的 ASCII 边框一致 | `shell.c:1478-1484`（注释：UTF-8 box chars break VGA vc terminals）、`term.c:1210-1226` |

### 8.2 关于限制 8 的补充

`ImeLookup` 的返回契约（`ime.h:43-46`）写明“each points at one UTF-8 hanzi (3 bytes + NUL)”，但实现返回的是**指向共享字符串内部的指针**，其后并非 NUL，而是下一个候选（`ime.c:55-64`）。`ImeCommit` 正确地只拷贝 `Utf8SeqLen` 个字节，而 `ImeShowStatus` 的格式化输出暴露了这个陷阱。修法很直接（按 `Utf8SeqLen` 有界拷贝到临时缓冲，或状态栏只显示候选数量），但属于源码修改，不在本文档范围内。

---

## 九、测试与验证方法

### 9.1 双通道观察模型

`scripts/smoke_test.py` 的文档头（第 17-25 行）给出了唯一的权威观察模型：

| 通道 | 承载内容 | 采集方式 |
| --- | --- | --- |
| **串口** | 仅 `debug_log` 输出（回归锚点、manager 启动、serial-test、pkg 与 hello 的输出） | `build/serial.log` |
| **VGA** | shell 提示符、回显、命令输出、Powerbox 面板 | monitor 的 `screendump` 存 PPM，再用 `tools/vga_decode.py` 解码成 113x38 文本网格 |

关键事实：**shell 的屏幕输出不会出现在串口**（`TERM_DEBUG_SERIAL_MIRROR` 默认编译掉，`term.c:1038-1057`），所以中文渲染只能从 VGA 通道看，而 IME 是否提交了正确码点只能从串口看（`ImeCommit` 的 `printf`）。

### 9.2 IME 验证（串口锚点）

`ImeCommit` 的调试行格式固定（`shell.c:749`）：

```text
ime: commit '<汉字>' U+<四位十六进制>
```

最小验证序列（由代码推导，与 `ed9f082` 提交说明中的实测一致——QEMU 实测提交 U+4E2D）：

1. 输入 `ime on`，或按 Ctrl+Space。
2. 输入拼音 `zhong`，按空格提交首选。
3. 在串口日志中检索 `ime: commit`，期望得到 `U+4E2D`。

脚本化注入按键时注意：`sendkey` 的名字表与修饰键支持见 `scripts/smoke_test.py:60-71`（`KEYMAP` 与 `MODIFIERS`，支持 ctrl、shift、alt），Ctrl+Space 的组合写法为 `sendkey ctrl-spc`。

### 9.3 CJK 渲染验证（VGA 通道）

`tools/vga_decode.py` 的适用边界必须说清楚：

| 项 | 事实 | 出处 |
| --- | --- | --- |
| 输入 | P6 PPM（`screendump` 产物） | `vga_decode.py` 用法说明 |
| 网格 | cell 9x20 px，字形 8x16，1024x768 得 113x38 | 同上 |
| 字形来源 | 解析 `user/services/term/font.h` 的 `s_font[95][16]`，按**块顺序**而非注释字符（因为单引号字形写成转义形式，会骗过正则） | `parse_font()` |
| 匹配 | 逐 cell 取 8x16 位图，与 95 个 ASCII 字形做**汉明距离**最近匹配（同时尝试正常与反色两种解释），允许至多 8 位误差 | `match_glyph()` 的 `MAX_DIST = 8` |
| **不支持 CJK** | 解码器只认识 8 位宽的 ASCII 字形；CJK 是 16x16 跨两格，解码器会把 lead 格左半边误判成某个“最接近的 ASCII 字符”，把续接格当成独立字符 | 由网格与字形表推导；脚本内无 CJK 分支 |

因此验证 CJK 渲染**不能只靠** `vga_decode.py` 的文本输出，可行为：

1. 用 `vga_decode.py` 验证同屏 ASCII 部分（提示符、命令回显）位置正确；
2. 对 CJK 部分直接查看 PPM（或转成 PNG）做像素级确认——这正是 `9f1171b` 提交说明中“font_cjk.h 经 QEMU 像素验证（中文与 ANSI 红色正确显示）”的做法；
3. 若要脚本化，可扩展解码器：CJK 字形同样在 `font_cjk.h` 中（每条 32 字节），按 18px 跨格提取 16x16 位图后与 6844 条表项做最近匹配，语义与现有 `match_glyph` 完全同构。

### 9.4 通过 tee 与 cat 验证存储链往返

```text
tee /Volumes/Users/中文.txt 中文内容     ← 文件名与内容都含 CJK
ls /Volumes/Users                         ← 名字原样列出（term 双宽渲染）
cat /Volumes/Users/中文.txt               ← 内容 UTF-8 透传，跨 1 KiB 块
```

`ed9f082` 的实测结论是 tee 与 cat 的 3 字节 UTF-8 往返。本文核对的两个必要条件都在代码里：`CatEmitText` 的 `pend[4]` 跨块状态（`shell.c:3142-3181`），以及 mem 与 virtio 两侧一致的 255 字节名字上限（§7.2）。

### 9.5 建议补充的自动化用例

| 用例 | 断言 | 通道 |
| --- | --- | --- |
| IME 提交 | 串口出现 `ime: commit` 且码点为 U+4E2D | 串口 |
| IME 前缀退化 | 输入 `txt` 后串口**无** `ime: commit`，行内容仍为 txt | 串口加 VGA |
| 行编辑 | 提交汉字后按左箭头再按退格，屏幕不出现孤立续接格 | VGA |
| 光标列 | 提示符含中文时，光标列等于 `Utf8StrWidth(prompt) + Utf8StrWidth(line, pos)` | VGA（比对像素列） |
| 名字上限 | 84 个汉字的文件名可建可列；长度达到 256 时报 `ERR_OVERFLOW` | 串口（命令返回码） |
| 面板覆盖 | Powerbox 面板关闭后，其覆盖过的中文单元格恢复为原字形（`CELL_WIDE_CONT` 往返） | VGA |

---

## 十、参考索引

| 类别 | 文件 |
| --- | --- |
| 编码 | `user/lib/libc/utf8.c`、`utf8.h`、`wchar.c`、`wchar.h`、`wctype.c`、`wctype.h`、`uchar.h`、`locale.h` |
| 字体 | `user/lib/font_cjk.h`、`user/services/term/font.h`、`user/lib/libgui/font.h` |
| 渲染 | `user/services/term/term.c`、`user/lib/libtui/tui.c`、`user/lib/libtui/tui.h`、`user/lib/libgui/gui.c`、`user/lib/libgui/gui.h`、`user/services/gui/main.c`、`user/services/gui/gui.h` |
| 编辑 | `user/services/shell/shell.c` |
| 输入法 | `user/lib/libime/ime.c`、`ime.h`、`ime_tab.h` |
| 存储 | `user/lib/libfs/fs.c`、`fs.h`、`user/services/vfs/vfs.h`、`vfs_server.c`、`fs_mem_driver.c`、`fs_virtio_blk_driver.c` |
| 内核 | `kernel/syscall/syscall.c`（`sys_debug_log` 的 UTF-8 边界截断） |
| 测试 | `scripts/smoke_test.py`、`tools/vga_decode.py` |
| 提交 | `9f1171b`（TUI ANSI 与 CJK）、`3ee600c`（GUI CJK）、`ed9f082`（全链路 UTF-8、IME 与存储） |
| 相关文档 | [TUI 设计文档](tui_design.md)、[VFS 设计文档](vfs_design.md)、[测试报告](test_report.md) |

> 返回 [文档索引](README.md)
