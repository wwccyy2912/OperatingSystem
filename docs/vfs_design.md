# OpSys VFS 服务设计文档（对象句柄 + 安全作用域书签）

> 版本：v0.2（Phase 0 ✅ / Phase 1 ✅ / Phase 2 ✅）
> 日期：2026-08-06
> 状态：Phase 0 完成并验收 ✅；Phase 1（virtio-blk 真实磁盘持久化）完成并验收 ✅；
> Phase 2（书签 + perm-manager + Powerbox）完成并验收 ✅
> 关联：docs/requirements.md（§1.2 文件系统服务、§4.2 IPC 协议）

---

## 〇、设计摘要

摒弃 POSIX「一切皆 fd」模型，VFS 作为独立用户态服务（`vfs_server`），以
**面向对象的资源句柄**（Volume / Item / FileHandle / Enumerator）承载文件
访问，以 **Security-Scoped Bookmark**（安全作用域书签）承载持久授权，以
**FSKit 风格的用户态驱动**承载底层存储格式。设计灵感来自 macOS
NSFileManager / Security-Scoped Bookmarks / APFS。

**本文档的目标**：把概念设计落成可直接实现的接口定义（C 结构体、IPC
协议、书签二进制格式、分阶段计划），并与 OpSys 现有代码库约束对齐。

---

## 一、现有代码库约束（设计必须遵守的事实）

| 约束 | 出处 | 对设计的影响 |
|------|------|--------------|
| IPC 消息上限 **4096 字节** | `kernel/include/kernel/types.h:82 MAX_MSG_SIZE` | 文件读写必须分块；大批量枚举需分批 |
| IPC 同步 `call` 风格 | `user/lib/libipc/ipc.c` → `ipc_call(port, req, req_len, resp, &resp_len)` | VFS 客户端协议沿用「请求结构体 + 响应结构体」 |
| 命名端口 `port_get("keyboard")` | `shell.c:870` | VFS 端口名定为 `"vfs"`，驱动端口名 `"vfs.fs.<driver>"` |
| 每进程能力表 + 跨进程 grant | `kernel/include/kernel/cap.h`（`cap_create_in_table` / grant / `rights_t`） | FileHandle 句柄直接复用能力语义；内核只加 `CAP_TYPE_FILE` 占位或全部走用户态表 |
| 服务由 manager 从 blob 拉起 | `manager.c:328 spawn_service` | `vfs_server` / `fs_*_driver` 加入服务表即可 |
| **无块设备驱动** | `kernel/arch/x86_64/` 仅 serial/rtc/rng/io | Phase 0 必须用内存卷（blob 只读卷 + RAM 可写卷），virtio-blk 留到 Phase 1 |
| 无密码学子系统 | — | 书签的 MAC 签名阶段化：先做服务端记录 + 随机 token，真 MAC 后置 |
| 无共享内存 IPC 路径 | 仅 `CAP_TYPE_MEM` 地基 | 零拷贝读取（§8.4）列为 Phase 3 |
| 类型风格 | `u8/u16/u32/u64/i32`、`_t` 后缀、`模块_动词_名词` 命名 | 本文档所有定义遵守 |

---

## 二、架构总览

```
┌─────────────┐   libfs（客户端库，静态链接进应用）
│  应用/服务   │   fs_open_item / fs_read / fs_create_bookmark ...
└──────┬──────┘
       │ ipc_call(port="vfs", req{op,...}, resp)
┌──────▼──────┐        ┌──────────────────┐        ┌─────────────────┐
│  vfs_server │◄──────►│  perm-manager    │        │  term（UI 代理） │
│ (用户态服务) │  授权   │  (权限记录+弹窗)  │◄──────►│  (Powerbox 询问) │
└──────┬──────┘        └──────────────────┘        └─────────────────┘
       │ FSKit-lite 挂载协议（§7）
┌──────▼──────┐
│ fs_mem_driver │  fs_ext4_driver │  fs_apfs_driver ...
│ (内存卷, Phase0)│ (virtio-blk, Phase1) │
└─────────────┘
```

要点：
- `vfs_server` 持有**全部命名空间状态**（卷表、Item 表、句柄表、书签表），
  驱动只负责「给定 itemID → 原始字节」。
- 路径字符串**只在客户端请求边界出现一次**；服务端内部一律用
  `(volume_id, item_id)` 二元组。客户端不持有路径时用书签。
