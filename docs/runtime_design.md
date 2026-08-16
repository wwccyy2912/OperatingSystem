# OpSys Runtime (C Runtime) 设计文档

> 版本：v1.0  
> 日期：2026-08-14  
> 状态：核心运行库（init/exit/errno/malloc/crt0/sigrestore）已实现并编入构建；runtime_demo 未接入 Makefile，§12 测试计划未执行  
> 关联：user/runtime/、user/lib/libos/syscalls.h

---

## 一、设计概述

OpSys Runtime 是用户态 C 程序的启动和生命周期管理框架。它包括：

- **启动流程**（\_start → \_init → main → exit）
- **全局构造函数支持**（.init_array 数组）
- **内存管理**（malloc/free/calloc/realloc）
- **进程终止**（atexit 处理、\_exit）
- **错误码处理**（errno 机制）
- **信号处理**（signal/kill）

相比传统 POSIX 运行时，OpSys Runtime 集成了微内核特性：

- **ASLR 堆随机化**（每进程独立的 heap_base）
- **用户态自旋锁**（零系统调用的 malloc 快路径）
- **能力系统**（内存映射需要 capability）
- **信号 checkpoint**（lazy delivery）

---

## 二、启动流程

### 2.1 从 \_start 到 main

```
ELF entry point: _start (user/runtime/crt0.S)
  ↓
call _init()  → 执行 .init_array 全局构造函数
  ↓
call main(0, NULL)  → 应用程序入口
  ↓
call exit(retval)  → 执行 atexit 处理 + _exit
  ↓
thread_exit()  → 系统调用终止
  ↓
内核回收进程（清理 PML4、capability 表、IPC 端口）
```

### 2.2 crt0.S 实现

**文件**：`user/runtime/crt0.S`

```asm
_start:
    and rsp, -16            ; 栈对齐（16 字节边界）
    call _init              ; 全局构造函数（.init_array）
    xor edi, edi            ; argc = 0
    xor esi, esi            ; argv = NULL
    call main               ; 进入应用
    mov edi, eax            ; retval
    call exit               ; noreturn
    hlt / loop
```

**设计考量**：

- 无栈帧（不需要 rbp）
- argc = 0, argv = NULL（OpSys v0.1 不支持命令行参数）
- exit() 标记 noreturn（LLVM 可以消除 hlt 后的代码）

### 2.3 全局构造函数（.init_array）

**文件**：`user/runtime/init.c`

```c
void _init(void)
{
    for (init_func_t *p = __init_array_start; p < __init_array_end; p++) {
        if (*p)
            (*p)();
    }
}
```

**工作原理**：

1. 链接脚本（user.ld）定义 `__init_array_start` 和 `__init_array_end`
2. 编译器放置全局构造函数指针到 `.init_array` 段
3. \_init() 逐个调用它们
4. 返回后执行 main()

**示例**：

```c
struct Resource {
    Resource() { printf("Resource constructed\n"); }
    ~Resource() { printf("Resource destructed\n"); }
};

Resource r;  // 放在 .init_array，_init() 调用构造
            // 在 main 执行前输出 "Resource constructed"
```

---

## 三、堆内存管理

### 3.1 设计目标

- **零系统调用快路径**（P0 性能）：uncontended malloc/free 无 syscall
- **ASLR 支持**（design item ⑭）：heap_base 每进程随机化
- **守卫页保护**：heap_base ± 1 page 为 PROT_NONE（阻止溢出）
- **就地扩展**（in-place realloc）：减少大块数据的 O(n^2) copy

### 3.2 内存布局

```
User process VA space:
  ┌─────────────────────────────────────┐
  │ Kernel (0xFFFF...)                  │
  ├─────────────────────────────────────┤
  │ [stack region: 0x90000000-0x100000000] ← thread stacks, guard pages
  │ [vspace_alloc available]
  │ [ELF: 0x400000（固定链接基址）]
  ├─────────────────────────────────────┤
  │ [heap high guard: heap_base+256MB]  │ <- [heap_max, unmappable]
  │ [heap region: heap_base+0 to +256MB]│ <- grows upward
  │ [heap low guard: heap_base-4KB]     │ <- [heap_base-4KB, unmappable]
  ├─────────────────────────────────────┤
  │ [vspace_alloc available]
  │ [Data/BSS]
  │ [Text/RO]
  │ [0x00000000-0x40000000]             │ <- reserv
  └─────────────────────────────────────┘
```

