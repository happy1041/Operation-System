# src_old 与当前 src 的改动对比说明

## 1. 对比范围

本文对比的是：

- 旧版本：`pintos/src_old`，可视为完成 Lab2 后的代码基线。
- 新版本：`pintos/src`，即当前通过 Lab3a 测试的代码。

本文只统计源码、构建脚本和测试脚本中的有效改动，不把生成文件、提交压缩包之类的内容算进来。

## 2. 总体结论

相对 `src_old`，当前 `src` 的主要变化可以概括为四类：

1. 新增了一套完整的虚拟内存子系统：补充页表、帧表、交换分区。
2. 修改了用户进程加载、缺页异常处理、系统调用中的用户内存访问逻辑，使其支持 lazy loading、stack growth、page eviction 和 swap。
3. 修改了线程初始化与内核启动流程，使 VM 相关数据结构在正确时机初始化。
4. 补充了若干构建、链接与测试兼容性修复，保证 `userprog` 和 `vm` 两种构建模式都能正常工作。

实际发生变化的核心文件如下：

- 新增文件：
	- `vm/frame.c`
	- `vm/frame.h`
	- `vm/page.c`
	- `vm/page.h`
	- `vm/swap.c`
	- `vm/swap.h`
- 修改文件：
	- `Makefile.build`
	- `filesys/filesys.h`
	- `lib/user/user.lds`
	- `tests/Make.tests`
	- `threads/init.c`
	- `threads/kernel.lds.S`
	- `threads/thread.c`
	- `threads/thread.h`
	- `userprog/exception.c`
	- `userprog/process.c`
	- `userprog/syscall.c`

## 3. 新增的 VM 子系统

### 3.1 `vm/page.c` / `vm/page.h`

这一组文件在 `src_old` 中完全不存在，是本次 Lab3a 的核心新增。

主要实现内容：

- 定义了 `struct vm_page`，用于描述一个用户虚页的元数据。
- 为每个进程建立 supplemental page table，底层使用 `hash` 组织。
- 支持注册三类页面来源：
	- 来自可执行文件的文件页。
	- 全零页。
	- 栈页。
- 支持在缺页时根据元数据真正装载页面，而不是在 `load_segment()` 时一次性把所有段读入内存。
- 支持 `page_resolve()`、`page_resolve_and_pin()`、`page_unpin()`，供系统调用路径在访问用户缓冲区前先把页面补齐并 pin 住。
- 支持在系统调用或用户态缺页时按启发式判断是否应当增长用户栈。

与 `src_old` 相比，本质变化是：

- 旧版没有补充页表，也没有“页面来源描述”这一层抽象。
- 新版把“页内容来自哪里、是否已加载、是否可写、是否已换出到 swap”这些信息显式保存下来，成为缺页处理的依据。

### 3.2 `vm/frame.c` / `vm/frame.h`

这一组文件在 `src_old` 中也不存在。

主要实现内容：

- 建立全局 frame table，跟踪每个用户物理页框当前属于哪个线程、映射到哪个用户虚页。
- 支持 `frame_allocate()`、`frame_free()`、`frame_pin()`、`frame_unpin()`、`frame_release_process()`。
- 当用户池页框耗尽时，使用时钟算法近似 LRU 选择 victim。
- 淘汰 victim 时检查 accessed/dirty 状态，必要时把页面写入 swap，再把 frame 重新分配给新的 faulting page。

与 `src_old` 相比，本质变化是：

- 旧版只会调用 `palloc_get_page()` 直接拿页，拿不到就失败。
- 新版在物理内存不足时会主动选择牺牲页并回收 frame，因此可以通过 page-linear、page-shuffle、page-merge-* 这类压力测试。

### 3.3 `vm/swap.c` / `vm/swap.h`

这一组文件在 `src_old` 中不存在。

主要实现内容：

- 使用 `BLOCK_SWAP` 块设备作为交换分区。
- 使用 `bitmap` 管理 swap slot 的分配与释放。
- 支持 `swap_out()`、`swap_in()`、`swap_free()`。

与 `src_old` 相比，本质变化是：

- 旧版没有交换分区机制，被淘汰页没有后备存储。
- 新版可以把匿名页或脏页写到 swap，再在后续缺页时换回。

## 4. 线程与初始化链路的改动

### 4.1 `threads/thread.h`

相对 `src_old`，在 `struct thread` 中新增了两项 VM 字段：

- `struct hash spt`：每个用户进程自己的补充页表。
- `void *saved_esp`：记录最近一次可用的用户栈顶，用于内核态触发缺页时判断是否允许栈增长。

此外，还新增了两个 VM 初始化函数声明：

- `thread_vm_init()`
- `thread_vm_enable()`

这意味着线程结构从 Lab2 的“只管理页目录和文件描述符”，扩展为“同时持有完整虚拟内存元数据”。

### 4.2 `threads/thread.c`

相对 `src_old`，这里的改动主要有三点：

