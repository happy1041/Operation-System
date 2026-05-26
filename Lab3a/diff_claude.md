# Lab 3a — `src_old` → `src` 改动讲解

本文逐项对比 `pintos/src_old`（Lab 2 提交时的快照）与当前 `pintos/src`（Lab 3a 完成态）。结构按"主题"组织：先讲新增的虚拟内存子系统，再讲为接入它而修改的现有模块，最后讲构建与链接脚本的配套改动。

---

## 总览

Lab 3a 的目标是把 Lab 2 中"一次性把可执行文件全部读进内存、栈页直接 palloc"的模型，替换为按需分页 + 帧管理 + swap。改动可以归为四类：

| 类别              | 文件                                                                                                       | 说明                                                                     |
| --------------- | -------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------- |
| **新增** VM 子系统   | `vm/page.{c,h}`、`vm/frame.{c,h}`、`vm/swap.{c,h}`                                                         | 三个核心模块：补充页表 (SPT)、帧表、swap 表                                            |
| **接入** VM 的现有模块 | `threads/init.c`、`threads/thread.{c,h}`、`userprog/process.c`、`userprog/exception.c`、`userprog/syscall.c` | 在合适的时机调用 VM API；为每线程加 SPT 与 `saved_esp`                                |
| **构建/链接**       | `Makefile.build`、`threads/kernel.lds.S`、`lib/user/user.lds`、`tests/Make.tests`                           | 把 vm 模块编进 build；让链接脚本兼容新工具链产生的更多 section                               |
| **小修补**         | `filesys/filesys.h`                                                                                      | 把 `fs_device` 从 tentative definition 改成 `extern`，避免 VM 后多 TU 引用导致的重复符号 |

整体设计采用**惰性装载 (lazy loading) + 全局 clock 替换 + 按需 swap**：可执行文件的每个用户页在 `load_segment` 时只登记 SPT 条目，真正的内容读入推迟到 page fault；当 frame 不足时由 clock 算法挑 victim，并按"文件干净页可丢弃，其余写 swap"的策略转移内容。

---

## 心智模型与整体框架

> 这一节是给"已经知道每个 API 干什么，但想从整体角度把它们串起来"的读者准备的。
> 只读这一节，应当也能解释清楚 Lab 3a 在做什么，以及为什么这么做。

### 1. Lab 2 → Lab 3a：到底改了什么

Lab 2 的内存模型一句话：**进程启动时把可执行文件全装入物理内存；栈固定一页；物理内存不够就 PANIC。**

它有三个硬限制：
1. 程序不能大于物理内存（哪怕 90% 代码跑不到也得装）；
2. 栈只有 4 KB，递归稍深就溢出；
3. 物理页框无回收，进程退出前独占。

Lab 3a 去掉这三个限制，套路是"虚页/物理页框分离 + 按需调度"：

| 维度       | Lab 2          | Lab 3a                              |
| -------- | -------------- | ----------------------------------- |
| 装载时机     | 立即装入           | 第一次访问时装入（lazy loading）              |
| 栈大小      | 固定 1 页         | 按需增长，上限 8 MB                        |
| 物理内存不足   | PANIC          | clock 算法淘汰一页 → 写 swap 或丢弃           |
| 页面真相来源   | 硬件页目录          | 软件 SPT + 硬件页目录共同表达                  |

### 2. 三层抽象（这张图必须记住）

```
┌───────────────────────────────────────────────────────────────┐
│  Supplemental Page Table (SPT)            ── 每进程一份         │
│  "这个进程理论上拥有哪些虚页？每页应该从哪里来？"                 │
│  哈希表，键 = 用户虚页地址                                       │
│  条目即使 loaded == false 也存在（记录文件偏移 / swap 槽位）       │
└────────────────────────────────┬──────────────────────────────┘
                                 │ page_lookup / page_load
                                 ▼
┌───────────────────────────────────────────────────────────────┐
│  Frame Table                              ── 全局一份           │
│  "哪些物理页框已经分给了谁？谁能被淘汰？"                         │
│  链表 + clock_hand，只跟踪当前 resident 的页                     │
└────────────────────────────────┬──────────────────────────────┘
                                 │ pagedir_set_page / clear_page
                                 ▼
┌───────────────────────────────────────────────────────────────┐
│  Page Directory                  ── 每进程一份，硬件直接消费     │
│  "CPU 访问 va 现在能命中哪个物理页？"                            │
│  仅包含 present 映射                                            │
└───────────────────────────────────────────────────────────────┘
```