**关键参数**（kernel/include/kernel/vmm.h）：

- HEAP_USER_BASE: 0x70000000 （compile-time default）
- HEAP_USER_SIZE: 0x10000000 （256 MB）
- ASLR 范围: [0x70000000, 0x78000000) @ 64 KB 粒度

### 3.3 分配器数据结构

```c
/* Block header (16 bytes) */
typedef struct block {
    size_t size;         /* Total block size (header + payload); bit 0 = FREE */
    struct block *next;  /* Free list link (valid only when free) */
} block_t;

#define BLOCK_HDR_SZ  sizeof(block_t) /* 16 */
#define MALLOC_ALIGN  16    /* payload alignment */
#define MALLOC_MIN_SIZE  64 /* smallest useful block (avoids fragmentation) */
#define CHUNK_SIZE    64 KB /* heap grow granule */
```

**大小编码**：

- FREE flag: bit 0 = 1（free），0（used）
- Size field: BLOCK_SIZE(b) = b->size & ~1

### 3.4 分配算法

#### 快速路径（malloc，大多数情况）

```
malloc(size):
  1. 计算 asize = BLOCK_HDR_SZ + round_up(size, 16)
  2. spin_lock(&s_heap_lock)
  3. first-fit 扫描 s_free_list
     - 若找到大小 ≥ asize 的块：
       - 切割（如果余数 ≥ 64）
       - 从 free list 摘除
       - 返回 (block + 16)
  4. 若 free list 无合适块：
     - heap_grow(asize)  → map_memory 系统调用
     - 循环回 3
  5. spin_unlock(&s_heap_lock)
  6. 若失败，errno = ENOMEM
```

#### 释放（free）

```
free(ptr):
  1. ptr = NULL → 忽略（POSIX 标准）
  2. spin_lock(&s_heap_lock)
  3. block = ptr - 16
  4. MARK_FREE(block)
  5. 插入到 free list 头
  6. coalesce_after(block)  → 与后继块合并
  7. spin_unlock(&s_heap_lock)
```

#### 合并（coalesce）

**为什么只向后合并？**

- Free list 是无序的；向前找需要整个列表扫描（O(n)）
- 在 malloc 时进行前向合并：从 free list 摘除后续块时自动合并

```c
static void coalesce_after(block_t *block)
{
    size_t block_sz = BLOCK_SIZE(block);
    block_t *next = (block_t *)((char *)block + block_sz);

    /* Scan free list for the block immediately following this one */
    for (block_t **pp = &s_free_list; *pp; pp = &(*pp)->next) {
        if (*pp == next) {
            /* Found it — merge */
            block->size = (block_sz + BLOCK_SIZE(next)) | 1;
            *pp = (*pp)->next;
            return;
        }
    }
}
```

#### 就地扩展（realloc）

**问题**：如果每次 realloc 都 malloc + memcpy + free，大块数据增长变成 O(n²)。

**解决**：尝试吸收后续的相邻空闲块，就地扩展。

```c
realloc(ptr, size):
  if (!ptr) return malloc(size);
  if (size == 0) { free(ptr); return NULL; }

  old_block = ptr - 16
  if (size <= old_payload)
      return ptr;  // 已有足够空间

  spin_lock(&s_heap_lock)

  /* Pass 1: 计算后续有多少连续空闲块 */
  contiguous_free = scan_subsequent_free_blocks(old_block)

  if (contiguous_free >= needed_size):
      /* Pass 2: 从 free list 摘除被吸收的块 */
      unlink_absorbed_blocks()
      /* 就地扩展 */
      old_block->size = needed_size
      /* 重新释放余下部分 */
      remainder->size = (contiguous_free - needed_size) | 1
      add to free list
      spin_unlock(&s_heap_lock)
      return ptr

  /* Fallback: 分配新块 + 复制 + 释放旧块（全在锁内） */
  newp = malloc_locked(size)
  memcpy(newp, ptr, old_payload)
  free_locked(ptr)

  spin_unlock(&s_heap_lock)
  return newp
```

