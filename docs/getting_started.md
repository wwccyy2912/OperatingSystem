# OpSys 快速上手指南

> 适用版本：OpSys v0.8-dev（git HEAD 105805d）　|　最后更新：2026-09-19
>
> 从零依赖到看见 shell 提示符 `opsys:/$` 的完整路径：环境依赖、构建流水线、QEMU 运行参数、首次启动现象、GDB/串口/VGA 三路调试与故障排查速查表。

本指南只讲"怎么把它跑起来、怎么确认它真的跑对了"。系统为什么这样设计，见 [architecture.md](architecture.md)；每条 shell 命令的语义，见 [shell_reference.md](shell_reference.md)。

---

## 一、五分钟跑起来

```bash
# 1) 依赖（Fedora 写法，其它发行版见 §2.3）
sudo dnf install gcc nasm binutils make grub2-tools xorriso mtools \
                 qemu-system-x86 qemu-img gdb python3

# 2) 构建内核 + 27 个用户服务 ELF + 可引导 ISO
cd OperatingSystem
make -j"$(nproc)" iso

# 3) 造一块持久化磁盘（Makefile 的 run 目标会直接打开 disk.img，缺了 QEMU 直接报错）
qemu-img create -f raw disk.img 8M
#    没有 qemu-img 时等价写法：dd if=/dev/zero of=disk.img bs=1M count=8

# 4) 启动
make run
```

按顺序你会看到：

| 阶段 | 在哪里看到 | 关键文本 |
| --- | --- | --- |
| GRUB 菜单 | 屏幕 + 串口 | `OpSys` / `Diagnostics: videoinfo`，10 秒倒计时 |
| 内核初始化 | 串口（运行 `make run` 的那个终端） | `OpSys kernel starting...` |
| init 自检 | 串口 | `init: Starting (PID 1)` 到 `=== Results: N/N passed ===` |
| 服务拉起 | 串口 | `manager: serial service ready (port 7)` … `manager: starting shell` |
| 桌面 | 屏幕（帧缓冲） | 居中 splash `OpSys Microkernel`，随后 `Welcome to OpSys` 与 `opsys:/$` |
| 登录（可选） | 屏幕 | `login admin` + 掩码密码，默认账户 `admin/admin` |

> 提示：shell 与终端服务的输出**不走串口**，只画在帧缓冲上（`scripts/smoke_test.py:17-23` 明确记载双通道观测模型：串口只有服务 `debug_log`，shell 输出永远在 VGA 通道）。所以"串口没有 shell 输出"不等于"系统没起来"，见 §7.3。

### 1.1 三条观测通道

```text
 kernel(Ring 0) / services(Ring 3) --+-- SerialPuts, debug_log --> COM1 --> -serial stdio|file
                                     |                                         （终端或文件）
                                     +-- TERM_OP_*（把字符画进帧缓冲）--> 屏幕窗口
                                                                         或 monitor screendump
                                                                          --> tools/vga_decode.py
```

| 通道 | 承载什么 | 怎么读 | 证据 |
| --- | --- | --- | --- |
| 串口 COM1 | 内核启动日志、服务 `printf`/`debug_log`、init 自检结果 | `make run` 的 `-serial stdio`；或 `-serial file:build/serial.log` | `kernel/kernel_main.c:94`、`user/services/serial/serial.c:111-122`（`SERIAL_COM1_BASE 0x3F8`、IRQ4） |
| 帧缓冲 | term 的文本渲染、shell 回显、Powerbox 面板、GUI 合成器 | 直接看 QEMU 窗口；无图形环境用 monitor `screendump` + `tools/vga_decode.py` | `user/services/term/term.c:2102-2104`（9x20 像素单元） |
| QEMU monitor | 注入键盘（`sendkey`）、抓屏（`screendump`）、复位（`system_reset`）、退出（`quit`） | `-monitor unix:/tmp/opsys-mon.sock` 或窗口里 Ctrl-Alt-2 | `scripts/smoke_test.py:50-51,650-666` |

---

## 二、环境依赖

### 2.1 工具清单（以 Makefile / scripts 为准）

| 工具 | Makefile / 脚本中的位置 | 用途 |
| --- | --- | --- |
| `nasm` | `Makefile:13` `AS := nasm`；`ASFLAGS := -f elf64`（`Makefile:40`） | 汇编 `kernel/arch/x86_64/*.asm`、`context_switch.S`、`syscall_entry.S`、`user/runtime/crt0.S`、`user/lib/libc/setjmp.S`（`Makefile:60-63,161-163`） |
| `gcc` | `Makefile:14` `CC := gcc` | 编译内核 C（`Makefile:32-38`）与用户态 C（`Makefile:45-54`） |
| `ld.lld` 或 `ld` | `Makefile:18`：优先取 `command -v ld.lld`，取不到则回退到 `command -v ld` | 链接 `kernel.elf` 与每个服务 ELF |
| `ld`（binutils，**硬编码**） | `Makefile:298` `cd build && ld -r -b binary -o ...` | 把服务 ELF 包成 `build/<svc>_blob.o`。这一步不走 `$(LD)`：即使你装了 lld，也必须装 binutils 的 `ld` |
| `objcopy` | `Makefile:15` `OBJCOPY := objcopy`；`Makefile:299-302` | `--redefine-sym` 把 `_binary_user_services_<svc>_elf_start` 改名成 `<svc>_elf_start` |
| `grub2-mkrescue` | `Makefile:322`（名字硬编码） | 生成可引导 ISO |
| `xorriso` | 不是 Makefile 变量，是 `grub2-mkrescue` 的内部依赖（`grub2-mkrescue --help` 列出 `--xorriso=文件`） | 真正写 ISO9660/El Torito 镜像；缺了 `make iso` 会失败 |
| `qemu-system-x86_64` | `Makefile:327,338`；`scripts/run.sh:20` | 运行与调试 |
| `qemu-img` | 不在仓库脚本里（`README.md:355` 提到） | 造 `disk.img`；等价于 `dd` |
| `gdb` | `Makefile:337`；`scripts/run.sh:107-121` | 连 QEMU 的 GDB stub |
| `python3` | `scripts/*.py`、`tools/vga_decode.py` | 自动化验收与 VGA 解码 |
| `clang-format` | `Makefile:355-371` | 只服务于 `make format` / `make format-check`，**不参与** `iso` |
| `make` | 顶层构建入口 | 全仓库统一构建（`docs/requirements.md:81-84`） |

`docs/requirements.md` §三 只给出技术选型（NASM 汇编 + C11、QEMU x86_64、Make 统一构建、`grub-mkrescue` 生成镜像），没有钉版本号；下表的下限是**从构建/运行实际用到的特性倒推**出来的实用值，仓库在构建期并不校验版本——遇到报错以实际提示为准。

### 2.2 最低版本与自查判据

