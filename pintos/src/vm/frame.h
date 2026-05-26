#ifndef VM_FRAME_H
#define VM_FRAME_H

#include <stdbool.h>
#include "threads/palloc.h"

struct thread;

/* 初始化全局 frame table。 */
void frame_init(void);

/* 为某个用户虚页分配物理页框，必要时触发淘汰。 */
void *frame_allocate(enum palloc_flags flags, void *upage);

/* 释放指定物理页框及其 frame table 元数据。 */
void frame_free(void *kpage);

/* 在内核访问用户页期间暂时禁止该 frame 被淘汰。 */
bool frame_pin(void *upage);

/* 恢复某个用户页对应 frame 的可淘汰状态。 */
void frame_unpin(void *upage);

/* 进程退出时移除其全部 frame table 记录。 */
void frame_release_process(struct thread *owner);

#endif /* vm/frame.h */