**性能**：

- 小块：快速 free list 搜索 O(1) 平均
- 大块：就地扩展避免 O(n²) copy
- uncontended：自旋锁不触发上下文切换

### 3.5 堆增长（heap_grow）

```c
heap_grow(need):
  /* 第一次调用时获取内核的 ASLR heap base */
  if (!s_heap_layout_loaded):
      hb = get_heap_base()  /* SYS_GET_HEAP_BASE */
      s_next_virt = hb
      s_heap_max = hb + HEAP_USER_SIZE
      s_heap_layout_loaded = true

  /* 检查是否超出堆区域 */
  if (s_next_virt >= s_heap_max)
      return -1

  /* 确定 chunk 大小：至少 64 KB，加倍直到覆盖 need */
  chunk = CHUNK_SIZE  /* 64 KB */
  while (chunk < need && chunk < HEAP_USER_SIZE / 2):
      chunk *= 2

  /* 检查是否有足够的虚拟空间 */
  room = s_heap_max - s_next_virt
  if (chunk > room)
      chunk = room

  /* 创建内存 capability 并映射 */
  cap = cap_create(CAP_TYPE_MEM, RIGHT_WRITE)
  addr = map_memory(cap, s_next_virt, chunk, PROT_READ | PROT_WRITE)
      /* SYS_MAP_MEMORY: 创建分页表条目 */

  if (!addr):
      cap_revoke(cap)
      return -1

  cap_revoke(cap)  /* 释放 capability，映射持久化 */

  heap_add_chunk(addr, chunk)  /* 添加到 free list */
  s_next_virt += chunk
  return 0
```

**关键点**：

- 守卫页由内核在 [heap_base-4K, heap_base+256MB+4K) 边界拒绝映射
- Capability 由能力系统跟踪，不需应用管理生命周期
- ASLR: heap base 每进程随机化（kernel/rng.c）

### 3.6 线程安全

**v0.1（单线程）**：

- 简单全局 `s_heap_lock` spinlock
- 无竞争时自旋锁 0 syscall（快路径）

**v1.0+（多线程）**：

- 每个 thread-local 分配器（减少全局锁竞争）
- 或 malloc_arena[N] 的方法（如 tcmalloc/jemalloc）
- errno 需要 TLS 支持

---

## 四、进程终止

### 4.1 atexit 机制

**文件**：`user/runtime/exit.c`

```c
#define ATEXIT_MAX 32

static atexit_func_t s_atexit[ATEXIT_MAX];
static int s_atexit_count = 0;

int atexit(atexit_func_t func)
{
    if (s_atexit_count >= ATEXIT_MAX)
        return -1;
    s_atexit[s_atexit_count++] = func;
    return 0;
}

void exit(int code)
{
    /* Call atexit handlers in REVERSE order */
    for (int i = s_atexit_count - 1; i >= 0; i--) {
        if (s_atexit[i])
            s_atexit[i]();
    }
    _exit(code);
}

void _exit(int code)
{
    thread_exit(code);  /* SYS_THREAD_EXIT → 内核清理进程 */
    __builtin_unreachable();
}
```

**生命周期**：

1. main() 返回 → crt0.S 调用 exit(retval)
2. exit() 反向调用已注册的 atexit handler
3. \_exit() 触发 SYS_THREAD_EXIT syscall
4. 内核杀死进程所有线程，回收资源

### 4.2 C++ 析构

全局对象析构通过 \_\_cxa_finalize() 自动生成的函数集成：

- 编译器在 .init_array 后添加 .fini_array（OpSys 已支持）
- 或通过 atexit() 注册析构函数

**示例**：

```c
// Generated by __cxxabi
void __attribute__((destructor)) destroy_obj()
{
    obj.~Object();  // 调用析构
}
```

