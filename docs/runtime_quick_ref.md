# OpSys Runtime 快速参考指南

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

### malloc 调试（v1.0+）

```c
/* 未实现，计划功能 */

/* 1. malloc 钩子 */
void *(*malloc_hook)(size_t size, const void *caller) = NULL;

/* 2. 内存泄漏检测 */
struct mallinfo info = mallinfo();
printf("total malloc'd: %d bytes\n", info.uordblks);

/* 3. valgrind 集成 */
#include <valgrind/memcheck.h>
VALGRIND_CHECK_MEM_IS_DEFINED(ptr, size);
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