| 工具 | 实用下限 | 依据（用到的特性） | 版本过老时的现象 |
| --- | --- | --- | --- |
| GCC | 4.9（建议 9 以上） | `-fstack-protector-strong`、`-mstack-protector-guard=global`（`Makefile:32-34`，注释 `Makefile:26-31` 解释为什么必须 global）、`-std=c11`、`-mcmodel=large` | `cc1: error: unrecognized command-line option '-mstack-protector-guard=global'` |
| NASM | 2.x（实测 2.16.03） | `-f elf64`、`bits 64`、`default rel`、`section .multiboot2`（`kernel/arch/x86_64/boot.asm:63`、`kernel/arch/x86_64/context_switch.S:22-23`） | `nasm: error: unrecognised option '-f elf64'` |
| GNU ld | 2.31（实测 2.46.1） | `-z max-page-size=0x1000`、`-z separate-code`（`Makefile:42,56-57`）、`ld -r -b binary`（`Makefile:298`） | `ld: unrecognized option '-z'` 或 `unrecognized option 'separate-code'` |
| LLVM lld（可选，会被 Makefile 优先选中） | 需支持 `-z separate-code` 与 `-T` 脚本（`scripts/user.ld`） | `Makefile:18` 优先用 `ld.lld`；`Makefile:56-57` | 同上，`unknown argument` |
| GRUB 2 | 需支持 `--modules`/`--locales`、`multiboot2` 模块、`videoinfo` 命令（实测 2.12） | `Makefile:322`、`boot/grub.cfg:4-6,20-29` | `grub2-mkrescue: unrecognized option '--locales'` |
| QEMU（x86_64） | 需支持 `virtio-blk-pci` 的 `disable-modern` 属性、`screendump`/`sendkey`/`system_reset`、`-d int`（实测 10.2.2） | `Makefile:331-333`；`scripts/smoke_test.py:650-666` | 自查：`qemu-system-x86_64 -device virtio-blk-pci,help` 应能列出 `disable-modern=<bool>` |
| Python 3 | 3.x 任意（脚本只用标准库 `os/re/sys/socket/subprocess/struct/time`，未使用 f-string 或海象运算符；实测 3.14.7） | `scripts/*.py`、`tools/vga_decode.py:28` | `SyntaxError`（基本不会遇到） |
| clang-format | 12 以上（实测环境未安装） | `.clang-format` 使用 `SpaceBeforeCaseColon`、`AlignConsecutiveBitFields`、`AllowShortEnumsOnASingleLine`、`AlignConsecutiveMacros` 等较新键（`.clang-format:36-46,60-62`） | `Unknown value for ...` 或 `invalid value`；自查：`make format-check` 能跑通 |

### 2.3 各发行版安装命令

包名随发行版与版本而异，下面的写法覆盖常见目标（**装完请用 §2.4 自检**）：

```bash
# Fedora / RHEL / Rocky
sudo dnf install gcc nasm binutils make grub2-tools xorriso mtools \
                 qemu-system-x86 qemu-img gdb python3 clang-tools-extra
# Debian / Ubuntu
sudo apt update
sudo apt install build-essential nasm binutils make grub-pc-bin grub-common \
                 xorriso mtools qemu-system-x86 qemu-utils gdb python3 clang-format
# Arch / Manjaro
sudo pacman -S --needed base-devel nasm binutils make grub xorriso mtools \
                       qemu-full gdb python clang
# openSUSE
sudo zypper install gcc nasm binutils make grub2 xorriso mtools \
                    qemu-x86 qemu-tools gdb python3 clang-tools
```

#### 发行版差异：`grub2-mkrescue` 还是 `grub-mkrescue`

Makefile 里写死了 Fedora 风格的名字（`Makefile:322`）：

```make
grub2-mkrescue --modules="all_video gfxterm gfxterm_background font" \
    --locales="" -o build/opsos.iso build/isodir 2>/dev/null
```

Debian/Ubuntu 上的可执行文件叫 `grub-mkrescue`。**这条命令的 stderr 被 `2>/dev/null` 丢掉了**，所以缺命令时你只会看到 make 抛出的一个 `Error 127`，看不到原因。两种不改仓库的解法：

```bash
# 方案 A：在 PATH 里补一个同名命令（不改仓库、不改 Makefile）
sudo ln -s "$(command -v grub-mkrescue)" /usr/local/bin/grub2-mkrescue

# 方案 B：先手工确认 ISO 生成链路本身能不能跑
grub-mkrescue --version && xorriso --version | head -1
```

### 2.4 一条命令自检环境

```bash
for t in gcc nasm ld ld.lld objcopy make grub2-mkrescue grub-mkrescue \
         xorriso qemu-system-x86_64 qemu-img gdb python3 clang-format; do
    printf '%-22s %s\n' "$t" "$(command -v "$t" || echo '(缺失)')"
done
gcc --version | head -1; nasm -v; ld --version | head -1
grub2-mkrescue --version 2>&1 | head -1; qemu-system-x86_64 --version | head -1
```

本指南中的实测数据来自下面这套环境，可作对照：

| 工具 | 实测版本 |
| --- | --- |
| gcc | 16.2.1（Red Hat 16.2.1-2） |
| nasm | 2.16.03 |
| GNU ld | 2.46.1-1.fc44（`ld.lld` 未安装，Makefile 自动回退到 `ld`） |
| grub2-mkrescue | (GRUB) 2.12 |
| qemu-system-x86_64 | 10.2.2 |
| python3 | 3.14.7 |

### 2.5 不需要什么

- **不需要交叉编译器**：`CC := gcc`（`Makefile:14`）就是宿主 `gcc`，输出 `elf64`；请直接在 x86_64 Linux 上构建。
- **不需要目标机 libc / libgcc**：内核与用户态都带 `-ffreestanding -nostdlib -nostdinc`（`Makefile:32-38,45-54`），只用 GCC 自带的 freestanding 头（`FREESTANDING_INC := $(shell $(CC) -print-file-name=include)`，`Makefile:22`）。用户态跑的是仓库自带的 C Runtime（`user/runtime/`）。
- **不需要 VirtualBox**（`docs/requirements.md:86-90` 把它列为真机近似验证的可选项）。
- **不需要网络**：`.ops` 包格式与 pkg 服务都是本地实现，构建与测试全程离线。
- 初始需求中的 Rust 未采用，实际是 C11 + NASM（`docs/requirements.md:76-79`）。

---

## 三、获取源码与目录导航

```bash
git clone <repo-url> OpSys && cd OpSys
git log -1 --oneline        # 本指南对应 105805d
```

| 路径 | 内容 | 与上手相关的点 |
| --- | --- | --- |
| `Makefile` | 唯一构建入口 | 所有目标的定义处，见 §4.1 |
| `kernel/` | Ring 0：`arch/x86_64`、`mm`、`sched`、`ipc`、`cap`、`syscall`、`blob`、`gfx`、`process` | 入口 `KernelMain()`（`kernel/kernel_main.c:89`） |
| `user/services/` | 每个服务一个目录，共 27 个可执行映像 | `init`（PID 1）、`manager`（服务管理器）、`shell`、`term`、`serial`、`keyboard`、`vfs` + 两个 fs 驱动、`perm`、`device_mgr`、`pkg`、`user`、`wm`、`gui`、`policy`、`net` 及若干 demo |
| `user/lib/` | `libc`/`libos`/`libipc`/`libfs`/`libpkg`/`libtui`/`libgui`/`libwm`/`libime` | 全部静态链进每个服务 ELF |
| `user/runtime/` | `crt0.S`、malloc、errno、exit、signal | `crt0.S` 提供用户态真正的入口 `_start` |
| `scripts/` | `build.sh`、`run.sh`、`user.ld`、验收脚本 | 见 §4.6、§7.5 |
| `boot/grub.cfg` | GRUB 菜单与显示模式 | 见 §5.4 |
| `tools/vga_decode.py` | 把 screendump 的 PPM 解回文本 | 见 §7.3 |
| `build/` | 全部中间产物（被 .gitignore 忽略） | 见 §4.4 |

---

## 四、构建

### 4.1 make 目标总表

`.PHONY` 声明为 `all kernel.elf init_user iso run debug clean help`（`Makefile:213`）；`format` / `format-check` 未列入 `.PHONY`（仓库里也没有同名文件，正常可用）。

| 目标 | 定义位置 | 做什么 | 产物 |
| --- | --- | --- | --- |
| `make`（等价 `all`） | `Makefile:215` | 只构建内核（依赖全部对象 + 全部 blob） | `./kernel.elf`（**仓库根目录**，不在 build/ 下） |
| `make help` | `Makefile:217-230` | 打印目标清单与选定的工具链（`CC=` / `AS=` / `LD=` / `OBJCOPY=`） | 无 |
| `make kernel.elf` | `Makefile:234-235` | `$(LD) $(LDFLAGS) -o kernel.elf <全部 .o + 27 个 blob>` | `./kernel.elf` |
| `make init_user` | `Makefile:253` | 只链接 init 服务 | `build/user/services/init.elf` |
| `make iso` | `Makefile:318-323` | 依赖 `kernel.elf init_user`；拷贝内核与 grub.cfg 到 `build/isodir/`，再调 `grub2-mkrescue` | `build/isodir/boot/kernel.elf`、`build/isodir/boot/grub/grub.cfg`、`build/opsos.iso` |
| `make run` | `Makefile:326-333` | 依赖 `iso`，然后启动 QEMU | 见 §5.1 |
| `make debug` | `Makefile:335-345` | 依赖 `iso`，以 `-nographic -serial mon:stdio -s -S` 启动 QEMU（GDB stub 1234，CPU 冻结） | 见 §7.1 |
| `make clean` | `Makefile:348-349` | `rm -rf build`（**不删根目录的 `kernel.elf`**） | 无 |
| `make format` | `Makefile:355-362` | `clang-format -i` 格式化 `kernel/`、`user/` 下的 `.c`/`.h` | 就地改写源文件 |
| `make format-check` | `Makefile:364-371` | `clang-format --dry-run -Werror` 校验 | 无 |

