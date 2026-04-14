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

process.h 里写一下 exit_status, 以及父子进程关系的 struct child_process。