- 设备（摄像头/GPU）不映射为文件；走 Capability + 服务代理（已有模式）。

---

## 三、对象模型（C 结构体定义）

### 3.1 统一标识：VolumeID 与 ItemID

```c
/* 卷 UUID —— 128 位，创建时由 vfs_server 生成 */
typedef struct { u64 hi; u64 lo; } vfs_uuid_t;

/* 卷内稳定 ID：inode 等价物。父级 ID 追踪 = 「文件移动后书签仍有效」的地基 */
typedef u64 vfs_item_id_t;

/* 全局资源定位符：唯一、稳定、可持久化（书签内嵌） */
typedef struct {
    vfs_uuid_t    vol;
    vfs_item_id_t id;
} vfs_resource_t;
```

### 3.2 Item（节点元数据）

```c
typedef enum {
    VFS_ITEM_FILE = 0,
    VFS_ITEM_DIR,
    VFS_ITEM_SYMLINK,        /* 仅 /System/usr 兼容层使用 */
    VFS_ITEM_MOUNT_POINT,    /* /Volumes 下的挂载点 */
} vfs_item_type_t;

typedef struct {
    vfs_item_id_t  parent_id;      /* 父目录 itemID（卷根为 0） */
    vfs_item_id_t  item_id;        /* 自身 ID */
    vfs_item_type_t type;
    char           name[256];      /* 名称（非路径！） */
    u64            size;
    u64            creation_date;  /* RTC ticks，与内核 RTC 服务对齐 */
    u64            mod_date;
    u32            posix_mode;     /* 仅 /System/usr 兼容层使用 */
    u32            uid;            /* 兼容层使用；策略不在内核 */
    u32            gid;
} vfs_item_info_t;
```

### 3.3 FileHandle（打开的文件实例 = 能力句柄）

```c
typedef u32 vfs_handle_t;   /* 服务端句柄表索引，0 无效 */

/* 打开时的访问权限 —— 与内核 rights_t 位语义对齐 */
#define VFS_ACCESS_READ    (1u << 0)
#define VFS_ACCESS_WRITE   (1u << 1)
#define VFS_ACCESS_EXEC    (1u << 2)
#define VFS_ACCESS_COW     (1u << 3)   /* 写时复制克隆（Phase 3） */

typedef struct {
    vfs_handle_t   handle;
    vfs_resource_t target;
    u32            access;        /* VFS_ACCESS_* 位掩码 */
    u64            offset;        /* 当前读写位置（服务端维护） */
    u32            flags;         /* 打开标志，见 VFS_OPEN_* */
} vfs_handle_info_t;
```

打开标志（对齐常见语义，Phase 0 先实现前三个）：

```c
#define VFS_OPEN_READONLY   0x01
#define VFS_OPEN_CREATE     0x02
#define VFS_OPEN_TRUNCATE   0x04
#define VFS_OPEN_APPEND     0x08
```

### 3.4 Enumerator（目录遍历迭代器）

```c
typedef struct {
    vfs_handle_t    handle;          /* 服务端枚举器句柄 */
    u32             batch_count;     /* 本次返回条目数 */
    /* 批量缓冲区：名称定长数组，避免 4096 IPC 上限问题 */
    char            batch[64][256];  /* 最多 64 个名称/批 */
    vfs_item_id_t   batch_ids[64];
} vfs_enum_batch_t;
```

> 设计决策：单批 ≤64 项，名称定长 256B → 单批最大 ~16KB > 4096，因此
> Phase 0 将 IPC 响应分页（每批 8~16 项，见 §6.4）。`batch[64][256]`
> 仅作为客户端聚合缓冲。

---

## 四、IPC 协议定义（vfs_server ↔ 客户端）

### 4.1 通用请求/响应帧

对齐现有 keyboard 服务风格（`req[0]=op`）：