`make format` / `format-check` 显式排除三类文件（`Makefile:357-360,366-369`）：`fs_mem_driver.c`（项目规则冻结）、`term/font.h` 与 `kernel/panic_font.h`（生成的字模数据）。风格由 `.clang-format` 定义：4 空格缩进、K&R 花括号（`BreakBeforeBraces: Attach`，`.clang-format:30`）、行宽 100（`.clang-format:27`）、指针右贴（`.clang-format:55`）。

### 4.2 从 .c 到 ISO：整条流水线

```text
kernel/*.c      -- gcc CFLAGS (-mcmodel=large, -fstack-protector-strong) --+
kernel/*.asm,.S -- nasm -f elf64 -----------------------------------------+
                                                              v
                                                 build/kernel/**.o
                                                                          |
user/**.c       -- gcc USER_CFLAGS (-mcmodel=large, -nostdlib) -----------+
user/**/*.S     -- nasm -f elf64 -----------------------------------------+
                                                       v
                                          build/user/**.c.o / **.S.o
                                                       |
   每个服务：<该服务入口 .o> + USER_SHARED_OBJ（其余全部用户 .o）   |
   ld -T scripts/user.ld --gc-sections -z separate-code            |
                                                       v           |
              build/user/services/<svc>.elf（27 个，基址 0x400000）  |
                       |  ld -r -b binary -> build/<svc>_blob_raw.o |
                       |  objcopy --redefine-sym ..._elf_start=<svc>_elf_start
                       v                                           |
                build/<svc>_blob.o --------------------------------+
                                                                   v
                ld -T kernel/arch/x86_64/linker.ld -> ./kernel.elf
                                                                   |
                iso: cp 到 build/isodir/boot/ + grub2-mkrescue
                                                                   v
                                                          build/opsos.iso
```

### 4.3 每个服务如何变成内核里的 blob

这是本构建系统最值得先理解的一步（`Makefile:168-207,255-305`）：

1. **共享对象池**：`USER_C` 列出所有用户态源文件（`Makefile:98-152`），全部编成 `build/user/**.c.o`。
2. **入口对象清单**：`USER_SVC_ENTRY_OBJ` 列出每个服务自己的入口对象（`Makefile:175-203`，例如 `build/user/services/shell/shell.c.o`）。
3. **共享集合**：`USER_SHARED_OBJ := $(filter-out $(USER_SVC_ENTRY_OBJ), $(USER_OBJ))`（`Makefile:204`）——即"除所有服务的入口对象之外的一切"（runtime、libc、libos、libipc、libtui、libgui、libwm、libfs、libpkg、crt0、setjmp）。这样每个服务都拿到完整的 C 运行环境，又不会撞上别的服务的 `main`。
4. **一服务一 ELF**：`SVC_LINK_RULE` 宏（`Makefile:258-262`）为 27 个服务各生成一条规则：

   ```make
   build/user/services/$(1).elf: build/user/services/$(2) $(USER_SHARED_OBJ)
       $(LD) $(USER_LDFLAGS) -o $@ $^
   ```

   `net` 是特例，它链两个入口对象（`main.c.o` + `proto.c.o`，`Makefile:290-292`）。
   链接脚本 `scripts/user.ld` 把所有用户程序固定在 `0x400000`（`scripts/user.ld:33`），并用显式 `PHDRS` 强制 W^X：`.text` R+X、`.rodata` R、`.data`/`.bss` R+W（`scripts/user.ld:24-29`）。
5. **嵌入内核**：`SVC_BLOB_RULE`（`Makefile:295-305`）对每个服务做三件事——`ld -r -b binary` 把 ELF 当二进制数据包成目标文件、`objcopy --redefine-sym` 把符号名从 `_binary_user_services_<svc>_elf_start` 改成 `<svc>_elf_start`、删掉中间文件。
6. **注册表**：`kernel/blob/blob.c` 里 `BLOB_REG("shell", shell_elf_start, (u64)shell_elf_size)` 逐个登记（`kernel/blob/blob.c:47-74` 声明、之后逐条 `BLOB_REG`），并且**注册失败即 panic**（`kernel/blob/blob.c:81-88` 的 fail-fast 宏），避免"少一个 blob、服务静默起不来"。
7. **谁拉起谁**：`init` 用 `BlobGet("manager", ...)` + `ProcessCreate` 拉起 manager（`user/services/init/main.c:2513-2529`，blob 缓冲固定 262144 字节）；manager 再用同样的方式拉起其余服务（服务表 `user/services/manager/manager.c:132-149`）。shell 的 `exec <blob 名>` 也能按名字取 blob 起进程（`user/services/shell/shell.c:3776`）。

### 4.4 产物清单

以下为本机 `make iso`（干净树、GCC 16.2.1 / GNU ld 2.46.1）实测：

| 路径 | 内容 | 实测大小 |
| --- | --- | --- |
| `kernel.elf` | 内核 ELF（含全部 27 个服务 blob） | 约 1.8 MB |
| `build/kernel/**/*.o`、`build/user/**/*.o` | 全部对象文件与 `.d` 依赖文件 | 内核侧约 375 KB，用户侧约 3.3 MB |
| `build/user/services/<svc>.elf` | 27 个独立用户程序（`init` 69 KB、`shell` 146 KB、`term` 284 KB、`gui` 517 KB 等） | 合计约 1.7 MB |
| `build/<svc>_blob.o` | 27 个 blob 目标文件（`init_blob.o`、`manager_blob.o` 等） | 单个 14 KB 到 518 KB，合计约 1.7 MB |
| `build/isodir/boot/kernel.elf` | ISO 里真正被 GRUB 加载的内核 | 同 `kernel.elf` |
| `build/isodir/boot/grub/grub.cfg` | ISO 里的引导配置 | 同 `boot/grub.cfg` |
| `build/opsos.iso` | 可引导 ISO（GRUB El Torito） | 约 21 MB（主要是 GRUB 模块与字库） |
| `disk.img` | QEMU 的 virtio-blk 后端（见 §5.2） | 建议 8 MiB |

`build/`、`kernel.elf`、`disk.img` 都在 `.gitignore` 中，不会进版本库。

### 4.5 关键编译/链接参数为什么这么写

| 参数 | 位置 | 原因 |
| --- | --- | --- |
| `-ffreestanding -nostdlib -nostdinc` | `Makefile:32-34,45-47` | 没有宿主 libc；只用 GCC 自带头（`Makefile:20-22` 的注释解释 `-isystem $(FREESTANDING_INC)` 的用意） |
| `-mcmodel=large` | `Makefile:34,47` | 内核与用户态都在高地址/独立地址空间，小代码模型寻址不够 |
| `-fstack-protector-strong -mstack-protector-guard=global` | `Makefile:32-33` | 打开 canary 检查；**必须**用 global guard，因为 freestanding 内核没有设置 FS base，默认的 `%fs:0x28` 会读到物理地址 0x28（注释见 `Makefile:24-31`）。运行时在 `kernel/arch/x86_64/stack_chk.c` |
| `-mno-red-zone -mno-sse -mno-sse2 -mno-mmx` | `Makefile:35` | 中断可能随时打断内核；红区会被压栈破坏；内核不保证保存 SIMD 状态 |
| `-ffunction-sections -fdata-sections` 加用户态 `--gc-sections` | `Makefile:32,45,57` | 让每个服务只保留自己用到的库代码（否则每个 ELF 都会拖进整套 libc 与全部客户端库） |
| `-MD -MP` 与末尾的 `-include ... .d` | `Makefile:38,54,373` | 头文件改动自动触发重编 |
| `-z max-page-size=0x1000` | `Makefile:42,56` | 段对齐到 4 KiB，内核按页映射 |
| `-z separate-code` | `Makefile:57` | 代码段与数据段分开映射，配合显式 `PHDRS` 实现 W^X |
| `-T scripts/user.ld`，链接基址 `0x400000` | `scripts/user.ld:33` | 内核按这个虚拟地址映射用户页（注释 `scripts/user.ld:13-14`） |

