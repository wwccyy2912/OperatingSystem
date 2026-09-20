# OpSys Runtime 快速参考指南

> **阅读提示**：本文是**设计基线文档**，记录做出决策时的思考与当时的现状快照。
> 其中标注为"规划/未实现"的条目可能已在后续版本落地。判断某项功能的**当前状态**，
> 请以代码、[../README.md](../README.md) 与 [architecture.md](architecture.md) 为准；
> 逐提交的实际进展见 [CHANGELOG.md](CHANGELOG.md)，文档地图见 [README.md](README.md)。

> 简明版，详见 docs/runtime_design.md

---

## 核心概念

### 进程生命周期

```
_start (crt0.S)
  ↓
_init()  ← 执行全局构造函数 (.init_array)
  ↓
main(0, NULL)  ← 应用入口
  ↓
exit(retval)  ← 反向调用 atexit handlers
  ↓
_exit()  → SYS_THREAD_EXIT
  ↓
内核清理进程
```

---

## 内存管理

### 快速分配

```c
#include <malloc.h>

/* 分配 1 KB */
void *buf = malloc(1024);
if (!buf) {
    perror("malloc");
    return;
}

/* 初始化为 0 */
int *arr = calloc(100, sizeof(int));

/* 扩展为 2 KB（就地扩展，无 copy） */
buf = realloc(buf, 2048);

/* 释放 */
free(buf);
free(arr);
```

### 堆布局（ASLR）

```
heap_base (随机，每进程不同)
  ↑
  │ [heap region: 256 MB]
  │ 应用 malloc 从这里分配
  │
[heap_base - 4KB] ← guard page（防溢出）
```

**获取 heap base**：

```c
#include <libos/syscalls.h>

uint64_t base = get_heap_base();
printf("My heap base: 0x%lx\n", base);
```

### 性能

- **无竞争**：malloc/free 无系统调用
- **大块扩展**：realloc 就地吸收后续空块（无 copy）
- **线程安全**（v0.1）：全局自旋锁

---

## 全局构造和析构

### C 风格（编译器属性）

```c
__attribute__((constructor))
void init_component(void)
{
    printf("Init before main()\n");
}

__attribute__((destructor))
void cleanup_component(void)
{
    printf("Cleanup after main() [v1.0+]\n");
}
```

### atexit 式（C 标准）

```c
#include <stdlib.h>

void cleanup(void)
{
    printf("Cleanup via atexit\n");
}

int main(void)
{
    atexit(cleanup);  /* 在 exit() 时反向调用 */
    return 0;
}
```

---

## 错误处理

### errno 机制

```c
#include <errno.h>

void *ptr = malloc(1 << 30);  /* 尝试 1 GB */
if (!ptr && errno == ENOMEM) {
    printf("Out of memory\n");
}
```

### 常见错误码

| 错误   | 值  | 含义           |
| ------ | --- | -------------- |
| ENOMEM | 12  | 内存不足       |
| EINVAL | 22  | 无效参数       |
| ENOSYS | 38  | 系统调用不支持 |

---

## 信号处理

### 基础用法

```c
#include <libos/syscalls.h>

static void handler(int sig)
{
    printf("Caught signal %d\n", sig);
}

int main(void)
{
    signal(SIGUSR1, handler);      /* 注册 */
    signal(SIGTERM, SIG_IGN);      /* 忽略 */
    signal(SIGSEGV, SIG_DFL);      /* 默认 */

    while (1) { sleep(1); }
}
```

### 常用信号

| 信号    | 号  | 默认动作 | 说明         |
| ------- | --- | -------- | ------------ |
| SIGTERM | 15  | 终止     | 正常终止请求 |
| SIGUSR1 | 10  | 终止     | 用户定义 1   |
| SIGUSR2 | 12  | 终止     | 用户定义 2   |
| SIGPIPE | 13  | 终止     | 管道断开     |
| SIGSEGV | 11  | 终止     | 段错误       |
| SIGKILL | 9   | 终止     | **不可捕获** |

### 限制（v0.1）

- ❌ 处理函数内无 malloc（可能死锁）
- ❌ 无信号掩码（sigprocmask）
- ❌ 无待处理查询（sigpending）
- ✅ POSIX 标准信号语义

---

## 调试和诊断

### 堆统计

```c
#include <libos/syscalls.h>

int free_pages = get_free_pages();
int pid = get_pid();
uint64_t heap_base = get_heap_base();

printf("PID=%d, free_pages=%d, heap_base=0x%lx\n",
       pid, free_pages, heap_base);
```

### 堆诊断（v0.9 已实现）

`user/runtime/include/malloc.h` 提供三个只读诊断接口，无需任何初始化：