```c
/* 所有请求的第一个 u32 为操作码 */
enum {
    VFS_OP_GET_ITEM       = 1,   /* 按 URL 查元数据（不打开） */
    VFS_OP_CREATE_DIR     = 2,
    VFS_OP_DELETE_ITEM    = 3,
    VFS_OP_OPEN_ITEM      = 4,   /* 打开，返回 FileHandle */
    VFS_OP_READ           = 5,   /* 句柄 + 偏移 + 长度 */
    VFS_OP_WRITE          = 6,
    VFS_OP_CLOSE          = 7,
    VFS_OP_ENUM_BEGIN     = 8,   /* 目录迭代开始 */
    VFS_OP_ENUM_NEXT      = 9,   /* 取下一批 */
    VFS_OP_CREATE_BOOKMARK = 10,
    VFS_OP_RESOLVE_BOOKMARK = 11,
    VFS_OP_REVOKE_BOOKMARK  = 12,
    VFS_OP_MOUNT           = 13, /* 驱动侧 → vfs_server */
    VFS_OP_UNMOUNT         = 14,
    VFS_OP_STAT_VOLUME     = 15,
};

/* 响应首字段为 i32 ret：0 = 成功，负数 = 错误码 */
/* 错误码沿用内核 ERR_* 约定（负值） */
```

### 4.2 关键消息体

```c
/* VFS_OP_GET_ITEM */
typedef struct {
    u32 op;                 /* = VFS_OP_GET_ITEM */
    char path[1024];        /* URL 字符串，仅此边界出现路径 */
} vfs_req_get_item_t;

typedef struct {
    i32  ret;
    vfs_item_info_t item;
} vfs_resp_get_item_t;

/* VFS_OP_OPEN_ITEM */
typedef struct {
    u32  op;
    char path[1024];        /* 或从书签打开：flags 置 VFS_OPEN_FROM_BOOKMARK */
    u32  flags;
    u32  access;
} vfs_req_open_t;

typedef struct {
    i32  ret;
    vfs_handle_t handle;
    vfs_item_info_t item;
} vfs_resp_open_t;

/* VFS_OP_READ —— 分块读取，单次 ≤ 4096 - 头开销 */
#define VFS_MAX_READ  (MAX_MSG_SIZE - 64)   /* ≈ 4032 字节 */
typedef struct {
    u32  op;
    vfs_handle_t handle;
    u64  offset;            /* 绝对偏移；客户端维护 */
    u32  len;               /* ≤ VFS_MAX_READ */
} vfs_req_read_t;

typedef struct {
    i32  ret;               /* 实际读到的字节数，0 = EOF，负 = 错误 */
    u8   data[VFS_MAX_READ];
} vfs_resp_read_t;

/* VFS_OP_CREATE_BOOKMARK / RESOLVE —— 见 §5 */
```

### 4.3 客户端库 libfs（C 接口）

```c
/* user/lib/libfs/fs.h —— 命名风格对齐 fs_动词_名词 */
int  fs_get_item(const char *url, vfs_item_info_t *out);
int  fs_create_dir(const char *url);
int  fs_delete_item(const char *url, int recursive);
int  fs_open_item(const char *url, u32 flags, u32 access,
                  vfs_handle_t *out_handle);
int  fs_read(vfs_handle_t h, u64 offset, void *buf, u32 len, u32 *got);
int  fs_write(vfs_handle_t h, u64 offset, const void *buf, u32 len);
int  fs_close(vfs_handle_t h);

/* 枚举器 */
int  fs_enum_begin(const char *url, vfs_handle_t *out_enum);
int  fs_enum_next(vfs_handle_t e, vfs_enum_batch_t *batch);
int  fs_enum_end(vfs_handle_t e);

/* 书签 */
int  fs_create_bookmark(const char *url, u32 access,
                        u8 *out_bk, u32 *bk_len);
int  fs_resolve_bookmark(const u8 *bk, u32 bk_len,
                         vfs_handle_t *out_handle, u32 *access);
int  fs_revoke_bookmark(const u8 *bk, u32 bk_len);
```

---

## 五、Security-Scoped Bookmark（安全作用域书签）

### 5.1 生命周期

```
应用请求 /Users/alice/Documents/预算.xlsx
   │
   ▼
vfs_server ──► perm-manager：该 app 有权限吗？
   │                │ 无
   │                ▼
   │         term 弹 Powerbox 询问（文本 UI，Phase 2）
   │         ┌ 允许 ┤
   │         ▼      └ 拒绝 → 返回 -EPERM
   │   perm-manager 记录授权，通知 vfs_server
   ▼
vfs_server 生成 BookmarkData（含卷 UUID + itemID + 权限 + 有效期）
   ▼
应用把 BookmarkData 存进自己的 Library/Preferences/（沙盒内）
   │
   ▼ 下次启动
resolve_bookmark(BookmarkData)
   │  VFS 校验：签名有效？itemID 仍在？权限仍被 perm-manager 允许？
   ├─ 全部通过 → 直接授予临时 FileHandle（无弹窗）
   └─ 文件被移动 → 用 parent_id 链重新定位（书签仍有效）
   └─ 用户已撤销 → -EACCES
```

