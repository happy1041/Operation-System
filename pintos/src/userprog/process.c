#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "threads/synch.h"
#include "threads/malloc.h"

static thread_func start_process NO_RETURN;
static bool load(const char *cmdline, void (**eip)(void), void **esp);

/** Starts a new thread running a user program loaded from
   FILENAME.  The new thread may be scheduled (and may even exit)
   before process_execute() returns.  Returns the new process's
   thread id, or TID_ERROR if the thread cannot be created. */
tid_t process_execute(const char *file_name)
{
  char *fn_copy;
  tid_t tid;

  /* Make a copy of FILE_NAME.
     Otherwise there's a race between the caller and load(). */
  fn_copy = palloc_get_page(0);
  if (fn_copy == NULL)
    return TID_ERROR;
  strlcpy(fn_copy, file_name, PGSIZE);

  char *save_ptr;
  char name_copy[16];
  strlcpy(name_copy, file_name, sizeof name_copy);
  char *exe_name = strtok_r(name_copy, " ", &save_ptr);

  /* Allocate child_info shared between parent and child. */
  struct child_info *ci = malloc(sizeof(struct child_info));
  if (ci == NULL)
  {
    palloc_free_page(fn_copy);
    return TID_ERROR;
  }
  ci->exit_status = -1;
  ci->exited = false;
  ci->waited = false;
  sema_init(&ci->sema, 0);
  sema_init(&ci->load_sema, 0);
  ci->load_success = false;
  ci->ref_count = 2;

  /* 将 ci 指针藏在 fn_copy 页面的最后几个字节里，作为向子线程传递参数的"隐藏通道"。
     fn_copy 本身只用于存放命令行字符串（最长 PGSIZE），真正的命令不会占满一整页，
     因此可以安全地借用页面尾部的 sizeof(void*) 字节来传递 ci 指针，
     避免额外分配第二块内存或使用全局变量。 */
  *(struct child_info **)(fn_copy + PGSIZE - sizeof(struct child_info *)) = ci;

  /* Create a new thread to execute EXE_NAME. */
  tid = thread_create(exe_name, PRI_DEFAULT, start_process, fn_copy);
  if (tid == TID_ERROR)
  {
    palloc_free_page(fn_copy);
    free(ci);
    return TID_ERROR;
  }

  ci->tid = tid;
  list_push_back(&thread_current()->children, &ci->elem);

  /* 阻塞等待子进程加载完成（成功或失败）。
     子进程在 start_process() 中加载 ELF 后会 sema_up(&ci->load_sema) 唤醒这里。
     如果加载失败，父进程需要从 children 列表中移除该条目并回收内存。 */
  sema_down(&ci->load_sema);
  if (!ci->load_success)
  {
    list_remove(&ci->elem);
    ci->ref_count--; /* 父进程放弃引用 */
    if (ci->ref_count == 0)
      free(ci);
    return TID_ERROR;
  }

  return tid;
}

/** A thread function that loads a user process and starts it
   running. */
static void
start_process(void *file_name_)
{
  char *file_name = file_name_;
  struct intr_frame if_;
  bool success;

  /* Retrieve child_info pointer from the end of the page. */
  struct child_info *ci = *(struct child_info **)(file_name + PGSIZE - sizeof(struct child_info *));
  thread_current()->child_info = ci;

  /* Initialize interrupt frame and load executable. */
  memset(&if_, 0, sizeof if_);
  if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
  if_.cs = SEL_UCSEG;
  if_.eflags = FLAG_IF | FLAG_MBS;
  success = load(file_name, &if_.eip, &if_.esp);

  /* Signal parent: load succeeded or failed. */
  ci->load_success = success;
  sema_up(&ci->load_sema);

  /* If load failed, quit. */
  palloc_free_page(file_name);
  if (!success)
    thread_exit();

  /* Start the user process by simulating a return from an
     interrupt, implemented by intr_exit (in
     threads/intr-stubs.S).  Because intr_exit takes all of its
     arguments on the stack in the form of a `struct intr_frame',
     we just point the stack pointer (%esp) to our stack frame
     and jump to it. */
  asm volatile("movl %0, %%esp; jmp intr_exit" : : "g"(&if_) : "memory");
  NOT_REACHED();
}

/** Waits for thread TID to die and returns its exit status.  If
   it was terminated by the kernel (i.e. killed due to an
   exception), returns -1.  If TID is invalid or if it was not a
   child of the calling process, or if process_wait() has already
   been successfully called for the given TID, returns -1
   immediately, without waiting.

   This function will be implemented in problem 2-2.  For now, it
   does nothing. */