1. 新增了 `vm_ready` 标志，用来控制何时允许初始化 SPT。
2. 在 `init_thread()` 中，除了原有的 `fd_table`、`child_info` 等 Lab2 字段外，又增加了 `spt` 与 `saved_esp` 的初始化逻辑。
3. 新增 `thread_vm_init()` 与 `thread_vm_enable()`，将最初线程的 VM 初始化推迟到内存分配器可用之后再进行。

这样修改的原因是：

- `hash_init()` 和相关 VM 结构会依赖动态内存。
- 如果像普通字段一样过早初始化，可能发生“线程系统先启动，但 VM 所需的内存子系统还没准备好”的问题。

### 4.3 `threads/init.c`

相对 `src_old`，这里新增了 VM 相关头文件引用，并在启动流程中插入了以下步骤：

- `thread_vm_enable()`
- `frame_init()`
- `swap_init()`

插入位置也很关键：

- 先完成 `palloc_init()` 和 `malloc_init()`。
- 再启用线程级 VM 数据结构。
- 再初始化 frame table 和 swap 子系统。

也就是说，当前启动流程不再只是“分页 + 中断 + 文件系统”，而是扩展成了“分页 + VM 元数据 + frame + swap + 用户异常处理”。

## 5. 用户进程加载与退出流程的改动

### 5.1 `userprog/process.c`

这是本次改动最多的旧文件之一。

#### 旧版行为

在 `src_old` 中：

- `load_segment()` 会立刻申请物理页。
- 直接从可执行文件中把段内容读进内存。
- 读完后立刻调用 `install_page()` 建立映射。
- `setup_stack()` 也会立刻申请并映射一页用户栈。

这种做法是 eager loading。

#### 新版行为

在当前版本中，这个文件被改造成“同时兼容非 VM 和 VM 两种构建模式”：

- 在 `#ifndef VM` 分支下，保留旧版 eager loading 行为，用于 `userprog` 构建。
- 在 `#ifdef VM` 分支下，`load_segment()` 不再立即读文件，而是调用 `page_register_file()` 将页面元数据登记到 SPT。
- `setup_stack()` 在 VM 模式下不再只是 `palloc_get_page()`，而是通过 `frame_allocate()` 与 `page_register_stack()` 建立可增长栈的第一页。
- `process_exit()` 在 VM 模式下新增：
	- `frame_release_process(cur)`
	- `page_table_destroy(&cur->spt)`

这意味着：

- 旧版“进程加载完成时，所有页面都已在内存中”。
- 新版“进程加载完成时，很多页面只登记了元数据，真正访问时才装载”。

#### 额外意义

这一文件的另一个重要改动是保留了非 VM 分支。

也就是说，当前代码不是单纯把 Lab2 loader 替换掉，而是做成了：

- `userprog` 构建仍可沿用旧逻辑。
- `vm` 构建切换到懒加载逻辑。

这样做可以避免当前 `src` 在非 VM 目录下编译失败。

## 6. 缺页异常与用户内存访问的改动

### 6.1 `userprog/exception.c`

相对 `src_old`，`page_fault()` 从“打印错误并杀死进程”改成了真正的 VM 缺页处理入口。

新增逻辑包括：

- 解析 fault address、缺页类型、是否写访问、是否来自用户态。
- 在用户态缺页时保存 `saved_esp`。
- 在内核态处理用户页访问时，使用 `saved_esp` 作为栈增长判断依据。
- 当 fault 地址属于合法用户空间且页面未装载时，先到 SPT 中查找对应 `vm_page`。
- 若查不到，但地址满足栈增长启发式，则自动注册新的栈页。
- 对可装载页调用 `page_load()` 完成缺页补页。

此外，还新增了 `should_grow_stack()`，并把用户栈增长上限定为 8 MB。

与 `src_old` 相比，本质变化是：

- 旧版 page fault 基本等价于“非法访问”。
- 新版 page fault 成为了按需分页、swap 回读、stack growth 的统一入口。

### 6.2 `userprog/syscall.c`

相对 `src_old`，这里的核心变化不是“多了哪些 syscall”，而是“系统调用如何安全访问用户地址空间”。

主要新增逻辑：

- 在 VM 模式下，进入 syscall 时保存 `thread_current()->saved_esp = f->esp`。
- `check_ptr()` 从旧版的“直接看 pagedir 是否已映射”，扩展为：
	- 非 VM 下仍用旧逻辑。
	- VM 下改为调用 `page_resolve()`，允许访问一个当前尚未驻留、但可以合法补页的地址。
- 为缓冲区和字符串新增 pin/unpin 逻辑：
	- `pin_buf()` / `unpin_buf()`
	- `pin_str()` / `unpin_str()`
- 在 `read`、`write`、`create`、`open`、`remove` 等会把用户指针传给文件系统的路径中，先 pin，再执行文件系统操作，再 unpin。

这样修改的原因是：

- Lab2 的检查方式只适用于“页已经在内存中”的情况。
- Lab3a 中，用户缓冲区对应的页可能还没装入，甚至可能在内核访问过程中被淘汰。
- 如果在持有文件系统锁时发生新的 page fault，会造成处理路径复杂甚至死锁，因此必须先补页并 pin 住。