```c
#include <malloc.h>

malloc_stats_t st;
MallocStats(&st);           /* 填充堆的当前形态，不分配内存 */

printf("heap  0x%x..0x%x (%d bytes)\n", st.heap_base, st.heap_end, st.heap_bytes);
printf("live  %d blocks / %d bytes, peak %d\n", st.blocks_live, st.used_bytes, st.peak_used);
printf("free  %d blocks / %d bytes, largest %d\n",
       st.blocks_free, st.free_bytes, st.largest_free);
printf("grow  %d calls / %d bytes, failures %d\n",
       st.grow_calls, st.grow_bytes, st.fail_count);

int problems = MallocCheck();   /* 0 = 一致；>0 = 问题数；-1 = 堆未初始化 */
size_t usable = MallocUsableSize(p);  /* 该分配实际可用的负载字节数 */
```

字段含义：`heap_base`/`heap_end` 是已经映射的堆区间（基址来自内核 ASLR），`used_bytes`/`free_bytes` 是**负载**字节（不含块头），`overhead` 是块头与对齐损耗，`largest_free` 是最大单块空闲（判断"还能不能分配下 N 字节"最有用），`peak_used` 是历史峰值，`fail_count` 是返回过 NULL 的次数。

`MallocCheck()` 真走一遍堆：块链完整性与尺寸合理性、空闲链是否成环、空闲块与活跃块是否重叠（由链精确覆盖证明）、是否越过堆末尾。发现问题时把详情写进串口调试日志并返回问题计数 —— 它是**只读**的，不会修改堆，因此可以在怀疑堆被写坏时随时调用。

### 标准文件 I/O（v0.9 已实现）

OpSys 没有内核文件描述符：文件是 vfs_server 持有的句柄。libc 把 `FILE` 做成一个**带缓冲的不透明记录**，真正的读写交给一个后端 vtable；`libfs` 通过 `.init_array` 构造函数安装 VFS 后端（`user/lib/libfs/stdio_vfs.c`），因此任何链接了共享用户对象的程序都能直接用：

```c
#include <stdio.h>

FILE *f = fopen("/Volumes/Disk/notes.txt", "w");
if (!f) { perror("fopen"); return 1; }
fprintf(f, "hello %d\n", 42);
fclose(f);

f = fopen("/Volumes/Disk/notes.txt", "r");
char line[128];
while (fgets(line, sizeof(line), f))
    printf("%s", line);   /* printf 仍走串口调试通道 */
fclose(f);
```

| 能力 | 说明 |
| --- | --- |
| 打开/关闭 | `fopen`（`r` `w` `a` `+` `b`）、`fclose`、`remove`、`rename` |
| 读写 | `fread`、`fwrite`、`fgetc`、`fputc`、`fgets`、`fputs`、`ungetc`（对待控制台也可用） |
| 定位 | `fseek`、`fseeko`、`ftell`、`rewind`（控制台流返回 -1 并置 `ESPIPE`） |
| 状态 | `feof`、`ferror`、`clearerr`、`fileno`（控制台返回 0/1/2，其它流返回 -1） |
| 格式化 | `fprintf` / `vfprintf` 写 FILE；`printf` / `puts` / `putchar` 仍写串口调试通道 |
| 解析 | `sscanf` / `vsscanf` / `fscanf`（`%d %i %u %o %x %p %c %s %a %e %f %g %n`、宽度前缀、`%*` 抑制赋值、长度修饰 `hh h l ll z t j L`） |
| 标准流 | `stdin` / `stdout` / `stderr`（走串口调试通道，`printf` 的既有行为不变） |
| 错误 | `perror`（输出到 stderr，并保留 errno） |

注意两点：路径是 **VFS URL**（`/Volumes/Disk/x`），libc 没有当前目录的概念；后端返回的负错误码会被映射成真正的 `errno` 值（不是简单取负的内核码）。没有安装后端时（只链 libc 的极端情况）`fopen` 失败并置 `errno = ENOSYS`，三个标准流照常工作。

### 对齐分配（v0.9 已实现）

```c
void *p = aligned_alloc(64, 4096);   /* C11：64 字节对齐，可 free() */
void *q = NULL;
int rc = posix_memalign(&q, 256, 1024);  /* POSIX：0 成功 / EINVAL / ENOMEM，不动 errno */
```

两个函数由**运行时堆**实现（`user/runtime/malloc.c`），与 `malloc`/`free`/`realloc` 共用同一套块头：对齐块携带反向指针，因此 `free()` 与 `realloc()` 都能正确释放/搬移。libc 的 `<stdlib.h>` 只做声明，不再提供内偏移指针的"弱回退"实现（那会让 `free()` 释放到块中间）。