int process_wait(tid_t child_tid)
{
  struct thread *cur = thread_current();
  struct list_elem *e;
  struct child_info *ci = NULL;

  /* Find the child_info matching child_tid. */
  for (e = list_begin(&cur->children); e != list_end(&cur->children); e = list_next(e))
  {
    struct child_info *c = list_entry(e, struct child_info, elem);
    if (c->tid == child_tid)
    {
      ci = c;
      break;
    }
  }

  /* tid 无效、不是当前进程的子进程、或已经 wait 过，均返回 -1。
     waited 标志防止同一个子进程被 wait 两次。 */
  if (ci == NULL || ci->waited)
    return -1;

  ci->waited = true;
  sema_down(&ci->sema); /* 阻塞，直到子进程在 process_exit() 中 sema_up 唤醒 */
  int status = ci->exit_status;

  /* 父进程完成 wait 后放弃对 ci 的引用。
     此时子进程已退出（ref_count 已被子进程减过），若父进程是最后一个则 free。 */
  list_remove(&ci->elem);
  ci->ref_count--;
  if (ci->ref_count == 0)
    free(ci);

  return status;
}

/** Free the current process's resources. */
void process_exit(void)
{
  struct thread *cur = thread_current();
  uint32_t *pd;

  /* 通过 child_info 向父进程报告退出状态。
     将 exit_status 写入共享结构后 sema_up，唤醒可能正在 process_wait() 阻塞的父进程。
     随后子进程放弃对 ci 的引用；若父进程早已退出（ref_count 已为 1），则由子进程 free。 */
  struct child_info *ci = cur->child_info;
  if (ci != NULL)
  {
    ci->exit_status = cur->exit_status;
    ci->exited = true;
    sema_up(&ci->sema); /* 唤醒父进程的 process_wait() */
    ci->ref_count--;
    if (ci->ref_count == 0)
      free(ci);
  }

  /* 当前进程退出时，释放它对所有子进程 child_info 的引用。
     若子进程已经退出（ref_count 已被子进程减为 1），则由父进程负责 free；
     若子进程还未退出（ref_count 仍为 2），则将其减为 1，
     子进程退出时会发现自己是最后一个引用者并自行 free。
     以此确保无论父子谁先退出，child_info 都能被正确释放，无内存泄漏。 */
  struct list_elem *e = list_begin(&cur->children);
  while (e != list_end(&cur->children))
  {
    struct child_info *c = list_entry(e, struct child_info, elem);
    e = list_next(e);
    list_remove(&c->elem);
    c->ref_count--;
    if (c->ref_count == 0)
      free(c);
  }

  /* Destroy the current process's page directory and switch back
     to the kernel-only page directory. */
  pd = cur->pagedir;
  if (pd != NULL)
  {
    /* Close all open file descriptors. */
    for (int i = 2; i < 128; i++)
    {
      if (cur->fd_table[i] != NULL)
      {
        file_close(cur->fd_table[i]);
        cur->fd_table[i] = NULL;
      }
    }

    /* Task 5：进程退出时恢复对自身可执行文件的写权限并关闭文件句柄。
       exec_file 仅在 load() 成功后被设置；内核线程或加载失败的进程其值为 NULL。
       was_user_process 作为"是否需要打印退出信息"的标志：
       只有真正运行过用户代码的进程才需要输出 "process_name: exit(N)"。 */
    bool was_user_process = (cur->exec_file != NULL);
    if (cur->exec_file != NULL)
    {
      file_allow_write(cur->exec_file); /* 解除写保护，允许其他人重新写该 ELF 文件 */
      file_close(cur->exec_file);
      cur->exec_file = NULL;
    }

    /* 只有真正运行过用户代码的进程才打印退出信息（内核辅助线程不打印）。 */
    if (was_user_process)
      printf("%s: exit(%d)\n", cur->name, cur->exit_status);

    /* Correct ordering here is crucial.  We must set
       cur->pagedir to NULL before switching page directories,
       so that a timer interrupt can't switch back to the
       process page directory.  We must activate the base page
       directory before destroying the process's page
       directory, or our active page directory will be one
       that's been freed (and cleared). */
    cur->pagedir = NULL;
    pagedir_activate(NULL);
    pagedir_destroy(pd);
  }
}

/** Sets up the CPU for running user code in the current
   thread.
   This function is called on every context switch. */
void process_activate(void)
{
  struct thread *t = thread_current();

  /* Activate thread's page tables. */
  pagedir_activate(t->pagedir);

  /* Set thread's kernel stack for use in processing
     interrupts. */
  tss_update();
}