### 4.6 `scripts/build.sh`

一个薄封装（`scripts/build.sh`）：

- `set -euo pipefail`（`scripts/build.sh:13`）；
- 默认 jobs 取 `$(nproc)`，可用 `-j N` 覆盖（`scripts/build.sh:28,62-64`）；
- 预检只**告警**不阻断（`scripts/build.sh:79-83`：找不到 `nasm`/`gcc` 时打印 WARNING，把失败留给 make 自己报）；
- 把目标透传给 make（`scripts/build.sh:95`），并追加 `VERBOSE=0`（`scripts/build.sh:88-93`）——注意 Makefile 中**没有**使用 `VERBOSE` 变量，这个参数目前无效果。

```bash
./scripts/build.sh iso            # 等价于 make -j$(nproc) iso
./scripts/build.sh -j4 run
```

### 4.7 增量构建与 `clean` 的边界

- 依赖关系由 `-MD -MP` 生成的 `.d` 文件维护，最后一行 `-include` 把它们全部并入（`Makefile:373`），改一个头文件会重编所有引用它的对象。
- `make clean` 删掉的是 `build/`（`Makefile:349`），**根目录的 `kernel.elf` 保留**；想彻底重建内核就手工删掉它。
- `make`（`all`）**不生成 ISO**；改完代码只 `make`、再 `make run` 是可以的（`run` 依赖 `iso`，会自动重建），但如果你手工启动 QEMU 跑旧的 `build/opsos.iso`，就会"改了没生效"。

---

> **v0.9 起 `make run` / `make debug` 默认附带 PCnet 网卡** 
> （`-netdev user,id=n0 -device pcnet,netdev=n0`），因此 `ip` / `netstat` / `ping` / 
> `http` 这些命令开箱可用；QEMU 的 slirp 把 `10.0.2.2` 映射到宿主机回环、`10.0.2.3` 
> 当作 DNS 服务器，所以既可以在客户机里 `http 10.0.2.2 <port> /` 访问宿主机上的服务， 
> 也可以用 `scripts/verify_tools.py` 自动跑这套往返验证。若你手工拼 QEMU 命令行， 
> 记得自己加上这两个参数，否则 `net` 服务会打印 NIC start FAILED 后退出（不影响其它功能）。 

## 五、运行

### 5.1 `make run` 的 QEMU 命令行逐参数解释

```make
run: iso
	qemu-system-x86_64 \
		-cdrom build/opsos.iso \
		-m 256M \
		-serial stdio \
		-d int,cpu_reset,guest_errors \
		-drive file=disk.img,if=none,id=vd,cache=writethrough \
		-device virtio-blk-pci,drive=vd,disable-modern=on
```
（`Makefile:326-333`）

| 参数 | 含义 | 本项目为什么需要 |
| --- | --- | --- |
| `-cdrom build/opsos.iso` | 挂一张 CD-ROM，SeaBIOS 从 El Torito 引导记录启动 GRUB | GRUB 再从 ISO 里的 `/boot/kernel.elf` 以 multiboot2 协议加载内核（`boot/grub.cfg:20-24`） |
| `-m 256M` | guest 物理内存 256 MiB | init 自检会打印可用页数：实测 256 MiB 下报 `63870 pages, 249 MB`（打印格式见 `user/services/init/main.c:254-258`） |
| `-serial stdio` | COM1 的字符流接到当前终端 | 内核 `SerialPuts` 与服务 `debug_log` 全在 COM1；这是**唯一**能看到启动日志的通道（§1.1） |
| `-d int,cpu_reset,guest_errors` | 打开三类 QEMU 日志：每次中断/异常、CPU 复位、guest 错误 | 崩溃排查用；输出到 **stderr**，实测启动 12 秒约 2500 行，会淹没终端。要留档就改 `-D qemu.log`，不需要时删掉这一项 |
| `-drive file=disk.img,if=none,id=vd,cache=writethrough` | 把宿主文件定义成块后端 `vd`，但**不**挂到总线 | `cache=writethrough` 让每次写立刻落盘，配合驱动的同步 RMW 语义（`user/services/vfs/fs_virtio_blk_driver.c:43-48`）；文件名写死为 `disk.img`，缺文件 QEMU 直接报错退出 |
| `-device virtio-blk-pci,drive=vd,disable-modern=on` | 把 `vd` 作为 virtio-blk PCI 设备挂上，强制 **legacy**（非 modern）virtio 接口 | 用户态驱动按 PCI ID `0x1AF4/0x1001` 扫设备（`user/services/vfs/fs_virtio_blk_driver.c:58-61`）；自查 `qemu-system-x86_64 -device virtio-blk-pci,help` 能看到 `disable-modern=<bool>` |

关于显示：`make run` **没有**传 `-display`/`-nographic`，QEMU 会用编译时选定的默认后端（GTK/SDL/VNC）。实测这台机器上的 QEMU 10.2.2 回退到 VNC 并打印：

```text
VNC server running on ::1:5900
```

此时用任意 VNC 客户端连 `localhost:5900` 就能看到屏幕；用 §5.3 的 `-nographic` 则把一切收进终端。

### 5.2 `disk.img`：唯一持久化面

- **作用**：virtio-blk 后端，被用户态驱动格式化成 `Disk` 卷（RW），是系统里**唯一**能跨重启保留数据的地方（`user/services/vfs/fs_virtio_blk_driver.c:23-26`）。内存卷 `Users`（32 MiB）与只读卷 `System` 由 `fs_mem_driver` 提供，重启即丢。
- **磁盘布局**（`user/services/vfs/fs_virtio_blk_driver.c:28-42`）：扇区 0 = 超级块（magic `VBDK`、块大小、inode 表几何、卷 UUID、`root_inode=1`）；随后是 256 个 inode、每个 128 字节的 inode 表（挂载时整表读进 32 KB 内存数组，每次修改同步 RMW 写回）；再往后是文件数据，每个 inode 一段连续 extent。空闲位图不落盘，挂载时从 inode 表重建，因此崩溃不会丢失分配状态。
- **创建**：

  ```bash
  qemu-img create -f raw disk.img 8M
  # 或（没有 qemu-img 时）
  dd if=/dev/zero of=disk.img bs=1M count=8
  ```

  8 MiB 与仓库脚本一致（`scripts/smoke_test.py:52` 指向 `disk.img`；`docs/test_report.md:15` 记为 8 MiB）。实测启动后驱动报 `16384 sectors, 512 bytes/sector` 与 `Disk volume ready - 8063 KiB RW`。
- **首次启动会自动格式化**：驱动在超级块 magic 不存在时执行格式化流程（`user/services/vfs/fs_virtio_blk_driver.c:70-75`），所以一块全零的新盘可以直接用。
- **建议显式写 `format=raw`**：Makefile 的 `-drive` 没写 `format`，QEMU 会打印

  ```text
  WARNING: Image format was not specified for 'disk.img' and probing guessed raw.
           Automatically detecting the format is dangerous for raw images, write operations on block 0 will be restricted.
           Specify the 'raw' format explicitly to remove the restrictions.
  ```

  自己写命令时请加 `format=raw`（`-drive file=disk.img,format=raw,if=none,id=vd,cache=writethrough`）。
- **重置磁盘**：删掉 `disk.img` 重新 `qemu-img create` 即可，下次启动会重新格式化；卷 UUID 在格式化时生成并持久化（`fs_virtio_blk_driver.c:50-52`），所以重建镜像后旧的 security-scoped bookmark 会失效。

