-- 分隔参数，之前的是给 pintos 脚本的参数，之后的是给 pintos kernel 的参数

比如 `pintos -p file -a name -- -q`，`-p file`(put) 是指示 pintos 脚本把宿主机 file 挂载并准备传送到虚拟机，`-a name`(as) 命名。 后面的 `-q` 表示 kernel 执行完之后关机。

Here's a summary of how to create a disk with a file system partition, format the file system, copy the echo program into the new disk, and then run echo, passing argument PKUOS. (Argument passing won't work until you implemented it.)

It assumes that you've already built the examples in examples/ and that the current directory is userprog/build:

```bash
$ pintos-mkdisk filesys.dsk --filesys-size=2
$ pintos -- -f -q
$ pintos -p ../../examples/echo -a echo -- -q
$ pintos -- -q run 'echo PKUOS'
```

The three final steps can actually be combined into a single command:

```
$ pintos-mkdisk filesys.dsk --filesys-size=2
$ pintos -p ../../examples/echo -a echo -- -f -q run 'echo PKUOS'
```


You might want to create a clean reference file system disk and copy that over whenever you trash your filesys.dsk beyond a useful state, which may happen occasionally while debugging.


To view the layout of a particular executable, run objdump (80x86) or i386-elf-objdump (SPARC) with the -p option.

The code segment in Pintos starts at user virtual address 0x08048000

Argument Passing，感觉是 ICS 学过。

关于 system call 相关，利用 int 0x30 进入内核 syscall_handler，此时 user stack 上放着的就是 [systemcall number] [arg0] [arg1] [arg2] ... 关于用户程序的 return address 以及一些寄存器，是被中断机制自动压入内核栈的。


### Lab1 中的联动错误

sema up：/* 只有当唤醒的线程优先级高于当前线程时才 yield */

### Part 1
完成了参数传递+系统调用 halt, exit, exec, wait 的实现。
process.c 负责实现新进程（也包括 load，压栈），syscall.c 负责实现系统调用接口。

process.h 里写一下 exit_status, 以及父子进程关系的 struct child_process——process_wait,process_exit 需要用到。

### Part 2
fd_table
syscall.c 里首先添加一个全局文件锁
感觉要查一下文件系统里各种函数的实现。

PS：对于字符串指针的合法 check 需要逐字节验证
PS：kill，load 失败时对 exit_status 的设置

### Part 3
禁止写可执行文件：load 和 process_exit


### Task 1 — Argument Passing（`setup_stack`）

**目标**：让 `args-none`、`args-single`、`args-multiple` 通过。

**切入点**：process.c 的 `setup_stack()`，原始实现只是把 `esp` 设为 `PHYS_BASE`。

**步骤**：
1. 用 `strtok_r` 把命令行拆成 `argv[]`
2. 从右到左把字符串内容压栈，记录每个字符串的地址
3. 对齐 esp 到 4 字节
4. 压 `NULL` 哨兵、各参数地址（从右到左）、`argv` 指针、`argc`、假 return address

**验证方法**：在 `setup_stack` 末尾加 `hex_dump` 打印栈，对照 Pintos 文档 p2.md 中的示例图。

---

### Task 2 — Process Termination & `exit`（`process_exit`）

**目标**：让 `halt`、`exit`、`exit-bad-ptr` 通过，输出 `process: exit(N)`。

**切入点**：`process_exit()` 空实现，exception.c 的 `kill()`。

**步骤**：
1. 在 `struct thread` 里加 `int exit_status`，初始化为 0
2. `SYS_EXIT` syscall 把 status 写入 `thread->exit_status` 后 `thread_exit()`
3. `process_exit()` 里打印 `printf("%s: exit(%d)\n", ...)`
4. exception.c kill() 里加 `exit_status = -1`

---

### Task 3 — Syscall Infrastructure + File Syscalls

**目标**：让 `open-*`、`read-*`、`write-*`、`close-*`、`create-*`、`remove-*` 等通过。

**切入点**：`syscall_handler()` 原始是 `printf + thread_exit`。

**步骤**：
1. 实现 `check_ptr`（pagedir_get_page 验证）、`get_arg`
2. 加 `static struct lock filesys_lock`，在 `syscall_init` 初始化
3. 在 `struct thread` 里加 `fd_table[128]` 和 `fd_next`（初始=2）
4. 逐一实现：`SYS_HALT`→`SYS_EXIT`→`SYS_CREATE`→`SYS_REMOVE`→`SYS_OPEN`→`SYS_CLOSE`→`SYS_FILESIZE`→`SYS_READ`→`SYS_WRITE`→`SYS_SEEK`→`SYS_TELL`
5. 每实现一组，`make check` 验证对应测试

---

### Task 4 — `exec` / `wait` 父子同步

**目标**：让 `exec-*`、`wait-*`、`multi-child-fd` 等通过。

这是最难的部分，建议先在纸上画清楚生命周期再写代码：

**数据结构设计**：
- 想清楚：子进程退出后父进程才 wait，父进程退出后子进程才退出，谁来 free？
- 答案：引用计数（`ref_count=2`），最后离开的那方 free

**步骤**：
1. 设计 `struct child_info`（至少需要：`tid`、`exit_status`、`sema`、`ref_count`、`waited`）
2. `process_execute`：分配 ci，传给子线程，`sema_down(&load_sema)` 等加载完
3. `start_process`：加载完后 `sema_up(&load_sema)` 通知父进程
4. `process_wait`：查找 ci，`sema_down` 等退出，读状态，ref_count--
5. `process_exit`：`sema_up` 通知父进程，对自己的 children 列表减引用

---

### Task 5 — Deny Write to Exec（最简单）

**目标**：让 `rox-*` 通过。

在 `load()` 的 `done:` 段 success 分支里：
```c
file_deny_write(file);
thread_current()->exec_file = file;
```
在 `process_exit()` 里：
```c
file_allow_write(cur->exec_file);
file_close(cur->exec_file);
```

---

### 建议的时间安排

| 任务 | 预计耗时 | 关键难点 |
|---|---|---|
| Task 1 参数传递 | 2-3小时 | 栈布局、字节序 |
| Task 2 进程退出 | 1小时 | 较简单 |
| Task 3 文件系统调用 | 3-4小时 | 锁、fd 表、check_ptr |
| Task 4 exec/wait | 4-6小时 | ref_count、sema 时序 |
| Task 5 写保护 | 30分钟 | 很简单 |

**最重要的一条**：Task 4 遇到困难时，在调试器里单步跟踪 `process_execute` → `start_process` → `process_exit` → `process_wait` 的完整路径，把每个 sem 值的变化打印出来，比盯着代码有用得多。

已进行更改。