/** We load ELF binaries.  The following definitions are taken
   from the ELF specification, [ELF1], more-or-less verbatim.  */

/** ELF types.  See [ELF1] 1-2. */
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

/** For use with ELF types in printf(). */
#define PE32Wx PRIx32 /**< Print Elf32_Word in hexadecimal. */
#define PE32Ax PRIx32 /**< Print Elf32_Addr in hexadecimal. */
#define PE32Ox PRIx32 /**< Print Elf32_Off in hexadecimal. */
#define PE32Hx PRIx16 /**< Print Elf32_Half in hexadecimal. */

/** Executable header.  See [ELF1] 1-4 to 1-8.
   This appears at the very beginning of an ELF binary. */
struct Elf32_Ehdr
{
  unsigned char e_ident[16];
  Elf32_Half e_type;
  Elf32_Half e_machine;
  Elf32_Word e_version;
  Elf32_Addr e_entry;
  Elf32_Off e_phoff;
  Elf32_Off e_shoff;
  Elf32_Word e_flags;
  Elf32_Half e_ehsize;
  Elf32_Half e_phentsize;
  Elf32_Half e_phnum;
  Elf32_Half e_shentsize;
  Elf32_Half e_shnum;
  Elf32_Half e_shstrndx;
};

/** Program header.  See [ELF1] 2-2 to 2-4.
   There are e_phnum of these, starting at file offset e_phoff
   (see [ELF1] 1-6). */
struct Elf32_Phdr
{
  Elf32_Word p_type;
  Elf32_Off p_offset;
  Elf32_Addr p_vaddr;
  Elf32_Addr p_paddr;
  Elf32_Word p_filesz;
  Elf32_Word p_memsz;
  Elf32_Word p_flags;
  Elf32_Word p_align;
};

/** Values for p_type.  See [ELF1] 2-3. */
#define PT_NULL 0           /**< Ignore. */
#define PT_LOAD 1           /**< Loadable segment. */
#define PT_DYNAMIC 2        /**< Dynamic linking info. */
#define PT_INTERP 3         /**< Name of dynamic loader. */
#define PT_NOTE 4           /**< Auxiliary info. */
#define PT_SHLIB 5          /**< Reserved. */
#define PT_PHDR 6           /**< Program header table. */
#define PT_STACK 0x6474e551 /**< Stack segment. */

/** Flags for p_flags.  See [ELF3] 2-3 and 2-4. */
#define PF_X 1 /**< Executable. */
#define PF_W 2 /**< Writable. */
#define PF_R 4 /**< Readable. */

static bool setup_stack(void **esp, const char *cmd_line);
static bool validate_segment(const struct Elf32_Phdr *, struct file *);
static bool load_segment(struct file *file, off_t ofs, uint8_t *upage,
                         uint32_t read_bytes, uint32_t zero_bytes,
                         bool writable);

/** Loads an ELF executable from FILE_NAME into the current thread.
   Stores the executable's entry point into *EIP
   and its initial stack pointer into *ESP.
   Returns true if successful, false otherwise. */