### 5.3 无图形环境

三种可行姿势：

```bash
# A. 只要日志：不要显示设备，串口直接进终端（Ctrl-A X 退出）
qemu-system-x86_64 -cdrom build/opsos.iso -m 256M -nographic \
  -serial mon:stdio \
  -drive file=disk.img,format=raw,if=none,id=vd,cache=writethrough \
  -device virtio-blk-pci,drive=vd,disable-modern=on

# B. 无显示、日志落文件（适合长跑与事后检索）
qemu-system-x86_64 -cdrom build/opsos.iso -m 256M -display none -monitor none \
  -serial file:build/serial.log \
  -drive file=disk.img,format=raw,if=none,id=vd,cache=writethrough \
  -device virtio-blk-pci,drive=vd,disable-modern=on

# C. 有显示但要脚本化取证：VNC + monitor socket（scripts/smoke_test.py 的做法）
qemu-system-x86_64 -cdrom build/opsos.iso -m 256M -vnc 127.0.0.1:0 \
  -serial file:build/serial.log \
  -monitor unix:/tmp/opsys-mon.sock,server=on,wait=off
```

`-nographic` 下 QEMU 的快捷键：`Ctrl-A C` 切到 monitor、`Ctrl-A X` 退出、`Ctrl-A H` 帮助。`make debug` 内部就是用 `-nographic -serial mon:stdio`（`Makefile:335-345`）。

### 5.4 GRUB 菜单与引导链

`boot/grub.cfg` 很短，关键行：

| 行 | 内容 | 作用 |
| --- | --- | --- |
| `boot/grub.cfg:1-2` | `set timeout=10` / `set default=0` | 菜单停 10 秒，默认选第 0 项 |
| `boot/grub.cfg:4-7` | `insmod all_video` / `gfxterm` / `serial`；`serial --unit=0 --speed=115200` | 同时启用图形终端与串口终端 |
| `boot/grub.cfg:11` | `loadfont /boot/grub/fonts/unicode.pf2` | 没有字库时 gfxterm 会静默退回文本控制台（注释 `boot/grub.cfg:9-10`） |
| `boot/grub.cfg:15-16` | `set gfxmode=1024x768x32`；`set gfxpayload=keep` | 强制线性帧缓冲，交给内核的 `SYS_FB_GET_INFO`/`SYS_FB_MAP` 路径使用（注释 `boot/grub.cfg:12-14`） |
| `boot/grub.cfg:17-18` | `terminal_input serial console`；`terminal_output serial gfxterm` | **GRUB 菜单本身也会出现在串口上**（实测日志开头就是 `GRUB version 2.12` 与菜单文本） |
| `boot/grub.cfg:20-24` | `menuentry "OpSys"`，内含 `insmod multiboot2` + `multiboot2 /boot/kernel.elf` + `boot` | 正常启动路径 |
| `boot/grub.cfg:26-29` | `menuentry "Diagnostics: videoinfo"`，内含 `videoinfo` + `halt` | 显示模式诊断项：打印 GRUB 当前图形适配器信息后停机，**不进内核** |

引导链：

```text
SeaBIOS --(El Torito)--> GRUB 2（ISO 内 core.img；gfxmode=1024x768x32, gfxpayload=keep）
   |   menuentry "OpSys" -> multiboot2 /boot/kernel.elf
   v
boot.asm：保护模式 -> 校验 multiboot2 -> CPUID/长模式 -> 恒等+高半区页表
          -> PAE/EFER.LME/分页 -> 64 位 GDT -> far jump -> call KernelMain()
   v      (kernel/arch/x86_64/boot.asm:18-25,104,554-555)
KernelMain()：GDT/IDT/PMM/VMM/调度/线程/能力/IPC/Mutex/Syscall 初始化
          -> 建 init（PID 1）-> 加载 init blob -> 映射用户栈
          -> "Transitioning to ring 3..."  (kernel/kernel_main.c:89,94-408)
```

### 5.5 退出、复位与关机

| 想要 | 做法 |
| --- | --- |
| 退出 QEMU（`-nographic`） | `Ctrl-A X` |
| 退出 QEMU（monitor 可用时） | monitor 里敲 `quit` |
| 硬复位 guest（保留 `disk.img` 数据） | monitor 里敲 `system_reset`——自动化脚本靠它验证持久化（`scripts/smoke_test.py:604-640`） |
| 在 guest 内关机 | shell 的 `shutdown`（注册于 `user/services/shell/shell.c:3780`）；注意 `reboot` 受能力门控，未授权会返回 `ERR_NOCAP` |

---

## 六、首次启动会看到什么

### 6.1 时间线

```text
 0s  +- GRUB 菜单（屏幕 + 串口），10 秒倒计时，默认 "OpSys"
10s  +- boot.asm 切长模式 -> KernelMain：逐行 "XXX initialized"（串口）
     +- 创建 init（PID 1）、加载 blob -> "Transitioning to ring 3..."
     +- init 自检（串口）："init: Starting (PID 1)" -> "=== Results: N/N passed ==="
     |    -> "init: fetched manager.elf blob" -> "init: service manager PID=k"
     +- manager 拉起服务（串口）：serial（含自检）-> term -> keyboard -> gui -> net
     |    -> flaky（故意退出 7，重启 3 次后 FAILED）-> MANAGER_OK -> vfs
     |    -> fs_mem_driver -> perm -> fs_virtio_blk_driver -> device_mgr -> pkg
     |    -> user -> wm -> policy -> shell
     +- term 画 boot splash（屏幕，居中）："OpSys Microkernel"
     +- shell 首次写屏幕：清掉 splash，画 banner 与提示符 "opsys:/$"
     +- init 收尾自检（串口）：P1/P2/P2V/KBD/P3/P4/P5
         -> "init: ALL SELFTESTS PASSED (n/n)" -> "init: entering idle loop"
```

自检耗时以十秒计（P4 会创建上千线程再回收）。启动期间串口日志在滚动、屏幕停在 splash，都是正常的。

### 6.2 串口侧（实测原文摘录）

```text
OpSys kernel starting...
  ASLR: RNG seeded, canary=0x........
  multiboot2 info at physical 0x......
  kernel phys base: 0x100000
  GDT initialized
  IDT initialized
  PMM initialized
  VMM initialized
  Scheduler initialized
  Threads initialized
  Capabilities initialized
  IPC initialized
  Mutexes initialized
  Syscalls initialized
  Creating init process...
  Init process created (PID 1), heap_base=0x........
  Loading init ELF...
  ELF loaded, entry=0x400000
  Mapping user stack...
  Transitioning to ring 3...

init: Starting (PID 1)
=== Syscall Tests ===
  TEST: DebugLog(ring3->ring0->ring3) ... PASS
  ...
=== Results: 34/34 passed ===
init: fetched manager.elf blob (28104 bytes)
init: service manager PID=5
manager: serial service ready (port 7)
manager: running serial self-test
serial-test: PASS - serial service verified
manager: starting display services
manager: keyboard started (PID=8)
manager: gui started (PID=9)
manager: net started (PID=10)
manager: flaky exited (code 7), restart 1/3
manager: flaky marked FAILED
manager: MANAGER_OK
manager: starting VFS services
manager: starting Powerbox
manager: fs_virtio_blk_driver started (PID=18)
manager: device_mgr started (PID=19)
manager: pkg started (PID=20)
manager: user started (PID=21)
manager: wm started (PID=22)
manager: policy started (PID=23)
manager: starting shell
...
=== P1 Permissions: 10/10 passed ===
=== P2 Gate: 5/5 passed ===
=== P2 VFS: 4/4 passed ===
=== KBD Focus: 1/1 passed ===
=== P3 Crash Recovery: 1/1 passed ===
=== P4 Resource Exhaustion: 2/2 passed ===
=== P5 Zero-Copy Read: 1/1 passed ===
init: ALL SELFTESTS PASSED (34/34)
init: entering idle loop
```

上述文本来自本机 QEMU 10.2.2 加 `-m 256M` 的一次完整启动（`-serial file:...` 抓取）。各行的**生成位置**是：

