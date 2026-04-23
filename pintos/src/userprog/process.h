#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"
#include "threads/synch.h"

tid_t process_execute(const char *file_name);
int process_wait(tid_t);
void process_exit(void);
void process_activate(void);

/* struct child_info —— 父进程与子进程之间的共享控制块。
 *
 * 设计目标：让两者能够通讯来完成父进程等待子进程、获取子进程退出状态等功能.
 *
 * 同步机制：
 *   sema（初始值 0）：父进程调用 process_wait 时 sema_down，
 *                      子进程在 process_exit 中 sema_up 唤醒父进程。
 *   load_sema（初始值 0）：父进程在 process_execute 中 sema_down，
 *                           子进程加载完 ELF 后 sema_up 通知结果。
 */
struct child_info
{
    tid_t tid;
    int exit_status;
    bool exited;                /* 子进程是否已退出 */
    bool waited;                /* 父进程是否已 wait 过 */
    struct semaphore sema;      /* 初始为 0：父等子退出 */
    struct semaphore load_sema; /* 初始为 0：父等子加载完成 */
    bool load_success;          /* 子进程加载是否成功 */
    int ref_count;              /* 初始为 2，减到 0 时 free */
    struct list_elem elem;      /* 挂在父进程的 children 列表里 */
};
#endif /**< userprog/process.h */