### 5.2 二进制格式（v1，明文初版 + 签名占位）

```c
#define VFS_BOOKMARK_MAGIC  0x4B4D4256   /* 'VB MK' */
#define VFS_BOOKMARK_VERSION 1

typedef struct {
    u32  magic;             /* VFS_BOOKMARK_MAGIC */
    u32  version;           /* = 1 */
    u32  payload_len;       /* 以下 payload 长度 */
    u32  reserved;
    /* --- payload（版本 1） --- */
    vfs_resource_t resource;   /* 卷 UUID + itemID（稳定定位） */
    vfs_item_id_t  parent_id;  /* 创建时的父 ID，用于移动追踪 */
    u32  access;               /* 授予的 VFS_ACCESS_* */
    u32  app_id_hash;          /* 沙盒 app id 的 32 位哈希 */
    u64  created_ticks;        /* RTC 时间戳 */
    u64  expiry_ticks;         /* 0 = 永不过期 */
    /* --- 签名区（Phase 3：引入密码学后启用） --- */
    u8   mac[16];              /* 当前填 0，占位 */
} vfs_bookmark_t;
```

**阶段化说明**：当前无密码学子系统，Phase 2 的书签有效性以
「vfs_server 服务端记录表 + 随机 token」为准，`BookmarkData` 仅为
服务端表索引的封装（应用拿到的是一段不透明 blob，路径字符串不出服务端）。
`mac[16]` 字段预留在 Phase 3 用 HMAC-SHA256（vfs_server 主密钥）真正签名。

### 5.3 权限检查的单一事实源

- **服务端强制**：vfs_server 每次 `open_item`/`resolve_bookmark` 都向
  perm-manager 校验当前授权（IPC 查询，缓存 1s 以内）。
- **撤回即失效**：用户在设置中撤销 → perm-manager 通知 vfs_server 删除
  该 app 的全部书签记录 → 应用下次 resolve 得到 `-EACCES`。
- **不信任路径字符串**：书签解析结果只返回句柄，不返回路径，应用侧
  无法据此做路径遍历。

---

## 六、路径解析与目录枚举

### 6.1 解析模型（Phase 0 简化版）

```
resolve(url):
  "VolumeName/a/b/c" → 卷表查 VolumeName → 根 itemID → 逐级 parent_id 查子项
  每级：名称哈希（FNV-1a 32）→ 驱动返回候选列表 → 精确匹配 name
```

Phase 0 不做全局 B-Tree 索引：驱动内部维护「父 ID → 子表」即可，层级
浅（≤6 层），每层一次 IPC，足够。**B-Tree 全局索引列为 Phase 3 优化**
（对齐 APFS 命名空间设计，但不阻塞主链路）。

### 6.2 挂载配置（修正概念稿的自相矛盾）

不用 `/etc/fstab` 文本文件。挂载配置是 vfs_server 自己的持久化对象：

```c
typedef struct {
    vfs_uuid_t     vol;           /* 卷 UUID */
    char           mount_name[64];/* 挂载名，如 "System" "SSD-Data" */
    char           driver[64];    /* 驱动名，如 "fs.mem" "fs.virtio_blk" */
    bool           read_only;
    bool           auto_mount;    /* 启动时自动挂载 */
} vfs_mount_config_t;
```

配置持久化在**系统卷**（Phase 0 为只读内存卷内嵌的静态表；Phase 1 起
存于系统卷自身）。

### 6.3 目录树（多卷视图，Phase 0 即可呈现）

```
/Volumes/System      ← fs_mem_driver（blob 只读卷，内容见下）
/Volumes/Users       ← fs_mem_driver（RAM 可写卷，重启丢失）
/Volumes/SSD-Data    ← Phase 1 真实磁盘
/.Trash/<uid>/       ← 用户隔离（Phase 2）
```

