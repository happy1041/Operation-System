#ifndef VM_SWAP_H
#define VM_SWAP_H

#include <stddef.h>

/* 初始化交换分区位图与锁。 */
void swap_init(void);

/* 将一个页框写出到 swap，返回其槽位编号。 */
size_t swap_out(const void *kpage);

/* 从指定 swap 槽位把页面读回页框。 */
void swap_in(size_t slot, void *kpage);

/* 主动释放不再使用的 swap 槽位。 */
void swap_free(size_t slot);

#endif /* vm/swap.h */