| 文本 | 出处 |
| --- | --- |
| `OpSys kernel starting...` 与各 `XXX initialized` | `kernel/kernel_main.c:94,119-167` |
| `Transitioning to ring 3...` | `kernel/kernel_main.c:408` |
| `init: Starting (PID 1)` | `user/services/init/main.c:2504` |
| `=== Results: N/N passed ===` | `user/services/init/main.c:1055` |
| `init: fetched manager.elf blob`、`init: service manager PID=` | `user/services/init/main.c:2522,2529` |
| `proc: CREATE pid=N name=shell` | `kernel/process/process.c:259` |
| P1 / P2 / P2V / KBD / P3 / P4 / P5 套件标题行 | `user/services/init/main.c:1521,1651,2091,2206,2284,2379,2481` |
| `init: ALL SELFTESTS PASSED (n/n)`、`init: entering idle loop` | `user/services/init/main.c:2580-2581` |
| `user: DEFAULT admin/admin created - CHANGE THE PASSWORD (passwd)` | `user/services/user/main.c:968` |
| `manager: flaky marked FAILED` | 重启策略上限 `MAX_RESTARTS 3`（`user/services/manager/manager.c:119,452-461`） |

### 6.3 屏幕侧：splash 到提示符

1. **term 服务启动时先画 splash**（`user/services/term/term.c:2131-2156`）：`TermClear()` 清屏，居中两行 `OpSys Microkernel` / `starting services...`（字面量在 `term.c:2135-2136`），然后才注册 `term` 端口（`term.c:2158`）——这样 splash 绘制期间不会有客户端写进来（注释 `term.c:2122-2130`）。同时置位 `s_splash_clear`（`term.c:2132`）。
2. **第一个客户端写屏幕时清掉 splash**（`term.c:1022-1030` 的 `s_splash_clear` 分支），于是 shell 的 banner 完整取代 splash，包括那两行居中文本。banner 内容见 `user/services/shell/shell.c:1477-1485`。
3. **提示符**：`PS1` 未设置时由 `snprintf(out, outsz, "opsys:%s$ ", s_cwd)` 生成（`user/services/shell/shell.c:588-600`），初始 `s_cwd = "/"`（`shell.c:106`），所以首次看到的是 `opsys:/$`。

用 `screendump` 加 `tools/vga_decode.py` 解出来的首屏（本机实测，1024x768 对应 113x38 字符网格）是：

```text
Welcome to OpSys
  Copyright (c) 2026 OpSys Project
  shell.c - Simple terminal shell (TTY-like)
  Type 'help' for a command list.

opsys:/$
```

### 6.4 自检套件与"跑对了"的判据

init 依次跑 8 组自检，**任一组不满分就停机**（`BootSelftestFail` 打印 `!!! SELFTEST FAILURE: <suite> n/m passed !!!` 后死循环 `ThreadYield`，`user/services/init/main.c:2494-2499`）：

| 套件 | 打印头（源码行） | 覆盖内容 |
| --- | --- | --- |
| 经典 syscall 套件 `RunTests()` | `=== Results: n/n passed ===`（`init/main.c:1019,1055`） | syscall 往返、IPC、能力、内存映射、线程、睡眠、压力基准 |
| P1 权限引擎 `RunP1PermTests()` | `=== P1 Permissions: n/n passed ===`（`init/main.c:1507,1521`） | 身份/角色/规则链/能力签发，跑在活的 vfs + perm 栈上 |
| P2 syscall 门控 `RunP2GateTests()` | `=== P2 Gate: n/n passed ===`（`init/main.c:1644,1651`） | 敏感 syscall 的能力校验（纯内核，无 IPC） |
| P2 VFS 授权 `RunP2VfsTests()` | `=== P2 VFS: n/n passed ===`（`init/main.c:2085-2091`） | 书签创建/解析、五个 VFS op 的门控与能力抹位 |
| KBD 焦点 `RunKbdFocusTests()` | `=== KBD Focus: n/n passed ===`（`init/main.c:2203-2206`） | 活键盘服务的 TAKE/RELEASE_FOCUS 所有权 |
| P3 崩溃恢复 `RunCrashRecoveryTests()` | `=== P3 Crash Recovery: n/n passed ===`（`init/main.c:2281-2284`） | kill pkg 后 manager 自动重启、端口恢复 |
| P4 资源耗尽 `RunResourceExhaustionTests()` | `=== P4 Resource Exhaustion: n/n passed ===`（`init/main.c:2375-2379`） | 超长 IPC 消息、线程表耗尽后的优雅 `ERR_NOMEM` |
| P5 零拷贝 `RunZeroCopyTests()` | `=== P5 Zero-Copy Read: n/n passed ===`（`init/main.c:2478-2481`） | 池后端 blob 经共享内存只读映射，内容与分块读一致 |

判定"启动成功"的最小信号集（自动化脚本用的就是这些锚点，见 `scripts/smoke_test.py` 的 `SERIAL_ANCHORS`）：

```text
init: ALL SELFTESTS PASSED          <- 全部自检通过
proc: CREATE pid=<n> name=shell     <- shell 进程被拉起
opsys:<cwd>$                        <- 屏幕上的提示符（VGA 通道）
```

### 6.5 登录与账户

- **不会自动登录**：shell 起来后直接给提示符，`login` 是一条普通命令（注册于 `user/services/shell/shell.c:3808`，实现在 `shell.c:3889-3937`）。
- **默认账户**：账户表为空时 user 服务在启动时自举创建 `admin`，角色 `OWNER`，密码 `admin`（`user/services/user/main.c:955-968`），并在串口提示改密。
- **登录**：

  ```text
  opsys:/$ login admin
  Password: *****                     <- 掩码回显，输入 admin 后回车
  login: ok - 'admin' (OWNER)
  ```

  也可以一条命令写完：`login admin admin`（`scripts/verify_users.py:186-190` 的自动化就是这么做的）。密码走 `ReadLineMasked`，回显 `*`（`shell.c:3905-3912`）。
- **为什么要登录**：权限模型取消了 root，敏感操作按角色放行。未登录时 `users`、`stop`、`kill` 等会被拒绝并给出提示（见 [permission_model.md](permission_model.md)）。
- **改密**：`passwd`（`shell.c:3811`）。
- **账户表在内存里**：user 服务启动时清空账户表、为空时重建 `admin/admin`（`user/services/user/main.c:944-968`），**不落盘**。所以"忘记密码"的正解是重启 QEMU（§9）。
- 用完可以 `logout`（`shell.c:3809`）。

### 6.6 看起来像故障、其实正常

| 现象 | 为什么正常 |
| --- | --- |
| 串口出现 `manager: flaky exited (code 7), restart 1/3` 直到 `manager: flaky marked FAILED` | `flaky` 是故意退出 7 的演示服务，重启 3 次后标记 FAILED，这正是重启策略被验证通过（`manager.c:119,435-461`） |
| 串口出现 `user: DEFAULT admin/admin created` | 首次启动的正常自举提示，不是安全问题（建议登录后立刻 `passwd`） |
| 屏幕 splash 停很久 | splash 在注册 term 端口前绘制，要等 init 自检跑完、shell 写第一行才被清掉 |
| `serial-test: real RX - no bytes observed (none injected)` | 自检会等外部往 COM1 注入字节，没人注入时这句是预期输出 |
| QEMU 打印 `WARNING: Image format was not specified ...` | Makefile 的 `-drive` 未写 `format=raw`（§5.2） |

---

## 七、调试

### 7.1 GDB：`make debug`

`make debug`（`Makefile:335-345`）等价于：

```bash
qemu-system-x86_64 -cdrom build/opsos.iso -m 256M -nographic \
  -serial mon:stdio -s -S \
  -drive file=disk.img,if=none,id=vd,cache=writethrough \
  -device virtio-blk-pci,drive=vd,disable-modern=on
```

`-s` 在 1234 端口开 GDB stub，`-S` 让 CPU 停在复位状态等你连接。另一个终端：