> 概念稿的 `/System /Users` 顶层目录由 vfs_server 的「逻辑视图」层实现：
> 挂载名 System/Users 直接映射到卷根。这样**不用真正的 bind mount**，
> 一个逻辑视图表即可，省去内核改动。

### 6.4 枚举分页（4096 上限对策）

- 单次 `VFS_OP_ENUM_NEXT` 响应：`{ ret, count(≤16), items[16] }`，
  每项定长 `name[256] + id` → 单响应 ≤ 16×264 + 头 ≈ 4.2KB。若超限，
  服务端自动降为 8 项/批。
- 客户端 `fs_enum_next` 循环聚合到用户提供的 `vfs_enum_batch_t`。

---

## 七、FSKit-lite 驱动协议（用户态文件系统驱动）

### 7.1 驱动进程职责

```
fs_<format>_driver（用户态进程，如 fs_mem_driver）
  ├─ 持有块设备能力（Phase 1：内核授予 CAP_IO_PORT/CAP_PCI_DEV/CAP_MEM）
  ├─ 维护卷内部结构：itemID ↔ (block list / 内存页)
  └─ 实现驱动协议（挂载后由 vfs_server 调用）：
        getattr(item_id)         → vfs_item_info_t
        lookup(parent_id, name)  → item_id
        read(item_id, off, len)  → 字节
        write(item_id, off, buf) → 字节数
        create_dir / delete / mkfile
        enumerate(parent_id, from) → 一批 (id, name)
```

### 7.2 挂载握手（`VFS_OP_MOUNT`）

```
vfs_server（启动时，或检测到热插拔）：
  1. spawn_service("fs_mem_driver")            // 复用 manager 的 blob 拉起
  2. 授权：通过 IPC 传递块设备能力 / 内存卷能力
  3. 驱动初始化格式（Phase 0：内存卷直接布局；Phase 1：读分区表）
  4. 驱动 → vfs_server: VFS_OP_MOUNT { uuid, root_item_id, driver_name }
  5. vfs_server 登记卷，绑定 mount_name，广播变更（notify）
```

### 7.3 Phase 0 的内存卷布局（无需驱动进程也可内嵌进 vfs_server）

Phase 0 允许两个实现二选一：
- **A（推荐，先跑通协议）**：`fs_mem_driver` 作为独立进程，从内核
  `SYS_BLOB_GET` 取 blob 构建只读卷（文件 = blob 名 → 内容），RAM 卷
  用 vfs_server 内部 `malloc` 的页表构建。
- **B（最快验证）**：内存卷逻辑先内嵌 vfs_server（`vfs_mem_vol.c`），
  驱动协议框架留空接口。选择 A 更符合微内核一致性，开发量只多一个进程。

**建议：Phase 0 选 A** —— 驱动协议是 FSKit 的灵魂，越早真跑越好。

---

## 八、分阶段实施计划（每阶段可运行、可验证）

### Phase 0 —— 对象模型 + 内存卷（不依赖磁盘）

**交付物**：
1. `user/services/vfs/vfs_server.c`：卷表、Item 表（内存卷）、句柄表、
   `VFS_OP_*` 1~9（GET/CREATE/DELETE/OPEN/READ/WRITE/CLOSE/ENUM）。
2. `user/services/vfs/fs_mem_driver.c`：blob 只读卷 + RAM 可写卷。
3. `user/lib/libfs/fs.c` + `fs.h`：§4.3 全部接口。
4. manager 服务表注册 `vfs`（manager.c:328 模式）；shell 增加测试命令
   `vfs_ls <url>`、`vfs_cat <url>`（shell.c 注册命令表）。
5. 系统卷预置内容：把现有 blob（hello.elf 等）映射为
   `/Volumes/System/Kernel/hello.elf`。

**验证**（QEMU，沿用串口 + screendump + 解码脚本流程）：
- 串口：`vfs_ls /Volumes/System/` 列出 blob 名。
- `vfs_cat /Volumes/System/Kernel/hello.elf` 输出字节数与 blob 大小一致。
- 写卷：`fs_open_item(..., VFS_OPEN_CREATE)` 建文件 → 写 → 读回一致。
- 权限：以 WRITE 打开只读卷 → `-EROFS`。

**验收记录（2026-08-07，QEMU 实测）✅**：
- `vfs_ls /Volumes/System/` → `Kernel`（1 条目）；`vfs_ls /Volumes/System/Kernel/`
  列出全部 blob。
