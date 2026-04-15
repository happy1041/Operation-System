#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "userprog/process.h"
#include "devices/shutdown.h"
#include "devices/input.h"
#include "filesys/filesys.h"
#include "filesys/file.h"

#define STDIN_FILENO 0  /* 标准输入的文件描述符编号，保留，不可关闭/重分配 */
#define STDOUT_FILENO 1 /* 标准输出的文件描述符编号，保留，不可关闭/重分配 */

static void syscall_handler(struct intr_frame *);
static void check_ptr(const void *ptr);                /* 验证单个用户空间指针的合法性 */
static void check_buf(const void *buf, unsigned size); /* 逐页验证缓冲区每一页均已映射 */
static void check_str(const char *s);                  /* 逐页验证字符串，每页只调用一次 pagedir_get_page */
static int get_arg(struct intr_frame *f, int n);       /* 从用户栈取第 n 个 syscall 参数 */
static void syscall_exit(int status);

/* 全局文件系统锁：Pintos 文件系统不是线程安全的，
   所有 filesys_* / file_* 调用都必须持有此锁。 */
static struct lock filesys_lock;

void syscall_init(void)
{
  lock_init(&filesys_lock);
  intr_register_int(0x30, 3, INTR_ON, syscall_handler, "syscall");
}

/* 系统调用总入口：从用户栈读取 syscall 编号，分发到各 case。
   用户程序触发 int 0x30 中断后进入此函数，f->esp 指向用户栈顶。
   栈布局：[esp+0]=syscall_num, [esp+4]=arg0, [esp+8]=arg1, ... */