三层的**一致性不变量**（debug 时一定要心里默念）：

- `pagedir` 有映射 ⇒ SPT 有条目 ∧ frame_table 有条目；
- SPT 条目 `loaded == true` ⇔ frame_table 有条目 ∧ pagedir 有映射；
- SPT 条目 `loaded == false` ⇒ pagedir 无映射（但 SPT 条目仍在）；
- frame_table 条目 `owner==T, upage==U` ⇒ T 的 SPT 中 U 对应条目 `loaded == true`。

### 3. 一个用户页的生命周期（状态机）

```
   [unregistered]
        │  load_segment / setup_stack / 栈增长
        ▼
   ┌─────────────────────────────┐
   │  REGISTERED, loaded=false   │ ←──┐
   │  (FILE / ZERO / STACK)      │    │
   └────┬────────────────────────┘    │
        │ page fault → page_load      │
        ▼                             │  干净 file 页：
   ┌─────────────────────────────┐    │  frame_evict 不写 swap
   │  RESIDENT, loaded=true      │────┘  type 仍是 FILE
   │  (frame_table 有项)         │
   └────┬────────────────────────┘
        │ frame_evict + 写 swap（脏页 / ZERO / STACK）
        ▼
   ┌─────────────────────────────┐
   │  SWAPPED OUT                │ type = VM_PAGE_SWAP
   │  loaded=false, swap_index=N │ swap_index ≠ SIZE_MAX
   └────┬────────────────────────┘
        │ 再次 fault → swap_in
        └──→ 回到 RESIDENT，可无限循环

   [destroyed]  process_exit 时整张 SPT 被销毁
```

两个关键洞察：

1. **`type` 不是静态属性**。一个 FILE 页被写脏后再被淘汰会转成 SWAP；STACK / ZERO 页被淘汰也变 SWAP。`type` 实际表示"下次重装时该从哪里来"。
2. **干净 file 页淘汰时不写 swap** ——直接清掉映射即可，下次缺页再从原文件读。这是省 swap 空间的核心优化（`frame_evict` 里的 `if (!(VM_PAGE_FILE && !dirty))`）。

### 4. 三种"补页"入口

`page_load` 是所有装入操作的最终汇集点，但调用它的入口有三个，区别非常关键：

| 入口            | 调用方                                          | 上下文                  |
| ------------- | -------------------------------------------- | -------------------- |
| 用户态 page fault | `exception.c::page_fault`                    | CPU 访问触发缺页异常，被动      |
| syscall 指针校验  | `syscall.c::check_ptr → page_resolve`        | syscall 入口校验用户指针，主动  |
| syscall 缓冲区 pin | `syscall.c::pin_buf/pin_str → page_resolve_and_pin` | 即将持文件系统锁前，主动 |

**为什么 syscall 要主动 resolve 而不依赖硬件 fault？**

内核在 syscall 里访问没装入的用户页时，如果纯靠硬件 fault，handler 收到的 `f->esp` 来自内核态、不是用户态 esp，无法判断栈增长。`saved_esp` 兜底这一点：syscall 入口先把 `f->esp` 存进 thread，万一内核态又触发了 fault，handler 用 `saved_esp` 而不是 `f->esp`。

但更重要的原因是死锁：持文件系统锁后再 fault 会递归试图持文件系统锁（见 5.4），所以必须在持锁前主动 resolve + pin。

### 5. 关键流程图解

#### 5.1 进程启动到第一条用户指令