### 退出与 atexit（v0.9 扩充）

```c
Atexit(cleanup);            /* 容量 32；满时返回非 0，且不覆盖已有项 */
__cxa_atexit(dtor, arg, dso);  /* C++ 兼容（独立 8 槽表） */
```

`exit()` 的顺序固定为：`atexit` 表 **LIFO** → `__cxa_finalize(NULL)` **LIFO** → `.fini_array` **逆序** → `_exit`。`exit()` 自带重入保护，handler 里再调用 `exit()` 不会递归。

### malloc 调试（规划中）

```c
/* 仍未实现：分配钩子、泄漏检测、valgrind 集成 */
```

---

## 编译示例

### 单个文件

```bash
gcc -nostdlib -T user.ld -o app.elf \
    build/crt0.o \
    build/init.o build/malloc.o build/exit.o build/errno.o \
    build/app.o
```

### 使用 Makefile

```makefile
RUNTIME_OBJ = crt0.o init.o malloc.o exit.o errno.o

app.elf: $(RUNTIME_OBJ) app.o
        ld -T user.ld -o $@ $^
```

---

## 已知限制

### v0.1（当前）

- 单线程（errno 全局）
- 无 TLS（线程本地存储）
- 无 malloc 钩子
- 无命令行参数

### v1.0（计划）

- ✅ TLS 支持
- ✅ 异步安全 malloc
- ✅ 多线程 arena 分配
- ✅ 内存标记扩展（MTE）

---

## 最佳实践

### ✅ DO

```c
/* 预分配 + 重用 */
char *buf = malloc(BUFSIZE);
while (reading) {
    read(fd, buf, BUFSIZE);  /* 无分配 */
}
free(buf);

/* atexit 清理资源 */
atexit(close_files);
atexit(flush_logs);

/* 检查 errno */
errno = 0;
void *p = malloc(size);
if (!p && errno == ENOMEM) {
    handle_oom();
}
```

### ❌ DON'T

```c
/* 信号处理内 malloc（v0.1 中可能死锁） */
void handler(int sig)
{
    char *buf = malloc(100);  /* 危险！ */
}

/* 不检查分配失败 */
int *arr = malloc(size);
arr[0] = 0;  /* 可能 SIGSEGV */

/* 过度碎片化 */
for (int i = 0; i < 1000000; i++) {
    free(malloc(i));  /* 内存碎片 */
}
```

---

## 常见问题

### Q: malloc(0) 返回什么？

A: OpSys 返回 NULL（标准行为）。

### Q: 能在全局构造内使用 malloc 吗？

A: 能，但要小心顺序。\_init() 先运行所有构造函数，malloc 初始化在之前。

### Q: realloc 何时就地扩展？

A: 当后续相邻块都是空闲时。内存碎片化会阻止就地扩展。

### Q: 信号处理内能调用 printf 吗？

A: v0.1 中 printf → malloc → 自旋锁，可能死锁。安全做法：

```c
void handler(int sig) {
    volatile int flag = 1;  /* 设置标志 */
}

int main() {
    while (!flag) { ... }  /* 检查标志 */
}
```

### Q: 如何检测内存泄漏？

A: v0.1 无工具。v1.0+ 规划 valgrind/AddressSanitizer 集成。

---

## 性能小贴士

### 1. 减少分配频次

```c
/* 慢 */
for (int i = 0; i < 1000; i++) {
    char *line = malloc(100);
    process(line);
    free(line);
}

/* 快 */
char *line = malloc(100);
for (int i = 0; i < 1000; i++) {
    process(line);
}
free(line);
```

### 2. 利用就地扩展

```c
/* 慢：多次分配 */
for (int i = 0; i < 100; i++) {
    buf = malloc(i * 1024);
    /* ... */
    free(buf);
}

/* 快：单次分配 + 就地扩展 */
size_t size = 1024;
void *buf = malloc(size);
for (int i = 0; i < 100; i++) {
    if (i * 1024 > size) {
        size *= 2;
        buf = realloc(buf, size);  /* 就地吸收相邻块 */
    }
    /* ... */
}
free(buf);
```

### 3. 避免过度锁竞争（v1.0+）

```c
/* v0.1 单线程，不适用 */
/* v1.0+ 多线程分配器：每线程独立 arena */
```

---

## 参考资源

- **完整设计**：docs/runtime_design.md
- **源代码**：user/runtime/
- **演示**：user/services/runtime_demo/main.c
- **内核接口**：user/lib/libos/syscalls.h
- **POSIX 标准**：IEEE 1003.1
