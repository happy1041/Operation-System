# Lab 2 代码修改详解：从 Lab 1 → Lab 2

本文档按逻辑顺序梳理了从 Lab 1 基线版本到 Lab 2 完成版本的所有代码修改。

---

## 目录

1. [总览：修改了哪些文件](#1-总览)
2. [thread.h — 给线程加上"进程"能力](#2-threadh)
3. [thread.c — 初始化新增字段](#3-threadc)
4. [process.h — 定义父子共享控制块](#4-processh)
5. [process.c — 进程生命周期全面重写](#5-processc)
   - 5.1 新增头文件
   - 5.2 `process_execute()` — 创建子进程
   - 5.3 `start_process()` — 子进程启动
   - 5.4 `process_wait()` — 等待子进程
   - 5.5 `process_exit()` — 进程退出清理
   - 5.6 `load()` — ELF 加载器改造
   - 5.7 `setup_stack()` — 参数传递
6. [syscall.c — 从空壳到完整系统调用](#6-syscallc)
7. [exception.c — 异常处理补丁](#7-exceptionc)
8. [各修改之间的依赖关系图](#8-依赖关系)

---

## 1. 总览

| 文件 | 修改类型 | 说明 |
|---|---|---|
| `threads/thread.h` | 新增字段 | 在 `struct thread` 中添加 6 个 USERPROG 字段 + 前向声明 |
| `threads/thread.c` | 新增初始化 | `init_thread()` 中初始化上述字段 |
| `userprog/process.h` | 新增结构体 | 定义 `struct child_info`，父子通信的核心 |
| `userprog/process.c` | **大幅重写** | 进程创建/等待/退出/加载/参数传递全部重写 |
| `userprog/syscall.c` | **完全重写** | 从 2 行 `printf+thread_exit` 变成 ~310 行完整 syscall |
| `userprog/exception.c` | 加 1 行 | 异常终止时设 `exit_status = -1` |

---

## 2. thread.h — 给线程加上"进程"能力

### 2.1 新增前向声明（文件顶部）

```c
// ——— Lab 1（无此行） ———

// ——— Lab 2 ———
/* Forward declaration to avoid circular include with process.h. */
struct child_info;
```

**为什么需要**：`struct thread` 要包含 `struct child_info *` 指针，但 `process.h` 又 include 了 `thread.h`，直接互相 include 会编译错误。用前向声明打破循环依赖。

### 2.2 `struct thread` 的 `#ifdef USERPROG` 块

```c
// ——— Lab 1 ———
#ifdef USERPROG
   uint32_t *pagedir;
#endif

// ——— Lab 2 ———
#ifdef USERPROG
   uint32_t *pagedir;             /**< Page directory. */
   int exit_status;               /**< 退出状态码（默认 0）。 */
   struct list children;          /**< 子进程的 child_info 链表。 */
   struct child_info *child_info; /**< 指向自己在父进程 children 链表中的 child_info。 */
   struct file *exec_file;        /**< 正在运行的可执行文件句柄（持有 deny-write）。 */
   struct file *fd_table[128];    /**< 文件描述符表；fd 0/1 保留给 stdin/stdout。 */
   int fd_next;                   /**< 下一个可分配的 fd 编号，从 2 开始。 */
#endif
```

**逐字段解释**：

| 字段 | 类型 | 用途 |
|---|---|---|
| `exit_status` | `int` | 进程退出码。`SYS_EXIT` 设置此值，`process_exit()` 打印它，父进程通过 `process_wait()` 读取它。 |
| `children` | `struct list` | 当前线程创建的所有子进程的 `child_info` 链表。用于 `process_wait()` 查找子进程。 |
| `child_info` | `struct child_info *` | 指向当前线程自己对应的 `child_info`（存放在**父进程**的 `children` 链表里）。退出时通过它通知父进程。 |
| `exec_file` | `struct file *` | 持有对自身可执行文件的打开句柄，并通过 `file_deny_write()` 禁止写入。防止运行时被覆盖。 |
| `fd_table[128]` | `struct file *[]` | 文件描述符表。索引就是 fd 编号。`fd_table[3]` 就是 fd=3 对应的 `struct file *`。NULL 表示未使用。 |
| `fd_next` | `int` | 下一个要分配的 fd。从 2 开始（0=stdin, 1=stdout 保留），单调递增，不回收。 |

---

## 3. thread.c — 初始化新增字段

### `init_thread()` 函数末尾

```c
// ——— Lab 1（无此段） ———

// ——— Lab 2（在 `t->waiting_lock = NULL;` 之后添加） ———
#ifdef USERPROG
  list_init(&t->children);        // 子进程列表初始为空
  t->child_info = NULL;           // 还没有父进程为它填充 child_info
  t->exit_status = 0;             // 默认退出码：0（正常）
  t->exec_file = NULL;            // 还没有加载可执行文件
  memset(t->fd_table, 0, sizeof(t->fd_table));  // 所有 fd 槽清零（NULL）
  t->fd_next = 2;                 // fd 0/1 保留，用户 fd 从 2 分配
#endif
```

**关键点**：`exit_status = 0` 意味着如果进程没有显式调用 `exit(N)`，默认退出码是 0。但如果被异常杀死（`exception.c`），会被改为 -1。

---

## 4. process.h — 定义父子共享控制块

### Lab 1 原版

```c
#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H
#include "threads/thread.h"

tid_t process_execute(const char *file_name);
int process_wait(tid_t);
void process_exit(void);
void process_activate(void);

#endif
```

### Lab 2 新增

```c
#include "threads/synch.h"   // ← 新增 include，因为要用 semaphore

// 在函数声明之后，新增整个结构体定义：
struct child_info {
    tid_t tid;                  // 子进程的线程 ID
    int exit_status;            // 子进程的退出码（由子进程在退出时写入）
    bool exited;                // 子进程是否已经退出
    bool waited;                // 父进程是否已经 wait 过此子进程
    struct semaphore sema;      // 父进程 wait 时阻塞在这里（初始值 0）
    struct semaphore load_sema; // 父进程等子进程加载完 ELF（初始值 0）
    bool load_success;          // 子进程 ELF 加载是否成功
    int ref_count;              // 引用计数：初始 2（父+子各一个引用）
    struct list_elem elem;      // 链表节点，挂在父进程的 children 链表中
};
```

**设计思路**：

- **为什么需要 `child_info` 而不是直接在 `struct thread` 里存退出状态？**
  因为子进程可能先于父进程退出，此时子进程的 `struct thread` 会被回收（`palloc_free_page`），父进程就读不到退出状态了。所以需要一个独立的堆上结构，生命周期由引用计数管理。

- **`ref_count` 的工作方式**：
  - 创建时 = 2（父持有一个引用，子持有一个引用）
  - 任意一方退出/完成 wait → `ref_count--`
  - 最后一个离开的那方发现 `ref_count == 0`，负责 `free(ci)`

- **两个信号量的分工**：
  - `load_sema`：`process_execute()` 中父进程 `sema_down` 等待，`start_process()` 中子进程加载完毕后 `sema_up`
  - `sema`：`process_wait()` 中父进程 `sema_down` 等待，`process_exit()` 中子进程退出时 `sema_up`

---

## 5. process.c — 进程生命周期全面重写

### 5.1 新增头文件

```c
// Lab 1 → Lab 2 新增的 #include：
#include "threads/synch.h"    // semaphore, lock
#include "threads/malloc.h"   // malloc/free（用于分配 child_info）
```

---

### 5.2 `process_execute()` — 创建子进程

**Lab 1 原版**（简化版，只创建线程）：
```c
tid_t process_execute(const char *file_name) {
  fn_copy = palloc_get_page(0);
  strlcpy(fn_copy, file_name, PGSIZE);
  // 直接把整个 "echo hello world" 当作线程名
  tid = thread_create(file_name, PRI_DEFAULT, start_process, fn_copy);
  if (tid == TID_ERROR) palloc_free_page(fn_copy);
  return tid;
}
```

**Lab 2 改动**（6 处关键变化）：

**① 从命令行中提取可执行文件名**
```c
char name_copy[16];
strlcpy(name_copy, file_name, sizeof name_copy);
char *exe_name = strtok_r(name_copy, " ", &save_ptr);
```
原来 `thread_create(file_name, ...)` 会把 `"echo hello world"` 整个当线程名。现在先用 `strtok_r` 提取出 `"echo"`，只用程序名作为线程名。这是 Lab 2 测试的要求——退出信息只打印程序名，不带参数。

**② 分配 `child_info` 并初始化**
```c
struct child_info *ci = malloc(sizeof(struct child_info));
ci->exit_status = -1;      // 默认 -1，如果子进程正常退出会被覆盖
ci->exited = false;
ci->waited = false;
sema_init(&ci->sema, 0);
sema_init(&ci->load_sema, 0);
ci->load_success = false;
ci->ref_count = 2;          // 父+子各持有一个引用
```

**③ 通过页面尾部传递 `ci` 指针**
```c
*(struct child_info **)(fn_copy + PGSIZE - sizeof(struct child_info *)) = ci;
```
`fn_copy` 是一整页（4096 字节），命令行字符串不会占满它。将 `ci` 指针写入页面最后 4 字节，子线程可以从同一页面取回这个指针。这避免了额外的全局变量或 malloc。

**④ 创建线程时用 `exe_name` 而非 `file_name`**
```c
tid = thread_create(exe_name, PRI_DEFAULT, start_process, fn_copy);
```

**⑤ 将 `ci` 加入父进程的 `children` 链表**
```c
ci->tid = tid;
list_push_back(&thread_current()->children, &ci->elem);
```

**⑥ 等待子进程加载完成**
```c
sema_down(&ci->load_sema);     // 阻塞，直到子进程调用 sema_up
if (!ci->load_success) {
    // 加载失败，清理 ci
    list_remove(&ci->elem);
    ci->ref_count--;
    if (ci->ref_count == 0) free(ci);
    return TID_ERROR;            // 返回 -1
}
return tid;
```
Lab 1 的 `process_execute()` 是"发射后不管"，Lab 2 必须等 `load()` 成功后才返回 tid。

---

### 5.3 `start_process()` — 子进程启动

**Lab 1 原版**：
```c
static void start_process(void *file_name_) {
  char *file_name = file_name_;
  // 初始化中断帧...
  success = load(file_name, &if_.eip, &if_.esp);
  palloc_free_page(file_name);
  if (!success) thread_exit();
  // asm 跳到用户态
}
```

**Lab 2 改动**（2 处）：

**① 取回 `child_info` 指针并关联到当前线程**
```c
struct child_info *ci = *(struct child_info **)(file_name + PGSIZE - sizeof(struct child_info *));
thread_current()->child_info = ci;
```
`child_info` 的地址通过页面尾部 4 字节传递过来，写入 `thread_current()->child_info`。

**② 加载完成后通知父进程**
```c
ci->load_success = success;
sema_up(&ci->load_sema);   // 唤醒 process_execute() 中的 sema_down
```
注意这两行在 `palloc_free_page(file_name)` **之前**（因为 `sema_up` 后父进程可能修改 ci，但 file_name 页面已经不需要了）。

---

### 5.4 `process_wait()` — 等待子进程

**Lab 1 原版**：
```c
int process_wait(tid_t child_tid UNUSED) {
  return -1;   // 空实现
}
```

**Lab 2 完整实现**：
```c
int process_wait(tid_t child_tid) {
  struct thread *cur = thread_current();

  // 1. 在 children 链表中查找匹配 child_tid 的 child_info
  struct child_info *ci = NULL;
  for (e = list_begin(&cur->children); ...; e = list_next(e)) {
    struct child_info *c = list_entry(e, struct child_info, elem);
    if (c->tid == child_tid) { ci = c; break; }
  }

  // 2. 找不到 或 已经 wait 过 → 返回 -1
  if (ci == NULL || ci->waited) return -1;

  // 3. 标记为已 wait，然后阻塞等子进程退出
  ci->waited = true;
  sema_down(&ci->sema);     // 子进程 process_exit() 中 sema_up 后解除阻塞
  int status = ci->exit_status;

  // 4. 清理
  list_remove(&ci->elem);
  ci->ref_count--;
  if (ci->ref_count == 0) free(ci);

  return status;
}
```

**核心逻辑**：
- 链表搜索 O(n)，但进程不会有太多子进程
- `waited = true` 防止同一子进程被 wait 两次
- 如果子进程已经退出，`sema` 值已经是 1，`sema_down` 立即返回，不阻塞

---

### 5.5 `process_exit()` — 进程退出清理

**Lab 1 原版**（非常简单）：
```c
void process_exit(void) {
  struct thread *cur = thread_current();
  uint32_t *pd = cur->pagedir;
  if (pd != NULL) {
    cur->pagedir = NULL;
    pagedir_activate(NULL);
    pagedir_destroy(pd);
  }
}
```

**Lab 2 改动**（在 `pd != NULL` 之前 + 之中都有改动）：

**① 通知父进程（在 `pd` 判断之前）**
```c
struct child_info *ci = cur->child_info;
if (ci != NULL) {
    ci->exit_status = cur->exit_status;  // 将退出码写入共享结构
    ci->exited = true;
    sema_up(&ci->sema);                  // 唤醒 process_wait() 中的父进程
    ci->ref_count--;                     // 子进程放弃引用
    if (ci->ref_count == 0) free(ci);    // 如果父进程已经退出，由子进程清理
}
```

**② 释放对所有子进程 `child_info` 的引用（在 `pd` 判断之前）**
```c
struct list_elem *e = list_begin(&cur->children);
while (e != list_end(&cur->children)) {
    struct child_info *c = list_entry(e, struct child_info, elem);
    e = list_next(e);        // 必须先保存 next，因为下面要 list_remove
    list_remove(&c->elem);
    c->ref_count--;
    if (c->ref_count == 0) free(c);
}
```
这保证了父进程退出时清理所有 `child_info`，不泄漏。

**③ 关闭所有打开的文件描述符（在 `if (pd != NULL)` 内、pagedir 销毁之前）**
```c
for (int i = 2; i < 128; i++) {
    if (cur->fd_table[i] != NULL) {
        file_close(cur->fd_table[i]);
        cur->fd_table[i] = NULL;
    }
}
```

**④ 释放可执行文件句柄 + 解除写保护（Task 5）**
```c
bool was_user_process = (cur->exec_file != NULL);
if (cur->exec_file != NULL) {
    file_allow_write(cur->exec_file);   // 解除 deny-write
    file_close(cur->exec_file);
    cur->exec_file = NULL;
}
```

**⑤ 打印退出信息**
```c
if (was_user_process)
    printf("%s: exit(%d)\n", cur->name, cur->exit_status);
```
只有真正运行过用户代码的进程才打印。用 `exec_file != NULL` 作为判断标志（内核线程没有 exec_file）。

---

### 5.6 `load()` — ELF 加载器改造

**Lab 1 原版函数签名**：
```c
bool load(const char *file_name, void (**eip)(void), void **esp)
```
直接用整个 `file_name`（如 `"echo hello world"`）作为文件名打开。

**Lab 2 改动**（3 处）：

**① 从命令行中提取程序名**
```c
char cmd_copy[256];
strlcpy(cmd_copy, file_name, sizeof cmd_copy);
char *exe_name = strtok_r(cmd_copy, " ", &save_ptr);
```
然后用 `exe_name`（不含参数）调用 `filesys_open(exe_name)`。

**② `setup_stack()` 调用改为传入完整命令行**
```c
// Lab 1:  if (!setup_stack(esp))
// Lab 2:  if (!setup_stack(esp, file_name))
```
`setup_stack` 签名从 `(void **esp)` 变为 `(void **esp, const char *cmd_line)`，因为需要解析参数。

**③ `done:` 标签处：deny-write + 保留文件句柄（Task 5）**
```c
// Lab 1:
//   done:
//     file_close(file);   // 无条件关闭
//     return success;

// Lab 2:
done:
  if (success) {
      file_deny_write(file);              // 禁止写入
      thread_current()->exec_file = file;  // 保留句柄
  } else {
      file_close(file);                   // 加载失败才关闭
  }
  return success;
```
Lab 1 无论成功失败都 `file_close`。Lab 2 成功时**不关闭**，而是保留句柄并调用 `file_deny_write()`，直到进程退出时才 `file_allow_write()` + `file_close()`。

---

### 5.7 `setup_stack()` — 参数传递

**Lab 1 原版**：
```c
static bool setup_stack(void **esp) {
  uint8_t *kpage = palloc_get_page(PAL_USER | PAL_ZERO);
  if (kpage != NULL) {
    success = install_page(((uint8_t *)PHYS_BASE) - PGSIZE, kpage, true);
    if (success) *esp = PHYS_BASE;
    // ...
  }
  return success;
}
```
只分配栈页，把 `esp` 设为 `PHYS_BASE`，不做任何参数处理。

**Lab 2 完整实现**（大幅扩展）：

```
函数签名改为：setup_stack(void **esp, const char *cmd_line)
```

在分配栈页之后，增加了完整的 C 调用约定参数压栈：

```
步骤 1: 将命令行字符串复制到临时页面
步骤 2: 用 strtok_r 拆分为 argv[0], argv[1], ..., argv[argc-1]
步骤 3: 从右到左将字符串内容压入栈（高地址 → 低地址）
步骤 4: 对齐 esp 到 4 字节边界
步骤 5: 压入 NULL 哨兵（argv[argc]）
步骤 6: 从右到左压入各字符串的地址（指向步骤 3 中的位置）
步骤 7: 压入 argv 指针（指向步骤 6 的第一个地址）
步骤 8: 压入 argc
步骤 9: 压入假返回地址（0x00000000）
```

**最终栈布局示例**（`"echo hello world"`）：
```
PHYS_BASE ──────────────────────────
    "world\0"        ← argv[2] 字符串
    "hello\0"        ← argv[1] 字符串
    "echo\0"         ← argv[0] 字符串
    [padding]        ← 对齐填充
    0x00000000       ← argv[3] = NULL 哨兵
    &argv[2]         ← 指向 "world" 的地址
    &argv[1]         ← 指向 "hello" 的地址
    &argv[0]         ← 指向 "echo" 的地址
    &argv[0]的地址   ← argv 本身
    3                ← argc
    0x00000000       ← 假返回地址
esp ────────────────────────────────
```

---

## 6. syscall.c — 从空壳到完整系统调用

### Lab 1 原版（共 22 行）

```c
#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"

static void syscall_handler(struct intr_frame *);

void syscall_init(void) {
  intr_register_int(0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void syscall_handler(struct intr_frame *f UNUSED) {
  printf("system call!\n");
  thread_exit();
}
```

任何系统调用都只是打印一句话然后杀掉进程。

### Lab 2 改动（~310 行）

**① 新增 8 个 `#include`**

```c
#include "threads/synch.h"      // lock
#include "threads/vaddr.h"      // is_user_vaddr, PHYS_BASE
#include "userprog/pagedir.h"   // pagedir_get_page
#include "userprog/process.h"   // process_execute, process_wait
#include "devices/shutdown.h"   // shutdown_power_off (SYS_HALT)
#include "devices/input.h"      // input_getc (SYS_READ stdin)
#include "filesys/filesys.h"    // filesys_open, filesys_create, filesys_remove
#include "filesys/file.h"       // file_read, file_write, file_close, etc.
```

**② 新增全局变量**

```c
static struct lock filesys_lock;   // 保护所有文件系统操作（Pintos FS 非线程安全）
```

**③ 指针验证三件套**

```c
// check_ptr(ptr) — 验证单个指针
//   NULL → 杀进程
//   >= PHYS_BASE → 杀进程
//   页目录中无映射 → 杀进程

// check_str(s) — 逐字节验证字符串
//   对每个字节调用 check_ptr，直到 '\0'

// get_arg(f, n) — 从用户栈取第 n 个参数
//   计算 esp + 4*(n+1) 的地址
//   验证这 4 字节都合法
//   解引用返回
```

**④ 实现 13 个系统调用**

| 系统调用 | 主要逻辑 |
|---|---|
| `SYS_HALT` | 调用 `shutdown_power_off()` |
| `SYS_EXIT` | 设 `exit_status`，调用 `thread_exit()` |
| `SYS_EXEC` | `check_str(cmd)` → `process_execute(cmd)` → 返回 tid |
| `SYS_WAIT` | `process_wait(tid)` → 返回退出码 |
| `SYS_CREATE` | 加锁 → `filesys_create(name, size)` → 解锁 |
| `SYS_REMOVE` | 加锁 → `filesys_remove(name)` → 解锁 |
| `SYS_OPEN` | 加锁 → `filesys_open(name)` → 解锁 → 分配 fd（`fd_next++`） |
| `SYS_CLOSE` | 验证 fd → 加锁 → `file_close()` → 解锁 → 清空 `fd_table[fd]` |
| `SYS_FILESIZE` | `file_length(fd_table[fd])` |
| `SYS_READ` | fd=0 → `input_getc()` 循环 / 否则 `file_read()` |
| `SYS_WRITE` | fd=1 → `putbuf()` / 否则 `file_write()` |
| `SYS_SEEK` | `file_seek(fd_table[fd], pos)` |
| `SYS_TELL` | `file_tell(fd_table[fd])` |

**⑤ 统一退出入口**

```c
static void syscall_exit(int status) {
    thread_current()->exit_status = status;
    thread_exit();
    // thread_exit() → process_exit() → 打印退出信息 + 清理资源
}
```

所有错误路径（非法指针、非法 fd、default case）都统一走 `syscall_exit(-1)`。

---

## 7. exception.c — 异常处理补丁

### Lab 1 原版

```c
case SEL_UCSEG:
    printf("%s: dying due to interrupt %#04x (%s).\n", ...);
    intr_dump_frame(f);
    thread_exit();          // ← 直接退出，exit_status 保持默认 0
```

### Lab 2 改动（加了 1 行）

```c
case SEL_UCSEG:
    printf("%s: dying due to interrupt %#04x (%s).\n", ...);
    intr_dump_frame(f);
    thread_current()->exit_status = -1;   // ← 新增：异常终止 = 退出码 -1
    thread_exit();
```

**为什么需要**：如果用户程序触发了非法内存访问、除零等异常，内核会杀掉它。Pintos 规范要求这种情况下 `wait()` 必须给父进程返回 -1。不加这行的话，`exit_status` 会保持默认值 0，父进程以为子进程正常退出了。

---

## 8. 各修改之间的依赖关系

```
                  thread.h（新增字段）
                  ┌──────────────┐
                  │ exit_status  │
                  │ children     │←───── process.h: struct child_info
                  │ child_info   │
                  │ exec_file    │
                  │ fd_table[]   │
                  │ fd_next      │
                  └──────┬───────┘
                         │ 被以下文件使用
           ┌─────────────┼──────────────┐
           ↓             ↓              ↓
     thread.c       process.c       syscall.c
     (初始化)     (生命周期管理)    (系统调用入口)
                       │
                       ├── process_execute(): 创建 child_info + 等 load
                       ├── start_process():  恢复 child_info + 通知 load 结果
                       ├── process_wait():   sema_down 等退出
                       ├── process_exit():   sema_up + 清理 fd + deny-write
                       ├── load():           提取 exe_name + deny-write
                       └── setup_stack():    参数解析压栈

     exception.c
     (exit_status = -1)
```

**数据流总结**：

1. **创建**：`SYS_EXEC` → `syscall.c` → `process_execute()` → `thread_create()` → `start_process()` → `load()` → `setup_stack()` → 用户态
2. **退出**：用户调 `exit()` 或被异常杀 → `syscall_exit()` 或 `exception.c` → `thread_exit()` → `process_exit()` → 通知父进程 + 清理
3. **等待**：`SYS_WAIT` → `process_wait()` → `sema_down` 阻塞 → 子进程 `process_exit()` 中 `sema_up` → 父进程被唤醒 → 读取退出码