```
process_execute(cmd)
  └─ thread_create(start_process, ...)
        └─ start_process(cmd):
             ├─ load(cmd, &esp, &eip):
             │    ├─ filesys_open(executable)
             │    ├─ for each ELF segment:
             │    │    load_segment(file, ofs, upage, ...):
             │    │      for each page:
             │    │        page_register_file(spt, upage, file, ofs, ...)
             │    │                              ↑ 只登记，不读文件
             │    └─ setup_stack(&esp):
             │         frame_allocate(PAL_USER|PAL_ZERO, stack_page)
             │         page_register_stack(spt, stack_page, loaded=true)
             │         install_page(stack_page, kpage, true)
             └─ 跳转到 entry point
                  → MMU 缺页 → page_fault → page_load
                     ↑ 这一刻才真正去 file_read 第一页代码
```

**记忆点**：lazy loading 的本质是把"装入"从 `load` 推迟到第一次 page fault；栈首页是唯一在 `setup_stack` 里立刻装入的页。

#### 5.2 一次普通的用户态 page fault

```
CPU 访问 va → MMU 发现 PTE invalid → INT 0E
   ↓
exception.c::page_fault(f):
   ├─ thread->saved_esp = f->esp        (备用)
   ├─ page = page_lookup(spt, fault_addr)
   ├─ 若 page==NULL 且 should_grow_stack(fault_addr, esp):
   │      page_register_stack(spt, ..., loaded=false)
   │      page = page_lookup(...)
   └─ if (page != NULL && 权限相符 && page_load(page)):
         return     ←─ 异常返回，重试同条指令，这次 MMU 命中
   else: kill(f)    ←─ 非法访问，进程以 -1 退出
```

`page_load` 内部按 `type` 分四路：
- `VM_PAGE_FILE`：持文件系统锁 → `file_seek` + `file_read` → memset 补零；
- `VM_PAGE_SWAP`：`swap_in`（顺便释放槽位）；
- `VM_PAGE_ZERO` / `VM_PAGE_STACK`：`frame_allocate(PAL_ZERO)` 已经清零，无 I/O。

#### 5.3 frame 不足触发淘汰

```
page_load → frame_allocate(flags, upage)
              ├─ palloc_get_page → NULL  (用户池满)
              └─ lock_acquire(frame_lock)
                  └─ frame_evict:
                       victim = frame_pick_victim()   ← clock 算法扫 2*N 轮
                       page   = page_lookup(victim->owner->spt, victim->upage)

                       victim->busy = pinned = true   ← 锁定
                       page->busy = true

                       dirty = pagedir_is_dirty(victim->owner->pagedir, ...)
                       pagedir_clear_page(victim->owner->pagedir, ...)
                          ↑ Q 的页表里这条映射被清掉

                       if !(FILE && !dirty):          ← 需要写 swap
                          page->swap_index = swap_out(victim->kpage)
                          page->type = VM_PAGE_SWAP

                       page->loaded = false
                       page->busy   = false           ← Q 此后可以再 fault

                       victim->owner = P              ← 同一个 kpage 换主人
                       victim->upage = upage_new
                       victim->pinned = busy = false
```

**核心观察**：`frame_evict` **不会** `palloc_free` 物理页，而是"换主人"——把同一个 kpage 从 Q 的 SPT/pagedir 里抽出来，挂给 P。所以 frame_table 的大小在淘汰前后不变。

#### 5.4 系统调用读用户缓冲区

```
sys_read(fd, buf, sz):
   check_ptr(buf)                ← page_resolve(buf, true)，含装入
   check_buf(buf, sz)            ← 跨页时逐页 check_ptr
   pin_buf(buf, sz, write=true)  ← 逐页 page_resolve_and_pin
   ────── 此时所有相关页 resident + pinned ──────
   lock_acquire(filesys_lock)
   file_read(...)                ← 安全：不会 fault，pinned frame 不会被淘汰
   lock_release(filesys_lock)
   ──────────────────────────────────────────────
   unpin_buf(buf, sz)
```

**为什么必须先 pin 再持锁？** 反过来想：先持 filesys_lock 再 fault → page_load 内部要 `filesys_acquire()` → 立刻递归死锁。把"页面 ready"和"持文件系统锁"在时间上分离，是消除整类死锁的最简洁手段。

