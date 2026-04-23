#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

void syscall_init(void);

/* 供 process.c 在 process_exit() 清理阶段使用。
   Pintos 文件系统不是线程安全的，所有文件系统调用都必须持有此锁。 */
void filesys_acquire(void);
void filesys_release(void);

#endif /**< userprog/syscall.h */