static void
syscall_handler(struct intr_frame *f)
{
  /* 验证 esp 本身及其后 3 字节（syscall_num 占 4 字节）均可访问 */
  check_ptr(f->esp);
  check_ptr((char *)f->esp + 3);
  int syscall_num = *(int *)f->esp;

  switch (syscall_num)
  {
  /* ── 进程控制 ── */
  case SYS_HALT:
    shutdown_power_off();
    break;

  case SYS_EXIT:
  {
    int status = get_arg(f, 0);
    syscall_exit(status);
    break;
  }

  case SYS_EXEC:
  {
    /* exec(cmd)：创建子进程执行 cmd 指代的程序。
       父进程会阻塞直到子进程完成 ELF 加载（成功或失败），
       加载失败时返回 -1（TID_ERROR），加载成功返回子进程 tid。
       注意：不等子进程退出，只等加载结束。 */
    const char *cmd = (const char *)get_arg(f, 0);
    check_str(cmd); /* 字符串可能跨页，需逐字节验证 */
    tid_t tid = process_execute(cmd);
    f->eax = tid;
    break;
  }

  case SYS_WAIT:
  {
    /* wait(tid)：等待指定子进程退出并返回其退出码。
       若 tid 不是直接子进程、已经 wait 过、或子进程被内核杀死，返回 -1。 */
    tid_t tid = (tid_t)get_arg(f, 0);
    f->eax = process_wait(tid);
    break;
  }

  /* ── 文件系统 ── */
  case SYS_CREATE:
  {
    /* create(name, size)：创建一个名为 name、初始大小为 size 字节的文件。
       文件创建后不会自动打开，需要再调用 open。成功返回 true。 */
    const char *name = (const char *)get_arg(f, 0);
    unsigned size = (unsigned)get_arg(f, 1);
    check_str(name);
    lock_acquire(&filesys_lock);
    f->eax = filesys_create(name, size);
    lock_release(&filesys_lock);
    break;
  }

  case SYS_REMOVE:
  {
    /* remove(name)：删除文件。即使文件正在被其他进程打开也可删除，
       但该文件在所有 fd 关闭前仍可继续读写（类似 Unix unlink）。 */
    const char *name = (const char *)get_arg(f, 0);
    check_str(name);
    lock_acquire(&filesys_lock);
    f->eax = filesys_remove(name);
    lock_release(&filesys_lock);
    break;
  }

  case SYS_OPEN:
  {
    /* open(name)：打开文件，返回文件描述符（fd >= 2）。
       fd 0/1 保留给 stdin/stdout，用户程序必须通过 fd_table 管理其他文件。
       fd_table 最多支持 128 项（fd 2..127），超出返回 -1。 */
    const char *name = (const char *)get_arg(f, 0);
    check_str(name);
    lock_acquire(&filesys_lock);
    struct file *file = filesys_open(name);
    lock_release(&filesys_lock);
    if (file == NULL)
    {
      f->eax = -1; /* 文件不存在或无法打开 */
      break;
    }
    struct thread *t = thread_current();
    if (t->fd_next >= 128)
    {
      file_close(file); /* fd 表已满，关闭文件防止泄漏 */
      f->eax = -1;
      break;
    }
    int fd = t->fd_next++; /* 分配 fd，单调递增（不复用已关闭的 fd） */
    t->fd_table[fd] = file;
    f->eax = fd;
    break;
  }

  case SYS_CLOSE:
  {
    /* close(fd)：关闭文件描述符。
       fd < 2（stdin/stdout）或无效 fd 视为错误，以 exit(-1) 终止进程。 */
    int fd = get_arg(f, 0);
    struct thread *t = thread_current();
    if (fd < 2 || fd >= 128 || t->fd_table[fd] == NULL)
      syscall_exit(-1);
    lock_acquire(&filesys_lock);
    file_close(t->fd_table[fd]);
    lock_release(&filesys_lock);
    t->fd_table[fd] = NULL; /* 清空槽位，避免悬空指针 */
    break;
  }

  case SYS_FILESIZE:
  {
    /* filesize(fd)：返回文件的字节数。fd 无效时返回 -1。 */
    int fd = get_arg(f, 0);
    struct thread *t = thread_current();
    if (fd < 2 || fd >= 128 || t->fd_table[fd] == NULL)
    {
      f->eax = -1;
      break;
    }
    lock_acquire(&filesys_lock);
    f->eax = file_length(t->fd_table[fd]);
    lock_release(&filesys_lock);
    break;
  }

  case SYS_READ:
  {
    /* read(fd, buf, size)：从 fd 读取最多 size 字节到 buf。
       fd == 0（stdin）：从键盘逐字节读取。
       其他 fd：从文件的当前位置读，读后文件位置自动前进。
       返回实际读取字节数，失败返回 -1。 */
    int fd = get_arg(f, 0);
    void *buf = (void *)get_arg(f, 1);
    unsigned sz = (unsigned)get_arg(f, 2);
    check_buf(buf, sz); /* 逐页验证缓冲区每一页均已映射 */
    if (fd == STDIN_FILENO)
    {
      uint8_t *b = buf;
      for (unsigned i = 0; i < sz; i++)
        b[i] = input_getc(); /* 从键盘驱动读单个字节（阻塞直到有按键） */
      f->eax = sz;
    }
    else
    {
      struct thread *t = thread_current();
      if (fd < 2 || fd >= 128 || t->fd_table[fd] == NULL)
      {
        f->eax = -1;
        break;
      }
      lock_acquire(&filesys_lock);
      f->eax = file_read(t->fd_table[fd], buf, sz);
      lock_release(&filesys_lock);
    }
    break;
  }

  case SYS_WRITE:
  {
    /* write(fd, buf, size)：将 buf 中 size 字节写入 fd。
       fd == 1（stdout）：用 putbuf 直接输出到控制台（VGA + 串口）。
       其他 fd：从文件的当前位置写，写后文件位置自动前进。
       返回实际写入字节数。 */
    int fd = get_arg(f, 0);
    void *buf = (void *)get_arg(f, 1);
    unsigned sz = (unsigned)get_arg(f, 2);
    check_buf(buf, sz); /* 逐页验证缓冲区每一页均已映射 */
    if (fd == STDOUT_FILENO)
    {
      putbuf(buf, sz); /* 内核输出函数，直接写控制台，无需加锁 */
      f->eax = sz;
    }
    else
    {
      struct thread *t = thread_current();
      if (fd < 2 || fd >= 128 || t->fd_table[fd] == NULL)
      {
        f->eax = -1;
        break;
      }
      lock_acquire(&filesys_lock);
      f->eax = file_write(t->fd_table[fd], buf, sz);
      lock_release(&filesys_lock);
    }
    break;
  }

  case SYS_SEEK:
  {
    /* seek(fd, pos)：将文件 fd 的读写位置（文件位置指针）设置为距文件开头 pos 字节处。
       pos = 0 表示跳到文件开头，pos = filesize 表示跳到文件末尾（之后 read 返回 0）。
       允许 seek 到超过文件末尾的位置，此后 write 会在中间形成"空洞"（read 返回 0 字节）。
       不返回值（void），fd 无效时静默忽略。 */
    int fd = get_arg(f, 0);
    unsigned pos = (unsigned)get_arg(f, 1);
    struct thread *t = thread_current();
    if (fd < 2 || fd >= 128 || t->fd_table[fd] == NULL)
      break; /* 无效 fd，静默忽略 */
    lock_acquire(&filesys_lock);
    file_seek(t->fd_table[fd], pos);
    lock_release(&filesys_lock);
    break;
  }

  case SYS_TELL:
  {
    /* tell(fd)：返回文件 fd 当前读写位置（相对于文件开头的字节偏移）。
       配合 seek 使用，可以保存/恢复文件位置。fd 无效时返回 0。 */
    int fd = get_arg(f, 0);
    struct thread *t = thread_current();
    if (fd < 2 || fd >= 128 || t->fd_table[fd] == NULL)
    {
      f->eax = 0;
      break;
    }
    lock_acquire(&filesys_lock);
    f->eax = file_tell(t->fd_table[fd]);
    lock_release(&filesys_lock);
    break;
  }

  default:
    syscall_exit(-1);
  }
}