#### 5.5 进程退出

```
process_exit():
   frame_release_process(cur)     ← 删 frame_table 里所有 owner=cur 的项
                                    （但不 palloc_free kpage）
   page_table_destroy(&cur->spt)  ← hash_destroy 回调：归还 SWAP 槽位 + free 元数据
   关闭所有 fd, file_close(exec_file)
   pd = cur->pagedir; cur->pagedir = NULL
   pagedir_activate(NULL)
   pagedir_destroy(pd)            ← 这里才真正 palloc_free 所有 present 物理页
```

顺序约束：
- `frame_release_process` 必须在 `pagedir_destroy` 之前（否则 frame_table 残留指向已释放页框的指针）；
- `page_table_destroy` 必须在 `pagedir_destroy` 之前（销毁时要读 `swap_index` 调 `swap_free`）；
- 物理页框的最终释放仍由 `pagedir_destroy` 完成，VM 子系统不接管这块。

### 6. 并发模型：四把锁 + 三个状态位

锁：

| 锁              | 保护                  | 持锁典型时长                          |
| -------------- | ------------------- | ------------------------------- |
| `frame_lock`   | frame_table + clock_hand | 短—中（frame_evict 含 swap_out I/O） |
| `swap_lock`    | swap_map 位图          | 极短（仅 scan_and_flip / reset）     |
| `filesys_lock` | 整个文件系统              | 中（一次 file I/O）                  |
| SPT            | 每进程私有               | 无需锁，不跨线程                        |

状态位（不是锁，但起同步作用）：

| 字段                   | 作用                                            |
| -------------------- | --------------------------------------------- |
| `vm_page.busy`       | 虚页正在被装入/换出 → 其他 fault 看到就 `thread_yield()` 重试 |
| `frame_entry.busy`   | 页框正在被复用 → clock 跳过                            |
| `frame_entry.pinned` | 内核临时禁止淘汰 → clock 跳过                           |

**反死锁的核心一条规则**：

> 持有 `filesys_lock` 之前，必须把所有可能访问的用户页 `page_resolve` 完毕，并对持锁期间会读写的部分 `frame_pin`。

只要遵守这条，"持锁 → fault → 再持锁"的循环就不可能形成。

### 7. 易错点速查

按踩坑概率排序：

1. **`page_load` 的 early return 忘了 `page->busy = false`** → 此页永远卡死；
2. **`load_segment` 忘了 `ofs += page_read_bytes`** → 所有页从同一文件位置读，内容全错；
3. **`pin_buf` 早退路径忘了 `unpin_buf`** → 对应 frame 永远不可淘汰，最终 OOM；
4. **`clock_hand` 在 `list_remove` 前没推进** → 指针悬空，下次扫描崩溃；
5. **`swap_index` 重复释放** → `swap_in` 已经 reset，destroy 不能再 reset；解法：装入后立刻 `swap_index = SIZE_MAX`；
6. **fork/exec 时 `saved_esp` 没清** → 误判栈增长；
7. **`frame_evict` 选中 SPT 里没有的页** → 上层逻辑没维护好"frame_table.entry ⇒ SPT.entry"不变量；
8. **持 filesys_lock 时 fault** → 死锁；解法：所有持锁前先 resolve + pin。

### 8. 一句话总结

> **SPT 是软件层的"真相之书"（包括不在内存的页），frame_table 是物理页框的账本（只含 resident），pagedir 是给硬件看的当前视图。**
>
> **page fault 是这三层之间的同步事件，`busy` / `pinned` 是临时打破异步的同步原语，整套设计的核心 trade-off 是用一小撮元数据换"程序可以比物理内存大"。**

---

## 一、新增：`vm/` 子系统

### 1.1 `vm/page.{c,h}` — 补充页表 (SPT)

**职责**：每个进程持有一张以用户虚页地址为键的哈希表，记录当前在内存中或可能在内存中的用户页的"软件层信息"。

核心类型：