---

## 五、错误处理

### 5.1 errno 机制

**文件**：`user/runtime/errno.c`

```c
int __errno = 0;
```

**头文件**：`user/runtime/include/errno.h`

```c
extern int __errno;
#define errno __errno

#define ENOMEM  12
#define EINVAL  22
#define ENOSYS  38
/* ... */
```

**v0.1 限制**：全局 `__errno`（单线程）

**v1.0+ 扩展**：

```c
int *__errno_location(void)
{
    return &__thread_local_storage->errno;  /* TLS */
}
```

### 5.2 系统调用返回值约定

OpSys 系统调用遵循 Unix 约定：

- 成功：返回 ≥ 0
- 失败：返回负数，errno 包含错误码

**示例**（malloc.c）：

```c
void *malloc(size_t size)
{
    spin_lock(&s_heap_lock);
    void *result = malloc_locked(size);
    spin_unlock(&s_heap_lock);

    if (!result && size != 0)
        errno = ENOMEM;  /* 设置错误码 */
    return result;
}
```

---

## 六、信号处理

### 6.1 概述

OpenSys 实现了 POSIX 风格的进程级信号，支持：

- 64 个信号（SIGKILL 9、SIGUSR1 10、SIGSEGV 11 等）
- Per-process handler table（`proc->sig_handlers[]`）
- Process-wide pending bitmask（`proc->sig_pending`）
- Lazy delivery via checkpoints（syscall return、interrupt return）

> 注：用户态仅薄封装，内核 process/signal.c 仍为投递引擎；完整用户态信号库迁移 ⏸（见 kernel_roadmap §4.2 P2）

### 6.2 信号处理函数注册

**文件**：`user/lib/libos/syscalls.h`

```c
typedef void (*sighandler_t)(int);

sighandler_t signal(int signum, sighandler_t handler)
{
    /* 注册处理函数或获取当前处理函数 */
    /* 成功返回前一个处理函数，失败返回 SIG_ERR */
}

int kill(int pid, int signum)
{
    /* 向进程发送信号 */
    /* 返回 0 或错误码 */
}
```

**处理函数指针类型**：

```c
typedef void (*sighandler_t)(int);

void my_handler(int signum)
{
    printf("Caught signal %d\n", signum);
}

signal(SIGUSR1, my_handler);
signal(SIGTERM, SIG_IGN);   /* 忽略 SIGTERM */
signal(SIGSEGV, SIG_DFL);   /* 默认处理（通常终止） */
```

### 6.3 内核信号传递（kernel/process/signal.c）

**传递流程**：

1. kill(pid, sig) → proc->sig_pending |= (1LL << sig)
2. Checkpoint 检测（syscall return、interrupt return）
3. signal_check_common() 查找待处理的信号
4. 若 SIG_DFL：执行默认动作（ignore 或 terminate）
5. 若 SIG_IGN：忽略
6. 若处理函数：构建 sigframe，跳转到处理函数

### 6.4 sigframe 布局

**文件**：`kernel/include/kernel/signal.h`

```
User stack during signal handling:

  high addr   [original user stack...]
              [interruped context]

              ┌─────────────────────────────────┐
              │ sigframe_t (152 bytes):         │
              │  - 15 GPRs (rax-r15)            │
              │  - rip (handler return addr)    │
              │  - rflags                       │
              │  - rsp (original stack ptr)     │
              │  - signum (signal number)       │
              └─────────────────────────────────┤
              │ [8-byte restorer addr]          │  ← RSP at handler entry
              │ (__restore_rt)                  │
  low addr    (aligned to 16-byte boundary)
```

### 6.5 用户态信号返回跳板（sigrestore.S）

**文件**：`user/runtime/sigrestore.S`

```asm
__restore_rt:
    ; RSP = sigframe base
    ; 处理函数的 ret 弹出 [RSP-8] = __restore_rt
    ; 控制流到达这里

    mov rdi, rsp         ; sigframe 基址 → RDI
    mov eax, 50          ; SYS_SIGRETURN
    int 0x80

    ; 不应该返回
.halt:
    hlt
    jmp .halt
```