- `vfs_cat /Volumes/System/Kernel/hello.elf` → `== read 35032 bytes ==`，
  与 build/user/services/hello.elf 实际大小 35032 字节一致。
- `vfs_write /Volumes/System/test.txt hello` → `open FAILED (-100)`，
  VFS_ERR_READONLY 即 -EROFS，只读卷写路径正确拒绝。
- 此前已验：`vfs_fill /Volumes/Users/` 触发 ENOSPC（-101）；`vfs_stat` 显示
  System 只读 / Users 32768 KB（32 MiB）；shell 事后可继续交互（无崩溃）。

### Phase 1 —— 真实存储：virtio-blk + 简单磁盘格式

**交付物**：
1. `kernel/arch/x86_64/virtio_blk.c`：PCI 枚举（cap 已有
   `CAP_TYPE_PCI_DEV/IO_PORT` 脚手架）+ virtio-blk 驱动，暴露给
   `fs_virtio_blk_driver`（用户态，内核只提供 DMA 能力）。
2. `fs_virtio_blk_driver`：块读写 + 简单卷格式（自研 TFS 风格：
   超级块 + inode 区 + 数据区，itemID 即 inode 号）。
3. `VFS_OP_MOUNT/UNMOUNT/STAT_VOLUME` 实装；挂载配置对象持久化。
4. QEMU 启动参数加 `-drive file=disk.img,if=virtio`。

**验证**：`vfs_cat` 读磁盘卷；写后重启 QEMU 数据仍在（持久性证明）；
`vfs_stat_volume` 显示空间占用。

**验收记录（2026-08-14，QEMU 实测，sendkey 注入 + screendump OCR 解码）✅**：
- `vfs_write /Volumes/Disk/hello.txt hello-disk-123` 首次 → `open FAILED (-105)`
  = VFS_ERR_ACCESS；term 弹 Powerbox 文本询问：`perm: app 0x5e11e5 requests
  /Volumes/Disk/hello.txt (W) - perm_answer 513 y/n` —— shell（Standard 角色）
  的 WRITE 无链规则，按默认拒绝走 Powerbox 授权流（§九 与 shell.c 注释一致）。
- `perm_answer 513 y` → `[ALLOWED] ... query 513 -> ALLOWED (0)`（grant 写入）。
- 重试 `vfs_write` → `14 bytes written to /Volumes/Disk/hello.txt`（授权后放行）。
- host 侧 `xxd disk.img` → 偏移 0x8200 见 `hello-disk-123` 数据块（真实落盘）。
- **冷重启 QEMU**（保留 disk.img）→ `vfs_cat /Volumes/Disk/hello.txt` →
  `== read 14 bytes ==` + `hello-disk-123` —— **持久性证明成立**。
- `vfs_stat /Volumes/Disk` → `8159 KB total, 0 KB used, read-write`
  （14 字节不足 1 KB 取整为 0 used，正确）。
- 回归：31/31 passed，0 PANIC/FATAL；Disk 卷跨重启同 UUID
  （6f707379-732d7666-1be-564245f5）未重格式化。

> **注（shell 写磁盘授权机制）**：P0 种子规则曾给 Standard 角色配
> `DATA_DOCS_WRITE=DENY` 链规则，链 DENY 短路使 Powerbox 永不触发，
> shell(STANDARD) 写 Disk 被静默拒绝，与 §九 验收流（-105 + 弹窗 →
> perm_answer → 重试成功）自相矛盾。已移除该链规则（perm-manager.c
> seed_rules），Standard 的 WRITE 走默认拒绝 → Powerbox 用户授权流，
> 与 docs/permission_model.md §2.6「Powerbox 是唯一授权入口」一致；
> READ 仍链 ALLOW 直通（vfs_cat/vfs_stat 无需授权）。所有 P1 测试
> 均基于 OWNER/GUEST 角色（chain DENY 语义未变），31/31 回归通过。

### Phase 2 —— 书签 + 权限管理器 + Powerbox（文本版）

**交付物**：
1. `user/services/perm/perm-manager`：授权记录表 + 撤销。
2. `vfs_server` 书签表 + `VFS_OP_CREATE/RESOLVE/REVOKE_BOOKMARK` 实装
   （§5.2 格式，MAC 占位 0）。