```c
enum vm_page_type { VM_PAGE_FILE, VM_PAGE_ZERO, VM_PAGE_STACK, VM_PAGE_SWAP };

struct vm_page {
    void *upage;
    enum vm_page_type type;
    bool writable;
    bool loaded;          /* 当前是否驻留某个 frame */
    bool busy;            /* 装入/换出过程中的串行化标志 */
    struct file *file;    /* 文件后备：文件指针、偏移、读字节、补零字节 */
    off_t ofs;
    uint32_t read_bytes;
    uint32_t zero_bytes;
    size_t swap_index;    /* 被换出时使用 */
    struct hash_elem elem;
};
```

对外 API：

| 函数                                     | 作用                                                                   |
| -------------------------------------- | -------------------------------------------------------------------- |
| `page_table_init / page_table_destroy` | 创建/销毁某进程的 SPT；销毁时回调 `page_destroy_action` 会主动归还 swap 槽位              |
| `page_register_file`                   | `load_segment` 时为每个虚页登记惰性装载条目（`read_bytes == 0` 时退化为 `VM_PAGE_ZERO`） |
| `page_register_stack`                  | 设置初始栈页或扩展栈页时登记 `VM_PAGE_STACK`                                       |
| `page_lookup`                          | 按页向下取整后做哈希查询                                                         |
| `page_resolve(uaddr, write)`           | 系统调用使用的"主动补页"入口：内核态确认地址可访问，必要时触发栈增长与 `page_load`                     |
| `page_resolve_and_pin / page_unpin`    | 在确保补页成功后，对应 frame 临时 pin 住，避免在 I/O 中被淘汰                              |
| `page_load`                            | 真正分配 frame 并把内容装入；按 `type` 分四路：文件读、swap 读、零页、栈页                      |

`page_load` 关键逻辑：

```c
while (page->busy) thread_yield();   /* 等待并发装入/换出完成 */
if (page->loaded) return true;       /* 别人已经装好 */
page->busy = true;
kpage = frame_allocate(<flags>, page->upage);
... 按 type 填内容 ...
pagedir_set_page(...);
page->loaded = true;
page->busy = false;
```

`busy` 字段在 `page_load` 与 `frame_evict` 之间起到互斥作用：换出方先把对应 `vm_page` 标 busy 再清页表项；fault 方看到 busy 就 `thread_yield()` 重试，从而避免"一页被装入两次"或"装到一半被换出"的竞争。

栈增长启发式 `page_should_grow_stack`：使用 `cur->saved_esp` 作参考，要求 fault 地址同时满足 `addr >= esp - 32` 与 `addr >= PHYS_BASE - 8MB`，与手册推荐一致。

### 1.2 `vm/frame.{c,h}` — 全局帧表

**职责**：管理所有从用户池分配出去的物理页框，提供按需分配与淘汰。

核心类型与状态：

```c
struct frame_entry {
    void *kpage;          /* 物理页框对应的内核虚拟地址 */
    void *upage;          /* 当前映射到的用户虚页 */
    struct thread *owner;
    bool pinned;          /* 内核临时锁定，不可淘汰 */
    bool busy;            /* 正在 fault / evict */
    struct list_elem elem;
};

static struct list  frame_table;     /* 全部 frame 的全局链表 */
static struct lock  frame_lock;      /* 串行化 frame_table 修改 */
static struct list_elem *clock_hand; /* clock 算法的扫描位置 */
```

对外 API 与算法：

- **`frame_allocate(flags, upage)`**：先 `palloc_get_page` 尝试直接拿空闲页；成功则创建 `frame_entry` 并挂入 `frame_table`；失败则在 `frame_lock` 保护下进入 `frame_evict`。
- **`frame_evict`**：
  1. 通过 `frame_pick_victim` 用 clock 算法挑 victim；
  2. 把 victim frame 标 `busy/pinned`，victim 对应的 `vm_page` 标 `busy`；
  3. `pagedir_clear_page` 把 Q 的页表映射清掉；
  4. 如果 victim 是干净的 `VM_PAGE_FILE` 页，直接丢弃（将来仍可重读文件）；否则 `swap_out` 写盘并把页面元数据转为 `VM_PAGE_SWAP`；
  5. 把 frame 的 `owner`、`upage` 改为新主，必要时按 `PAL_ZERO` 清零，返回给调用者。