**生命周期**：

1. 内核设置 RIP = handler，RSP = base - 8，RDI = signum
2. 处理函数执行（可以调用 malloc、printf 等）
3. 处理函数 ret → \_\_restore_rt
4. \_\_restore_rt 调用 SYS_SIGRETURN
5. 内核从 sigframe 恢复原始上下文，继续执行被中断的代码

---

## 七、头文件组织

### 7.1 内部头文件

```
user/runtime/include/
  ├─ runtime.h          /* 内部声明（_init, atexit, exit） */
  ├─ errno.h            /* 错误码 */
  ├─ malloc.h           /* malloc API */
  └─ signal.h (新增)    /* 信号处理 */
```

### 7.2 公共头文件

应用包含（来自 libc/libos）：

```c
#include <malloc.h>        /* malloc, free, calloc, realloc */
#include <errno.h>         /* errno, E* constants */
#include <libos/syscalls.h> /* signal, kill */
#include <signal.h>        /* POSIX 标准信号常数 */
```

---

## 八、编译和链接

### 8.1 编译命令

```bash
# 汇编（NASM，Makefile: AS := nasm, ASFLAGS := -f elf64）
nasm -f elf64 user/runtime/crt0.S -o build/crt0.o

# C 编译
gcc -c user/runtime/init.c -o build/init.o
gcc -c user/runtime/malloc.c -o build/malloc.o
gcc -c user/runtime/exit.c -o build/exit.o
gcc -c user/runtime/errno.c -o build/errno.o

# 链接（用户程序）
gcc build/crt0.o build/init.o build/malloc.o build/exit.o build/errno.o \
    -nostdlib -T user.ld -o myapp.elf
```

### 8.2 链接脚本（user.ld）

```ld
ENTRY(_start)

SECTIONS {
    . = 0x400000;  /* 固定链接基址 */

    .text : { *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }

    .init_array : {
        __init_array_start = .;
        *(.init_array)
        __init_array_end = .;
    }

    .bss : { *(.bss*) *(COMMON) }

    .comment 0 : { *(.comment) }
}
```

---

## 九、性能特性

### 9.1 零系统调用快路径

**malloc 无竞争情况**：

```
malloc(size):
  1. spin_lock（无竞争 → 无上下文切换）
  2. free list 搜索（L1 缓存命中）
  3. 块分割和返回
  4. spin_unlock

总耗时：~100-200 纳秒（无 syscall）
```

### 9.2 ASLR 成本

```
malloc 首次调用成本：
  1. heap_grow() 检测到 s_heap_layout_loaded = false
  2. get_heap_base() → SYS_GET_HEAP_BASE syscall（一次性）
  3. 后续调用无成本
```

### 9.3 大块数据就地扩展

**场景**：文件 I/O 缓冲区逐行增长

```
无优化：
  write:  malloc(100) + ... + malloc(200)...
          每次 realloc → malloc + memcpy(0..100) + free
          总 copy 数据量：100 + 200 + 300 + ... = O(n²)

有优化（就地扩展）：
  realloc 尝试吸收后续空闲块
  无 copy，只改变 block->size
  总 copy = 0
```

---

## 十、已知限制与扩展计划

### 10.1 v0.1 限制

- ❌ 无 pthread 风格线程 API（errno 为全局单例；有 C11 threads 库）
- ❌ 无 TLS（thread-local storage）
- ❌ 无信号安全的 malloc（SIGKILL 时泄漏内存）
- ✅ .fini_array 已支持（C++ 全局析构）
- ❌ 无命令行参数（argc/argv）
- ❌ 无环境变量（getenv/setenv）

### 10.2 v1.0 扩展（Phase 3）

- ✅ TLS 支持（errno per-thread）
- ✅ 信号安全 malloc（async-safe）
- ✅ .fini_array 支持
- ✅ 线程本地 arena（减少 lock 竞争）
- ✅ 命令行参数传递

### 10.3 v2.0+ 规划