bool load(const char *file_name, void (**eip)(void), void **esp)
{
  char *save_ptr;
  char cmd_copy[256];
  strlcpy(cmd_copy, file_name, sizeof cmd_copy);
  char *exe_name = strtok_r(cmd_copy, " ", &save_ptr);

  struct thread *t = thread_current();
  struct Elf32_Ehdr ehdr;
  struct file *file = NULL;
  off_t file_ofs;
  bool success = false;
  int i;

  /* Allocate and activate page directory. */
  t->pagedir = pagedir_create();
  if (t->pagedir == NULL)
    goto done;
  process_activate();

  /* Open executable file. */
  file = filesys_open(exe_name);
  if (file == NULL)
  {
    printf("load: %s: open failed\n", exe_name);
    goto done;
  }

  /* Read and verify executable header. */
  if (file_read(file, &ehdr, sizeof ehdr) != sizeof ehdr || memcmp(ehdr.e_ident, "\177ELF\1\1\1", 7) || ehdr.e_type != 2 || ehdr.e_machine != 3 || ehdr.e_version != 1 || ehdr.e_phentsize != sizeof(struct Elf32_Phdr) || ehdr.e_phnum > 1024)
  {
    printf("load: %s: error loading executable\n", exe_name);
    goto done;
  }

  /* Read program headers. */
  file_ofs = ehdr.e_phoff;
  for (i = 0; i < ehdr.e_phnum; i++)
  {
    struct Elf32_Phdr phdr;

    if (file_ofs < 0 || file_ofs > file_length(file))
      goto done;
    file_seek(file, file_ofs);

    if (file_read(file, &phdr, sizeof phdr) != sizeof phdr)
      goto done;
    file_ofs += sizeof phdr;
    switch (phdr.p_type)
    {
    case PT_NULL:
    case PT_NOTE:
    case PT_PHDR:
    case PT_STACK:
    default:
      /* Ignore this segment. */
      break;
    case PT_DYNAMIC:
    case PT_INTERP:
    case PT_SHLIB:
      goto done;
    case PT_LOAD:
      if (validate_segment(&phdr, file))
      {
        bool writable = (phdr.p_flags & PF_W) != 0;
        uint32_t file_page = phdr.p_offset & ~PGMASK;
        uint32_t mem_page = phdr.p_vaddr & ~PGMASK;
        uint32_t page_offset = phdr.p_vaddr & PGMASK;
        uint32_t read_bytes, zero_bytes;
        if (phdr.p_filesz > 0)
        {
          /* Normal segment.
             Read initial part from disk and zero the rest. */
          read_bytes = page_offset + phdr.p_filesz;
          zero_bytes = (ROUND_UP(page_offset + phdr.p_memsz, PGSIZE) - read_bytes);
        }
        else
        {
          /* Entirely zero.
             Don't read anything from disk. */
          read_bytes = 0;
          zero_bytes = ROUND_UP(page_offset + phdr.p_memsz, PGSIZE);
        }
        if (!load_segment(file, file_page, (void *)mem_page,
                          read_bytes, zero_bytes, writable))
          goto done;
      }
      else
        goto done;
      break;
    }
  }

  /* Set up stack. */
  if (!setup_stack(esp, file_name))
    goto done;

  /* Start address. */
  *eip = (void (*)(void))ehdr.e_entry;

  success = true;

done:
  /* We arrive here whether the load is successful or not. */
  if (success)
  {
    /* Task 5：将可执行文件句柄保留在 exec_file 中，并调用 file_deny_write 禁止写入。
       这防止了进程在运行期间有人覆盖其正在执行的代码（类似 Linux 的 ETXTBSY）。
       file_deny_write / file_allow_write 是 Pintos 文件系统层的引用计数机制：
       每次 deny 使计数 +1，allow 使计数 -1，计数为 0 时才真正允许写入。
       文件句柄会在 process_exit() 中通过 file_allow_write + file_close 归还。 */
    file_deny_write(file);
    thread_current()->exec_file = file;
  }
  else
  {
    file_close(file); /* file 可能为 NULL（filesys_open 失败），file_close 对 NULL 安全 */
  }
  return success;
}

/** load() helpers. */

static bool install_page(void *upage, void *kpage, bool writable);

/** Checks whether PHDR describes a valid, loadable segment in
   FILE and returns true if so, false otherwise. */
static bool
validate_segment(const struct Elf32_Phdr *phdr, struct file *file)
{
  /* p_offset and p_vaddr must have the same page offset. */
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
    return false;

  /* p_offset must point within FILE. */
  if (phdr->p_offset > (Elf32_Off)file_length(file))
    return false;

  /* p_memsz must be at least as big as p_filesz. */
  if (phdr->p_memsz < phdr->p_filesz)
    return false;

  /* The segment must not be empty. */
  if (phdr->p_memsz == 0)
    return false;

  /* The virtual memory region must both start and end within the
     user address space range. */
  if (!is_user_vaddr((void *)phdr->p_vaddr))
    return false;
  if (!is_user_vaddr((void *)(phdr->p_vaddr + phdr->p_memsz)))
    return false;

  /* The region cannot "wrap around" across the kernel virtual
     address space. */
  if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
    return false;

  /* Disallow mapping page 0.
     Not only is it a bad idea to map page 0, but if we allowed
     it then user code that passed a null pointer to system calls
     could quite likely panic the kernel by way of null pointer
     assertions in memcpy(), etc. */
  if (phdr->p_vaddr < PGSIZE)
    return false;

  /* It's okay. */
  return true;
}

/** Loads a segment starting at offset OFS in FILE at address
   UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
   memory are initialized, as follows:

        - READ_BYTES bytes at UPAGE must be read from FILE
          starting at offset OFS.

        - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.

   The pages initialized by this function must be writable by the
   user process if WRITABLE is true, read-only otherwise.

   Return true if successful, false if a memory allocation error
   or disk read error occurs. */