- **`frame_pick_victim`**：从 `clock_hand` 开始最多扫描 2*N 次。跳过 `pinned` / `busy` 的 frame 与对应 `vm_page` 也 busy 的 entry；对剩下的检查 owner 页表中的 accessed bit，若为 1 则清零给第二次机会，否则选中。第二轮扫描里 accessed 已被清，因此终止有保证。
- **`frame_pin / frame_unpin`**：syscall 路径在持文件系统锁前把对应 frame 钉住，避免在中途被换出。
- **`frame_release_process`**：进程退出时由 `process_exit` 调用，只删除该 owner 的 `frame_entry` 元数据；物理页框由后续 `pagedir_destroy` 路径释放。

### 1.3 `vm/swap.{c,h}` — Swap 子系统

**职责**：用块设备承担的 swap 分区，按页粒度分配/读写/回收。

```c
#define SECTORS_PER_PAGE (PGSIZE / BLOCK_SECTOR_SIZE)
static struct block  *swap_block;
static struct bitmap *swap_map;
static struct lock    swap_lock;
```

- `swap_init`：通过 `block_get_role(BLOCK_SWAP)` 找到 swap 分区，按 `PGSIZE / BLOCK_SECTOR_SIZE` 划分槽位；没有 swap 时 `swap_map = NULL`，后续 `swap_out` 的 `ASSERT` 会拒绝换出。
- `swap_out(kpage)`：`bitmap_scan_and_flip` 找空闲槽位，再循环 `block_write` 一整页；满则 `PANIC("swap partition full")`。
- `swap_in(slot, kpage)`：循环 `block_read` 后立刻 `bitmap_reset` 释放槽位。
- `swap_free(slot)`：进程在仍持有 swap 内容但即将销毁 SPT 时显式归还槽位（由 `page_destroy_action` 触发）。

bitmap 的扫描-翻转与释放分别独立加 `swap_lock`，与块设备本身的 I/O 解耦，避免 I/O 时间内长时间持锁。

---

## 二、对现有模块的修改

### 2.1 `threads/thread.{c,h}`

为 `struct thread` 增加两个 VM 字段（`#ifdef VM`）：

```c
struct hash spt;     /* 每进程补充页表 */
void *saved_esp;     /* 最近一次记录的用户态 esp，供内核态 fault 参考 */
```

并新增两个函数 + 一个 file-scope 标志：

```c
static bool vm_ready;            /* 第一阶段 thread_init 时还不能 hash_init */
void thread_vm_init(struct thread *t);  /* 实际 page_table_init */
void thread_vm_enable(void);            /* 由 pintos_init 在 malloc/palloc 就绪后调用，置 vm_ready 并补做 initial_thread 的 SPT */
```

`init_thread` 中先 `memset` SPT 字段、置 `saved_esp = NULL`，仅当 `vm_ready` 为真才调用 `thread_vm_init`。这一两阶段初始化解决了"`thread_init` 早于 `malloc_init`，但 `hash_init` 依赖 `malloc`"的鸡和蛋问题。

### 2.2 `threads/init.c`

在 `pintos_init` 中插入 VM 启用点：

```c
palloc_init(...);
malloc_init();
#ifdef VM
thread_vm_enable();          /* SPT 启用：此时 malloc 已就绪 */
#endif
paging_init();
#ifdef VM
frame_init();                /* 初始化 frame_table 与 clock_hand */
#endif
... ide_init / locate_block_devices ...
#ifdef VM
swap_init();                 /* swap 必须在块设备探测之后 */
#endif
filesys_init(...);
```

顺序很关键：`frame_init` 必须在 paging 之后但在第一次用户页分配之前；`swap_init` 依赖块设备已注册。

### 2.3 `userprog/process.c`

**`load_segment`** — 由"立刻装入"改为"登记 SPT，惰性装入"：