/* check_ptr - 验证用户空间指针的合法性。
   若指针满足以下任一条件则直接以 -1 退出当前进程：
     1. 为 NULL；
     2. 不属于用户虚拟地址空间（高于 PHYS_BASE）；
     3. 在当前进程的页目录中没有对应的物理页（未映射）。
   这是防止用户程序通过非法指针读写内核内存的第一道防线。 */
static void
check_ptr(const void *ptr)
{
  if (ptr == NULL || !is_user_vaddr(ptr) ||
      pagedir_get_page(thread_current()->pagedir, ptr) == NULL)
    syscall_exit(-1);
}

/* check_buf - 验证用户空间缓冲区 [buf, buf+size) 所覆盖的每一页均已映射。
   先验证起始地址，再逐一检查页边界，最后验证末字节，确保跨越多页
   的缓冲区中间页不被漏检。size==0 时仅验证起始指针本身。 */
static void
check_buf(const void *buf, unsigned size)
{
  const uint8_t *p = (const uint8_t *)buf;
  check_ptr(p); /* 验证第一页 */
  /* 从第一个页边界起逐页检查：条件 next < p+size 保证凡缓冲区涉及的页都被命中，
     无需单独检查末字节（末页的页边界已被循环覆盖，或与第一页相同） */
  uintptr_t next = (((uintptr_t)p) & ~(uintptr_t)(PGSIZE - 1)) + PGSIZE;
  for (; next < (uintptr_t)p + size; next += PGSIZE)
    check_ptr((const void *)next);
}

/* check_str - 逐页验证以 NUL 结尾的字符串。
   每到达一个新页时调用一次 check_ptr，然后在页内线性扫描 '\0'，
   从而将 pagedir_get_page 调用次数从 O(字节数) 降低到 O(页数)。 */
static void
check_str(const char *s)
{
  while (true)
  {
    check_ptr(s); /* 验证当前页是否已映射 */
    /* 在本页内扫描 '\0'，不再逐字节调用 check_ptr */
    const char *page_end = (const char *)((((uintptr_t)s) & ~(uintptr_t)(PGSIZE - 1)) + PGSIZE);
    while (s < page_end)
    {
      if (*s == '\0')
        return;
      s++;
    }
    /* s 现已指向下一页开头，外层循环继续验证 */
  }
}

/* get_arg - 从中断帧中读取第 n 个（0-based）syscall 参数。
   x86 syscall 调用约定：用户程序用 pushl 依次压入参数，再 int $0x30。
   栈布局：[esp+0]=syscall 号, [esp+4]=arg0, [esp+8]=arg1, ...
   这里将 esp 当作 int* 使用，第 n 个参数在 esp[n+1]。
   先验证指针的 4 个字节均合法，再解引用返回值。 */
static int
get_arg(struct intr_frame *f, int n)
{
  int *arg_ptr = (int *)f->esp + n + 1;
  check_ptr(arg_ptr);
  check_ptr((char *)arg_ptr + 3); /* 确保 4 字节整数不跨页越界 */
  return *arg_ptr;
}

/* syscall_exit - 统一的进程退出入口（内部使用）。
   将 exit_status 写入线程结构后调用 thread_exit()，
   thread_exit() 最终会触发 process_exit() 完成资源清理和父子同步。 */
static void
syscall_exit(int status)
{
  thread_current()->exit_status = status;
  thread_exit();
}