```text
$ gdb kernel.elf
(gdb) target remote :1234
(gdb) break KernelMain          # 内核 C 入口，kernel/kernel_main.c:89
(gdb) continue
(gdb) info registers            # 查看寄存器
(gdb) x/10i $pc                 # 反汇编当前位置
(gdb) stepi                     # 单条指令
```

三个容易踩的坑：

1. **符号名是 `KernelMain`，不是 `kernel_main`**。全仓库函数名已统一 PascalCase（见提交 `4877fcd` 内核侧、`aa2b97b` 用户态），`scripts/run.sh:112` 里 `break kernel_main` 的提示是旧的；用 `nm kernel.elf` 配合 `grep KernelMain` 可自查（应输出 `T KernelMain`）。`boot.asm` 也是通过 `extern KernelMain` 调用它的（`kernel/arch/x86_64/boot.asm:104,554-555`）。
2. **没有源码级单步**：`CFLAGS` / `USER_CFLAGS` 都不含 `-g`（`Makefile:32-38,45-54`），所以只有符号表级调试（函数名、全局变量、地址）。需要行级单步时，在本地给 CFLAGS 末尾追加 `-g` 后 `make clean && make iso`（改动不入库）。
3. **`-S` 下"什么都没发生"是正常的**：CPU 还没执行一条指令，串口自然没有输出；`continue` 之后才会看到 `OpSys kernel starting...`。

`make debug` 用了 `-nographic`，所以串口日志在运行 QEMU 的终端里、GDB 在另一个终端里，互不干扰。

### 7.2 串口日志的三种取法

| 方式 | 命令 / 位置 | 适合 |
| --- | --- | --- |
| 直接看 | `make run`（`-serial stdio`） | 交互启动，看一遍自检 |
| 落文件 | `-serial file:build/serial.log`（§5.3 姿势 B） | 长时间跑、事后检索锚点 |
| 自动化取用 | `-serial file:build/serial.log` 加 monitor socket（`scripts/smoke_test.py:48-50,650-666`） | 回归脚本 |

> **关于 `scripts/run.sh --serial`**：脚本在已有的 `-serial mon:stdio` 之后再追加一条 `-serial file:build/serial.log`（`scripts/run.sh:84-95`）。QEMU 按出现顺序把第 N 条 `-serial` 绑定到第 N 个串口，因此这条追加项对应的是 **COM2**；而内核日志与 serial 服务只驱动 COM1（`user/services/serial/serial.c:111-122`），该文件会一直是 0 字节。要留档请用 §5.3 姿势 B——把**第一条** `-serial` 直接指向文件（实测这样能得到 150 KB 以上的完整启动日志）。

### 7.3 屏幕取证：monitor `screendump` 加 `tools/vga_decode.py`

当你要确认"屏幕上到底写了什么"（shell 输出、Powerbox 面板、GUI 窗口），唯一可靠的办法是抓屏再解码：

```text
 QEMU：term/gui 画进线性帧缓冲（1024x768x32，9x20 像素/单元）
   |  1) monitor 执行 screendump /tmp/s.ppm
   v  2) 宿主得到 /tmp/s.ppm（P6 PPM，约 2.3 MB）
 宿主  3) python3 tools/vga_decode.py /tmp/s.ppm
      4) 输出 113x38 文本网格（逐行打印）
```

交互式（有 QEMU 窗口时）：

```text
Ctrl-Alt-2          <- 切到 monitor
(qemu) screendump /tmp/screen.ppm
(qemu) sendkey l
Ctrl-Alt-1          <- 切回显示
```

脚本化（等价于 `scripts/smoke_test.py`）：

```bash
# 启动时挂上 monitor socket（脚本里的做法见 smoke_test.py:650-666）
qemu-system-x86_64 -cdrom build/opsos.iso -m 256M -vnc 127.0.0.1:0 \
  -serial file:build/serial.log \
  -monitor unix:/tmp/opsys-mon.sock,server=on,wait=off &

# 让 monitor 抓屏（脚本用 python socket 发一行命令，这里用 socat 等价演示）
printf 'screendump /tmp/screen.ppm\n' | socat - UNIX-CONNECT:/tmp/opsys-mon.sock

# 把 PPM 解码成文本
python3 tools/vga_decode.py /tmp/screen.ppm
```

`tools/vga_decode.py` 做的事（`tools/vga_decode.py:15-27,33-55,73-112`）：

1. 解析 P6 PPM（`parse_ppm`）；
2. 从 `user/services/term/font.h` 里按**块顺序**抽出 `s_font[95][16]` 字模（0x20 起 95 个可打印字符；按块顺序而非注释字符解析，是为了绕开 `'\''` 这类转义写法）；
3. 按 9x20 像素单元切格，每格取 8x16 位图，用亮度阈值 400 判亮暗，再与字模做汉明距离最近匹配（上限 8 位错误，同时尝试正常与反色两种解释，后者命中光标格）；
4. 逐行打印文本网格（默认 1024x768 得到 113x38）。

注意 `screendump` 是**异步**落盘：monitor 会先回提示符再写文件，脚本因此轮询文件大小直到稳定（`scripts/smoke_test.py:170-182`）。

### 7.4 中断与崩溃排查

```bash
# 1) 中断/异常全量跟踪（写 stderr，实测启动 12 秒约 2500 行）
qemu-system-x86_64 ... -d int,cpu_reset,guest_errors
# 2) 同样的日志落文件，便于事后检索
qemu-system-x86_64 ... -d int,cpu_reset,guest_errors -D build/qemu-int.log
# 3) 三重故障不重启、不关机，停在现场（配合 -d 看最后一条异常）
qemu-system-x86_64 ... -no-reboot -no-shutdown -d int,cpu_reset,guest_errors
```

排查顺序建议：先看**串口**最后一行（服务名前缀会告诉你谁挂了），再看 `-d int` 里最后一条异常记录，最后用 GDB 在 `KernelMain` 或可疑函数下断点。内核自身的致命路径会走到 `panic()`（`kernel/panic.c`）；init 自检失败则停在 `BootSelftestFail` 的 `ThreadYield` 死循环（`user/services/init/main.c:2494-2499`），串口会明确打印 `!!! SELFTEST FAILURE: ...`。

### 7.5 仓库自带的自动化脚本

| 脚本 | 用途 | QEMU 姿势 |
| --- | --- | --- |
| `scripts/smoke_test.py` | R1 基线 + R2 盲区 + R3 压力三轮冒烟；`--drive` 追加磁盘持久化 | `-vnc 127.0.0.1:0` 加 `-serial file:` 加 `-monitor unix:`（`smoke_test.py:650-666`） |
| `scripts/verify_users.py` | 账户与退出保护（login/whoami/useradd/users/stop） | 同上，用 `sendkey` 逐字符注入（`verify_users.py:78-93`） |
| `scripts/verify_wm.py`、`scripts/verify_window_demo.py` | 窗口管理器与窗口演示 | 同上 |
| `scripts/verify_demos.py` | runtime / tui / hello / window 各 demo | 同上 |
| `scripts/accept.py` | Powerbox 书签全流程验收 | 同上 |
| `scripts/ops_pack.py` | `.ops` 包打包与检查（见 [ops_format.md](ops_format.md)） | 不需要 QEMU |

```bash
python3 scripts/smoke_test.py            # 基础三轮
python3 scripts/smoke_test.py --drive    # 追加 virtio-blk 持久化（会读写 disk.img）
```

这些脚本都会**自己启动一个 QEMU**，不要与 `make run` 同时跑，否则会抢 VNC 端口与 monitor socket。

---

## 八、改一个文件后的快速验证

```text
 编辑单个 .c/.h
   |
   +--(1) 只编这一个对象，秒级反馈语法/类型错误
   |        make build/kernel/ipc/ipc.c.o              # 内核文件
   |        make build/user/services/shell/shell.c.o   # 用户服务文件
   |
   +--(2) 重建内核 + 每个服务 ELF + blob + ISO：make -j"$(nproc)" iso
   |
   +--(3) make run，核对锚点：init: ALL SELFTESTS PASSED (n/n)
            manager: starting shell / 屏幕出现 opsys:/$
```