```c
#ifdef VM
if (!page_register_file(&cur->spt, upage, file, ofs,
                        page_read_bytes, page_zero_bytes, writable))
    return false;
#else
    /* 旧的 palloc + file_read + install_page 路径保留为非 VM 编译路径 */
#endif
...
ofs += page_read_bytes;   /* 新增：把文件偏移与虚页同步推进 */
```

注意旧代码用 `file_seek(file, ofs)` 一次性把指针定位到段首，循环里靠顺序读推进；新代码因为每页元数据独立保存 `ofs`，所以需要在循环里**显式累加** `ofs`，并删去开头的 `file_seek`（在 `#ifndef VM` 分支保留）。

**`setup_stack`** — 通过 `frame_allocate` + `page_register_stack` 走 VM 路径：

```c
#ifdef VM
kpage = frame_allocate(PAL_USER | PAL_ZERO, stack_page);
if (kpage != NULL) {
    if (!page_register_stack(&cur->spt, stack_page, /*loaded=*/true))
        { frame_free(kpage); return false; }
    if (install_page(stack_page, kpage, true))
        *esp = PHYS_BASE;
    else { page_unregister(&cur->spt, stack_page); frame_free(kpage); return false; }
}
#else
    /* 旧 palloc_get_page 路径保留 */
#endif
```

栈首页登记时 `loaded = true`，因为它在创建时就已经分配并安装到页表。

**`process_exit`** — 在销毁 `pagedir` 之前插入：

```c
#ifdef VM
frame_release_process(cur);   /* 删除该 owner 的所有 frame_entry */
page_table_destroy(&cur->spt);/* hash_destroy 中归还每个 swap 槽位 */
#endif
```

注意顺序：先放 frame 元数据，再销毁 SPT；SPT 销毁回调会读 `page->swap_index` 调 `swap_free`，因此必须在 `pagedir_destroy` 之前完成。

### 2.4 `userprog/exception.c`

`page_fault` 整个被 `#ifdef VM` 包成"VM 版"和"旧版"两个独立实现。VM 版的关键步骤：

1. 用户态 fault 时把 `f->esp` 存到 `thread_current()->saved_esp`，供未来内核态访问相同用户页时参考；
2. 仅当 `not_present && is_user_vaddr(fault_addr)` 时考虑补页；
3. 先 `page_lookup`，找不到再判断 `should_grow_stack(fault_addr, user_esp)`，符合即 `page_register_stack(..., loaded=false)` 然后再次 lookup；
4. 找到合法且写权限相符的 `vm_page` 时调 `page_load`，成功则直接 `return`；
5. 否则落到旧的 `printf + kill(f)` 终止路径。

栈增长启发式 `should_grow_stack` 复刻了 `page.c` 内部的同名规则（`addr >= stack_bottom && addr >= esp - 32`），保证 fault handler 与 `page_resolve` 的标准一致。

### 2.5 `userprog/syscall.c`

新增四个静态辅助函数：`pin_buf / unpin_buf / pin_str / unpin_str`。它们都用 `page_resolve_and_pin` 把缓冲区涉及的每个页 pin 住，I/O 结束后再 `page_unpin`。失败一律 `syscall_exit(-1)`。

`check_ptr` 在 VM 编译下改为：

```c
if (!page_resolve(ptr, false))
    syscall_exit(-1);
```

即把"是否已经映射"的检查升级为"是否可补页（含栈增长）"。`check_buf` / `check_str` 仍用 `pagedir_get_page` 做粗筛 —— 真正的补页与 pin 由 `pin_buf` / `pin_str` 在调 `lock_acquire(&filesys_lock)` 之前完成。

`syscall_handler` 入口新增一行：

```c
#ifdef VM
thread_current()->saved_esp = f->esp;
#endif
```

记录用户态 esp，是为了让"系统调用进入内核后再触发的 page fault"也能正确判断栈增长。

涉及用户缓冲区的 syscall（`create / remove / open / read / write` 等）全部按"`check_*` → `pin_*` → `lock_acquire(filesys_lock)` → I/O → `lock_release` → `unpin_*`"的模板改造，保证：