3. Powerbox：perm-manager → term 文本询问（"允许 app 访问 X？[y/n]"），
   shell 或 term 上应答。
4. `.Trash/<uid>/` 隔离 + `com.apple.quarantine` 风格 xattr 位。

**验证**：
- 授权前 resolve → `-EACCES`；授权后 resolve → 拿到句柄。
- 移动文件后 resolve 书签 → 仍有效（parent_id 追踪）。
- 撤销后 resolve → `-EACCES`。

**验收记录（2026-08-08，QEMU 实测，sendkey 注入 + 串口镜像日志）✅**：
- `vfs_write /Users/a.txt hello` → `5 bytes written`（测试文件就绪）。
- `bm_create /Users/a.txt r`（未授权）→ `FAILED (-105)` = VFS_ERR_ACCESS
  即 -EACCES，默认拒绝生效。
- term 弹 Powerbox 文本询问：`perm: app 0x5e11e5 requests /Users/a.txt (R)
  - perm_answer 389 y/n`；`perm_answer 389 y` → `ALLOWED (0)`。
- 再 `bm_create` → `ok, 88-byte bookmark cached`（授权后放行）。
- `bm_resolve` → `handle -1380597971, item 'a.txt' (id 2), access 1`。
- `move /Users/a.txt /Users b.txt` → `move: 'b.txt' -> item 2`（改名成功，
  itemID 稳定）。
- 移动后再 `bm_resolve` → `handle -1603253150, item 'b.txt' (id 2),
  access 1` —— **书签跨移动仍有效**（父级 ID 追踪，§5 地基验证通过）。
- `perm_revoke` → `1 grant(s) dropped`；再 `bm_resolve` → `FAILED (-105)`
  —— 撤销即时生效，-EACCES 复现。

> 注：验收期间发现并修复 do_move 用户栈溢出（#PF，error=0x6）——用户线程
> 栈仅单页 4 KB（`thread_create_user`），而 do_move 在栈上放两个
> `segs[8][256]`（4 KB）超限；已按 `s_req`/`s_resp` 模式提升为文件级
> static 共享缓冲（vfs_server 单线程，安全）。修复后帧 0x1218 → 0x198。
> 验收脚本归档于 `scripts/accept.py`（QEMU sendkey 注入 + 串口镜像日志，
> 用于回归，见 kernel_roadmap.md §5.2）。

### Phase 3 —— 零拷贝 + 快照 + CoW（远期）

- 共享内存读路径（`CAP_TYPE_MEM` 映射文件页，绕开 4096 上限）。
- 卷快照（Phase 1 卷格式预留快照位图）；系统更新回滚。
- 书签 MAC 真签名（引入 HMAC-SHA256 库）。
- 全局 B-Tree 命名空间索引（可选，性能优化非必需）。
- CoW 克隆：`VFS_ACCESS_COW` 位生效。

---

## 九、安全模型要点

1. **路径不出服务端**：客户端拿到的是句柄与书签；即使应用被攻破，
   无法构造路径遍历（Android file:// 漏洞类问题从根上消除）。
2. **无全局根**：vfs_server 只持有所需能力；磁盘驱动持块设备能力；
   内核不解释 VFS 语义（对齐 requirements.md §5.3「凭据检查在用户态」）。
3. **默认拒绝**：应用无书签即无访问；Powerbox 是唯一授权入口。
4. **撤回即时生效**：权限单一事实源在 perm-manager，vfs_server 每次
   open/resolve 校验。
5. **POSIX 兼容层收敛在 /System/usr**：仅 CLI 工具经符号链接 + 权限
   映射访问；用户数据层强制 URL + 书签。

---

## 十、已定案决策（原开放问题 1~3，2026-08-06 拍板）

### 决策 1：FileHandle 完全由 vfs_server 用户态管理（内核零改动）✅ 已定案

**结论**：句柄 = vfs_server 内部句柄表中的条目，配随机 token；内核不加
`CAP_TYPE_FILE`。

**理由**：
1. `requirements.md §4.3` 明确「用户态能力（扩展）：由服务自己定义，内核
   不解释」——FileHandle 正是此类。
2. 内核 `cap.h` 的 `CAP_TYPE_*` 全部对应**内核对象**（PORT/MEM/IRQ/
   IO_PORT/PCI_DEV）；FileHandle 是 VFS 语义对象，内核不理解也无权解释。