## 7. 构建、链接与脚本层的改动

### 7.1 `Makefile.build`

相对 `src_old`，这里原本写的是：

- `# No virtual memory code yet.`
- `#vm_SRC = vm/file.c`

现在改成：

- `vm_SRC = vm/frame.c`
- `vm_SRC += vm/page.c`
- `vm_SRC += vm/swap.c`

这意味着 VM 子系统从“未接入构建”变成了正式编译的一部分。

### 7.2 `lib/user/user.lds`

相对 `src_old`，用户程序链接脚本做了两类关键变化：

1. 新增 `PHDRS`，显式拆分 `text` 和 `data` 两个 `PT_LOAD` segment。
2. 扩展了各 section 的匹配范围，把 `.text.*`、`.rodata.*`、`.data.*`、`.got*`、`.data.rel*`、`.bss.*` 等都纳入正确段中。

这样修改的作用是：

- 让用户代码段保持只读可执行。
- 让数据段保持可写。
- 避免现代工具链把整个用户程序链接成一个可读可写可执行的单段。

它直接影响到 `pt-write-code` 这类测试是否能正确识别“代码页不可写”。

### 7.3 `threads/kernel.lds.S`

相对 `src_old`，内核链接脚本也做了重要修复：

- `.rodata` 增加了对 `.rodata.*` 的覆盖。
- `.data` 扩展为包含 `.data.*`、`.got`、`.got.plt`、`.data.rel.ro*`、`.data.rel*` 等初始化数据段。
- `_start_bss` 被放到真正的 `.bss` 段起点，而不是让一些初始化数据意外落进 `bss_init()` 清零范围。

这部分不属于手册中最表面的 Lab3a 代码，但它是当前 VM 内核能稳定启动的关键基础修复。

### 7.4 `tests/Make.tests`

相对 `src_old`，这里只改了一行：

- 旧版：`TESTCMD = pintos -v -k -T $(TIMEOUT)`
- 新版：`TESTCMD = $(PINTOS) -v -k -T $(TIMEOUT)`

作用是：

- 不再强依赖环境变量 `PATH` 中必须存在裸 `pintos` 命令。
- 改为使用 `Make.config` 中定义好的 Pintos 脚本路径。

这是测试环境兼容性修复，不是 VM 核心功能，但能避免 `make grade` / `make check` 因命令找不到而失败。

## 8. 文件系统头文件的兼容性修复

### `filesys/filesys.h`

相对 `src_old`，这里只做了一个很小但必要的修复：

- 旧版：`struct block *fs_device;`
- 新版：`extern struct block *fs_device;`

原因是旧写法会在头文件中直接定义符号，导致多重定义链接问题。

这不是 Lab3a 手册要求的功能点，但在当前代码树下属于构建正确性的必要修复。

## 9. 按功能总结“从 Lab2 到 Lab3a”的变化

如果不按文件，而按功能看，可以把这次升级总结为：

1. **进程加载方式改变**
	 - Lab2：装载时立即把段全部读入内存。
	 - 当前：装载时只登记元数据，首次访问再真正读入。

2. **用户页不再要求始终常驻内存**
	 - Lab2：页面要么存在，要么非法。
	 - 当前：页面可以未装入、被换出、再换回。

3. **缺页异常从“报错”变成“调度点”**
	 - 当前的 page fault 会负责查 SPT、做 stack growth、从文件或 swap 取回页面。

4. **系统调用开始理解“非驻留用户页”**
	 - 当前 syscall 路径不再只接受已经映射的页，而是支持先补页、再 pin、再访问。

5. **用户栈从固定一页变成按需增长**
	 - 当前根据 `esp` 附近访问自动增长，最大 8 MB。

6. **内存不足时可以淘汰与换出**
	 - 当前通过 frame table + clock eviction + swap 支撑大工作集测试。

## 10. 哪些改动是 Lab3a 核心，哪些是配套修复

### Lab3a 核心改动

- `vm/page.c` / `vm/page.h`
- `vm/frame.c` / `vm/frame.h`
- `vm/swap.c` / `vm/swap.h`
- `userprog/process.c`
- `userprog/exception.c`
- `userprog/syscall.c`
- `threads/thread.c`
- `threads/thread.h`
- `threads/init.c`
- `Makefile.build`

### 为了稳定运行补上的配套修复

- `lib/user/user.lds`
- `threads/kernel.lds.S`
- `tests/Make.tests`
- `filesys/filesys.h`

其中前一组决定了 Lab3a 功能是否存在，后一组决定了这些功能能否在当前工具链、当前测试环境下稳定编译与运行。

## 11. 最终一句话总结

相对 `src_old` 的 Lab2 代码，当前 `src` 已经从“只支持 eager load 的用户程序系统”升级为“一套支持 supplemental page table、lazy loading、stack growth、frame eviction、swap，以及 syscall pinning 的 Lab3a 虚拟内存实现”，并额外补上了若干构建、链接和测试环境兼容性修复。