- 持文件系统锁前页一定 resident；
- 持锁期间页不会被换出；
- 不存在"持锁时再 fault"导致 frame_lock 与 filesys_lock 的环路等待。

---

## 三、构建与链接

### 3.1 `Makefile.build`

把注释掉的占位 `vm_SRC` 替换为：

```make
vm_SRC  = vm/frame.c
vm_SRC += vm/page.c
vm_SRC += vm/swap.c
```

`Make.vars` 形如 `KERNEL_SUBDIRS = threads devices lib lib/kernel userprog vm` 已经把 vm 当作有效子目录列入；这里只是把对应的源文件挂到编译列表。

### 3.2 `threads/kernel.lds.S` 与 `lib/user/user.lds`

两个链接脚本都做了同一类扩展：

- 把 `.text / .rodata / .data / .bss` 改成同时匹配 `.text.* / .rodata.* / .data.* / .bss.*`；
- `.data` 段补收 `.got / .got.plt / .data.rel*` 等 PIC 工具链常见的额外段；
- `.bss` 段补收 `COMMON`。

`user.lds` 还显式加了 `PHDRS { text PT_LOAD FLAGS(5); data PT_LOAD FLAGS(6); }`，并把 `:text / :data` 标注挂到各 section 上。

这些不是 VM 逻辑改动，但**没有它们 Lab 3a 多半装不起来**：本实验在用户与内核里都会触发原本不出现的额外段（栈页测试程序、新的 hash/list 用法等），旧链接脚本会因为有 section 落在 `_end_bss` 之外或者跨页边界而导致测试程序在 `setup_stack` 之外的页 fault，或者内核 BSS 没清干净。改完之后所有目标 section 都被显式纳入，与 ELF 加载器的视图保持一致。

### 3.3 `tests/Make.tests`

```diff
- TESTCMD = pintos -v -k -T $(TIMEOUT)
+ TESTCMD = $(PINTOS) -v -k -T $(TIMEOUT)
```

改为通过 `PINTOS` 变量调用而不是裸 `pintos`，这样可以在不同环境下覆盖 pintos 脚本路径（Lab 3a 的 grading 脚本会显式传入）。

### 3.4 `filesys/filesys.h`

```diff
- struct block *fs_device;
+ extern struct block *fs_device;
```

旧版用 tentative definition；Lab 2 时只有 `filesys.c` 一处定义，多 TU 引用不出错。Lab 3a 引入 VM 之后多个新 TU（`page.c` 经由 `file.h`、`frame.c` 间接）都包含 `filesys.h`，部分链接器会在每个 TU 生成一个 `fs_device` 定义，导致多重定义。改成 `extern` 之后只在 `filesys.c` 真正定义一次。

---

## 四、同步设计速览

整套设计采用"少量较粗粒度锁 + 页/帧级 busy/pinned 状态位"：

| 锁 / 状态位 | 保护对象 |
| --- | --- |
| `frame_lock` | `frame_table` 链表与 `clock_hand` |
| `swap_lock` | `swap_map` 位图 |
| `filesys_lock` | 文件系统（沿用 Lab 2） |
| `vm_page.busy` | 同一页的并发 fault/evict 互斥 |
| `frame_entry.busy` | 同一帧的换出过程互斥 |
| `frame_entry.pinned` | I/O 期间禁止被淘汰 |

避免死锁的关键约束：**进入持有 `filesys_lock` 的临界区之前，先确保相关用户页 resident 且 pinned**。这条规则消除了"持文件系统锁 → 再触发 page fault → 在 page fault 中又试图持 frame_lock 并最终再回到文件系统读"的潜在环。

---

## 附：被原地保留的"非 VM 路径"

`process.c` 的 `load_segment`、`setup_stack` 以及 `syscall.c`、`exception.c` 中的旧逻辑都用 `#ifndef VM`/`#else` 保留下来，使得不开 `VM=1` 时仍能编译成 Lab 2 等价的二进制。Lab 3a 的工作并没有删掉旧路径，只是在 `#ifdef VM` 分支里完全替换为基于 SPT + 帧表 + swap 的新流程。