3. 权限强制点本就在 vfs_server（每次 open/resolve 校验 perm-manager），
   内核背书不增加安全收益，只增加内核复杂度和攻面。
4. 零内核风险，Phase 0 即可交付。

**风险与对策**：
- vfs_server 崩溃 → 句柄全失效 → 应用收到 `-ESTALE`，重开（见决策 4）。
- 句柄伪造 → 句柄值含 32 位随机 token（内核 `SYS_GET_RTC_TIME`/RNG 种子），
  且句柄仅经 IPC 传递，不落磁盘。

### 决策 2：Powerbox 用 term 文本询问，UI 后端接口解耦 ✅ 已定案

**结论**：perm-manager 定义统一的「授权询问接口」，Phase 2 的 UI 后端 =
term 文本询问；v0.4 图形服务落地后只换后端，接口不变。

**理由**：term 是当前唯一用户可见终端；文本询问（`允许 xxx 访问
/Users/alice/Documents/预算.xlsx ？[y/n]`）无需任何内核或图形改动。

**接口抽象**：

```c
/* perm-manager 向外暴露的询问接口（供 UI 代理调用） */
#define PERM_OP_QUERY       1   /* UI 代理 → perm-manager：取待决询问 */
#define PERM_OP_ANSWER      2   /* UI 代理 → perm-manager：用户应答 */
/* UI 代理注册：term 启动时向 perm-manager 注册 port 名为 "perm.ui" */
```

### 决策 3：Phase 0 RAM 卷容量 = 32MB（实测支撑）✅ 已定案

**实测数据**（`qemu-serial-pre-fix.log`，`-m 256M`）：

| 指标 | 数值 |
|------|------|
| PMM 追踪总物理内存 | 0xffe0 pages = 256MB |
| 启动后空闲 | 62263 pages = **243MB** |
| 内核 + 全部服务占用 | ≈ 13MB |

**结论**：RAM 可写卷默认 32MB（`#define VFS_RAM_VOL_SIZE_MB 32`），通过
用户态 `map_memory()`（`SYS_MAP_MEMORY`，已存在）按需分配物理页。32MB
仅占空闲内存的 13%，余量充足；blob 只读卷（System）直接引用 `SYS_BLOB_GET`
取回的 blob 缓冲区，不额外占 RAM 卷。

**验证方法**（Phase 0 验收）：`vfs_stat_volume` 显示 RAM 卷容量 = 32MB；
连续写入 >32MB 应返回 `-ENOSPC` 且系统其余功能正常（内存水位无异常）。

---

## 十之一、开放问题（剩余待拍板）

1. **vfs_server 崩溃恢复**：Phase 0 不做持久化恢复（内存卷无状态）；
   Phase 1 磁盘卷引入后，句柄表失效策略需定义（应用收到 -ESTALE 后
   重开）。——暂缓，Phase 1 编码前再定。

---

## 十一、代码库改动清单（按 Phase）

| Phase | 新增/修改 | 位置 |
|-------|-----------|------|
| 0 | vfs_server / fs_mem_driver / libfs | `user/services/vfs/`、`user/lib/libfs/` |
| 0 | manager 服务表注册 vfs | `user/services/manager/manager.c` |
| 0 | shell 测试命令 vfs_ls / vfs_cat | `user/services/shell/shell.c` |
| 0 | Makefile 服务列表扩展 | `Makefile`（SVC_NAMES 加 vfs） |
| 1 | virtio-blk 驱动 | `kernel/arch/x86_64/virtio_blk.c` + `kernel/include/kernel/` |
| 1 | fs_virtio_blk_driver + 卷格式 | `user/services/vfs/` |
| 2 | perm-manager | `user/services/perm/` |
| 3 | 共享内存 / 快照 / 签名 | 内核 + vfs_server |

---

## 十二、与 requirements.md 的对齐

- requirements.md §4.3「用户态能力（扩展）：由服务自己定义，内核不解释」
  → FileHandle 句柄即此模式的应用。
- requirements.md §1.2 服务表「文件系统服务 CAP_BLOCK_IO」→ Phase 1 的
  驱动进程对应；vfs_server 本身不需要块能力。
- requirements.md §9「文档要求：ipc_protocols.md」→ 本文 §4 即为
  vfs 协议章节，可拆分归档。