static bool
load_segment(struct file *file, off_t ofs, uint8_t *upage,
             uint32_t read_bytes, uint32_t zero_bytes, bool writable)
{
  ASSERT((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT(pg_ofs(upage) == 0);
  ASSERT(ofs % PGSIZE == 0);

  file_seek(file, ofs);
  while (read_bytes > 0 || zero_bytes > 0)
  {
    /* Calculate how to fill this page.
       We will read PAGE_READ_BYTES bytes from FILE
       and zero the final PAGE_ZERO_BYTES bytes. */
    size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
    size_t page_zero_bytes = PGSIZE - page_read_bytes;

    /* Get a page of memory. */
    uint8_t *kpage = palloc_get_page(PAL_USER);
    if (kpage == NULL)
      return false;

    /* Load this page. */
    if (file_read(file, kpage, page_read_bytes) != (int)page_read_bytes)
    {
      palloc_free_page(kpage);
      return false;
    }
    memset(kpage + page_read_bytes, 0, page_zero_bytes);

    /* Add the page to the process's address space. */
    if (!install_page(upage, kpage, writable))
    {
      palloc_free_page(kpage);
      return false;
    }

    /* Advance. */
    read_bytes -= page_read_bytes;
    zero_bytes -= page_zero_bytes;
    upage += PGSIZE;
  }
  return true;
}

/** Create a minimal stack by mapping a zeroed page at the top of
   user virtual memory. */
static bool
setup_stack(void **esp, const char *cmd_line)
{
  uint8_t *kpage;
  kpage = palloc_get_page(PAL_USER | PAL_ZERO);
  if (kpage != NULL)
  {
    if (install_page(((uint8_t *)PHYS_BASE) - PGSIZE, kpage, true))
      *esp = PHYS_BASE;
    else
    {
      palloc_free_page(kpage);
      return false;
    }
  }

  /* 栈底地址：esp 不可低于此值，否则越过栈页边界。 */
  void *stack_bottom = (void *)((uint8_t *)PHYS_BASE - PGSIZE);

  char *copy = palloc_get_page(0);
  if (copy == NULL)
    return false;
  strlcpy(copy, cmd_line, PGSIZE);

  char *argv[64];
  int argc = 0;
  char *token, *save_ptr;
  for (token = strtok_r(copy, " ", &save_ptr); token != NULL; token = strtok_r(NULL, " ", &save_ptr))
  {
    if (argc >= 64)
      break;
    argv[argc++] = token;
  }

  /* Push arguments onto the stack in reverse order. */
  char *arg_addr[64];
  for (int i = argc - 1; i >= 0; i--)
  {
    size_t len = strlen(argv[i]) + 1;
    *esp -= len;
    if (*esp < stack_bottom)
    {
      palloc_free_page(copy);
      return false;
    }
    memcpy(*esp, argv[i], len);
    arg_addr[i] = *esp;
  }

  /* Word-align the stack pointer. */
  uintptr_t esp_uint = (uintptr_t)*esp;
  esp_uint &= 0xfffffffc;
  *esp = (void *)esp_uint;

  /* 计算剩余需要压栈的空间：
     (argc+1) 个指针 + argv + argc + 返回地址 */
  size_t remaining = (argc + 1) * sizeof(char *) + sizeof(char **) + sizeof(int) + sizeof(void *);
  if ((uintptr_t)*esp - remaining < (uintptr_t)stack_bottom)
  {
    palloc_free_page(copy);
    return false;
  }

  /* Push null sentinel. */
  *esp -= sizeof(char *);
  *(char **)*esp = NULL;

  /* Push argv pointers in reverse order. */
  for (int i = argc - 1; i >= 0; i--)
  {
    *esp -= sizeof(char *);
    *(char **)*esp = arg_addr[i];
  }

  /* Push argv (pointer to argv[0]). */
  char **argv_ptr = (char **)*esp;
  *esp -= sizeof(char **);
  *(char ***)*esp = argv_ptr;

  /* Push argc. */
  *esp -= sizeof(int);
  *(int *)*esp = argc;

  /* Push fake return address. */
  *esp -= sizeof(void *);
  *(void **)*esp = NULL;

  palloc_free_page(copy);
  return true;
}

/** Adds a mapping from user virtual address UPAGE to kernel
   virtual address KPAGE to the page table.
   If WRITABLE is true, the user process may modify the page;
   otherwise, it is read-only.
   UPAGE must not already be mapped.
   KPAGE should probably be a page obtained from the user pool
   with palloc_get_page().
   Returns true on success, false if UPAGE is already mapped or
   if memory allocation fails. */
static bool
install_page(void *upage, void *kpage, bool writable)
{
  struct thread *t = thread_current();

  /* Verify that there's not already a page at that virtual
     address, then map our page there. */
  return (pagedir_get_page(t->pagedir, upage) == NULL && pagedir_set_page(t->pagedir, upage, kpage, writable));
}