- Memory tagging extension (MTE) 集成
- Pointer authentication (PAC) 支持
- Memory-safe malloc variants
- Allocator 动态选择

---

## 十一、用例和最佳实践

### 11.1 启动过程

```c
// main.c
static Resource resource;  // 放入 .init_array

int main(void)
{
    // _init() 已调用 resource constructor

    resource.initialize();

    int ret = app_main();

    // exit() 将：
    // 1. 调用已注册的 atexit handlers
    // 2. 调用 resource destructor（如支持 .fini_array）
    // 3. 触发 SYS_THREAD_EXIT
    return ret;
}
```

### 11.2 内存分配

```c
// 申请 1 MB 缓冲
char *buf = malloc(1 << 20);
if (!buf) {
    printf("malloc failed: %d\n", errno);
    return 1;
}

// 增长缓冲
buf = realloc(buf, 2 << 20);  /* 就地扩展，无 copy */

// 释放
free(buf);
```

### 11.3 信号处理

```c
static void handle_sigterm(int sig)
{
    printf("Caught SIGTERM, cleaning up...\n");
    cleanup();
    _exit(0);
}

int main(void)
{
    signal(SIGTERM, handle_sigterm);

    while (1) {
        // 处理请求...
    }
}
```

### 11.4 atexit 用法

```c
static FILE *logfile = NULL;

void close_logfile(void)
{
    if (logfile)
        fclose(logfile);
}

int main(void)
{
    logfile = fopen("/log/app.log", "w");
    if (!logfile)
        return 1;

    atexit(close_logfile);  /* 进程退出时自动调用 */

    // ... app logic ...
}
```

---

## 十二、测试计划

### 12.1 单元测试

- [ ] malloc/free 基本操作
- [ ] realloc 就地扩展
- [ ] calloc 清零
- [ ] 分片（fragmentation）处理
- [ ] 堆增长（heap_grow）
- [ ] errno 设置正确性

### 12.2 集成测试

- [ ] \_init() 全局构造函数调用顺序
- [ ] atexit 反向执行顺序
- [ ] 信号传递和处理
- [ ] 堆和栈的分离（guard pages）

### 12.3 压力测试

- [ ] 1000 次 malloc/free
- [ ] 大块分配（>100 MB）
- [ ] 频繁 realloc 的文件 I/O
- [ ] 并发 malloc（v1.0+）

---

## 十三、文件清单

| 文件                             | 大小    | 说明          |
| -------------------------------- | ------- | ------------- |
| `user/runtime/crt0.S`            | 41 行   | 启动代码      |
| `user/runtime/init.c`            | 32 行   | 全局构造      |
| `user/runtime/exit.c`            | 49 行   | atexit + exit |
| `user/runtime/errno.c`           | 17 行   | 全局 errno    |
| `user/runtime/malloc.c`          | 374 行  | 堆分配器      |
| `user/runtime/sigrestore.S`      | 31 行   | 信号返回跳板  |
| `user/runtime/include/runtime.h` | 35 行   | 内部头        |
| `user/runtime/include/errno.h`   | 48 行   | 错误码        |
| `user/runtime/include/malloc.h`  | 19 行   | malloc API    |
| `user/runtime/include/signal.h`  | 新增    | 信号常数      |
| `docs/runtime_design.md`         | 本文件  | 设计文档      |

---

## 十四、参考和关联

### 内核设计

- kernel/process/signal.c - 信号传递实现
- kernel/mm/vmm.c - 虚拟地址空间管理
- kernel/mm/pmm.c - 物理内存管理
- kernel/syscall/syscall.c - SYS_MAP_MEMORY、SYS_SIGRETURN 等

### 库文件

- user/lib/libos/syscalls.h - 系统调用包装
- user/lib/libos/spinlock.h - 用户态自旋锁
- user/lib/libc/ - C 标准库（printf、memcpy 等）

### POSIX 标准

- IEEE 1003.1 (POSIX.1) 进程启动
- C99/C11 stdlib 动态内存
- POSIX 信号（IEEE 1003.1）