对象路径就是源码路径前面加 `build/`、后面加 `.o`（`Makefile:156-166` 的 `patsubst`），例如：

| 改了什么 | 单文件编译命令 |
| --- | --- |
| `kernel/ipc/ipc.c` | `make build/kernel/ipc/ipc.c.o` |
| `kernel/arch/x86_64/boot.asm` | `make build/kernel/arch/x86_64/boot.asm.o` |
| `user/services/shell/shell.c` | `make build/user/services/shell/shell.c.o` |
| `user/lib/libc/string.c` | `make build/user/lib/libc/string.c.o`（会被**所有**服务 ELF 重新链接） |
| `user/runtime/crt0.S` | `make build/user/runtime/crt0.S.o` |

要点：

- **单文件 `.o` 成功不等于系统能跑**，它只证明编译通过；链接问题（符号缺失、段冲突）要到 `make iso` 才暴露。
- 改了共享库（`user/lib/**`、`user/runtime/**`）会触发 27 个服务 ELF 全部重链与 27 个 blob 重建，比改单个服务慢得多，属正常。
- 改了**服务入口**（如 `shell.c`）后务必跑完整 `make iso`：`user.ld` 用 `--gc-sections`（`Makefile:57`）裁剪未引用段，只有重新链接才能看到真实体积与符号解析结果。
- 想复用上一块 `disk.img` 里的数据（例如验证持久化），别删盘；想从零开始就重新 `qemu-img create`（§5.2）。

---

## 九、故障排查速查表

| 症状 | 最可能的原因 | 处理 | 依据 |
| --- | --- | --- | --- |
| `make: nasm: No such file or directory` | 没装 NASM | 按 §2.3 安装，用 `command -v nasm` 自检 | `Makefile:13,238-244` |
| `cc1: error: unrecognized command-line option '-mstack-protector-guard=global'` | GCC 过老 | 升级 GCC（4.9 起，建议 9 以上） | `Makefile:32-34` |
| `ld: unrecognized option '-z'` 或 `unrecognized option 'separate-code'` | binutils 过老 | 升级 binutils（2.31 起）或安装 lld | `Makefile:42,56-57` |
| `make iso` 只报 `make: *** [Makefile:322: iso] Error 127`，没有其它信息 | `grub2-mkrescue` 不存在（该行 stderr 被 `2>/dev/null` 吞掉）或发行版叫 `grub-mkrescue` | 手工跑 `grub2-mkrescue --version` 与 `xorriso --version`；按 §2.3 建同名软链 | `Makefile:322-323` |
| GRUB 报 `error: file '/boot/kernel.elf' not found` | ISO 里没有内核（`iso` 步骤被中断，或手工拷了旧 ISO） | 重新 `make iso`；检查 `build/isodir/boot/kernel.elf` 是否存在 | `Makefile:318-321`、`boot/grub.cfg:22` |
| GRUB 报 `unknown command 'multiboot2'`，或菜单一片空白 | GRUB 模块没打进 ISO；字库缺失导致 gfxterm 回退 | 确认是 GRUB 2；`iso` 目标已带 `--modules="all_video gfxterm gfxterm_background font"` | `Makefile:322`、`boot/grub.cfg:9-11` |
| QEMU 直接退出：`Could not open 'disk.img': No such file or directory` | `disk.img` 不存在（文件名在 Makefile 里写死） | `qemu-img create -f raw disk.img 8M` | `Makefile:332` |
| QEMU 直接退出：`Could not open 'build/opsos.iso': ...` | ISO 没生成 | `make iso` | `Makefile:326,328` |
| `-s: Failed to find an available port: Address already in use` 或 `gdbstub: couldn't create chardev` | 1234 端口被另一个 QEMU 占着 | 用 `ss -ltnp` 找到并结束它，或改用 `-gdb tcp::1235` | 实测报错文本；`Makefile:336,343` |
| 终端一行输出都没有 | 盯错了通道（shell 不走串口），或 `-S` 冻结了 CPU | 看运行 QEMU 的终端（`-serial stdio`）；`make debug` 下先 `continue` | §1.1、`Makefile:330,343` |
| 串口日志滚完但屏幕一直停在 `OpSys Microkernel` | init 自检还没跑完（P4 会创建上千线程） | 等；或确认串口是否已出现 P4 套件行与 `ALL SELFTESTS PASSED` | `user/services/init/main.c:2570-2581` |
| 屏幕全黑且 QEMU 打印 `VNC server running on ...:5900` | 宿主 QEMU 没编 GTK/SDL，回退到 VNC | 用 VNC 客户端连上去，或改 `-nographic` / `-display none` | 实测（§5.1） |
| 出现 `WARNING: Image format was not specified ...` | `-drive` 未声明格式 | 自己写命令时加 `format=raw` | §5.2 实测 |
| `scripts/run.sh --serial` 生成的文件一直是 0 字节 | 追加的 `-serial` 落到 COM2，内核只用 COM1 | 用 §5.3 姿势 B（第一条 `-serial` 指向文件） | `scripts/run.sh:84-95`、`user/services/serial/serial.c:111-122` |
| 键盘敲字没反应 | Powerbox 面板抢走了键盘焦点，面板未应答前键入会丢 | 先应答面板（面板文本提示 `perm_answer <id> y/n`），再重新输入命令 | 行为记载于 `scripts/smoke_test.py:25-29`；面板文本见 `user/services/perm/perm-manager.c:699,715` |
| 屏幕出现 `!!! SELFTEST FAILURE: <suite> n/m passed !!!` 后卡住 | 某个自检套件失败，init 主动停止启动 | 按套件名查对应源码（§6.4 表）；配合 `-d int` 与 GDB 定位 | `user/services/init/main.c:2494-2499` |
| `break kernel_main` 提示找不到符号 | 符号已统一为 PascalCase | 改用 `break KernelMain` | `kernel/kernel_main.c:89` |
| GDB 能停住但 `list` / `next` 不能用 | 编译没带 `-g`，只有符号表 | 用 `x/10i $pc`、`info registers`；需要行级调试就临时给 CFLAGS 加 `-g` 重建 | `Makefile:32-38` |
| `make clean` 之后根目录还有 `kernel.elf` | `clean` 只删 `build/` | 手工 `rm kernel.elf` 才是彻底重建 | `Makefile:348-349` |
| 改了代码但行为没变 | 手工启动了旧的 `build/opsos.iso` | 用 `make run`（自动依赖 `iso`），或先 `make iso` | `Makefile:326` |
| 忘记 `admin` 密码 | 账户表在内存里，不落盘 | 重启 QEMU，自举会重新创建 `admin/admin`（盘上文件数据不受影响） | `user/services/user/main.c:944-968` |
| `make format` 报 `clang-format: command not found` | 没装 clang-format | 按 §2.3 安装（12 以上）；它不影响 `iso` | `Makefile:355-371` |
| `disk.img` 里的数据"没了" | 写到了内存卷 `Users`（重启即丢），或重建了镜像（UUID 变化导致旧书签失效） | 持久化要写到 `/Volumes/Disk/...`；重置镜像后旧 bookmark 需重建 | `user/services/vfs/fs_virtio_blk_driver.c:23-26,50-52` |

---

## 十、下一步读什么

| 你想做的事 | 去看 |
| --- | --- |
| 认识系统全貌 | [architecture.md](architecture.md) |
| 查某条 shell 命令怎么写 | [shell_reference.md](shell_reference.md) |
| 查服务端口与 IPC 协议 | [service_reference.md](service_reference.md) |
| 查 syscall 编号与门控 | [syscall_reference.md](syscall_reference.md) |
| 理解权限、登录与沙盒 | [permission_model.md](permission_model.md)、[ops_format.md](ops_format.md) |
| 写测试、跑回归 | [testing_guide.md](testing_guide.md)、[test_report.md](test_report.md) |
| 改代码（风格、纪律、陷阱） | [developer_guide.md](developer_guide.md) |
| 为什么内核只留这些机制 | [kernel_roadmap.md](kernel_roadmap.md)、[microkernel_audit.md](microkernel_audit.md) |

> 返回 [文档索引](README.md)
