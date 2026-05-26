#include "vm/swap.h"
#include <bitmap.h>
#include <debug.h>
#include "devices/block.h"
#include "threads/synch.h"
#include "threads/vaddr.h"

#define SECTORS_PER_PAGE (PGSIZE / BLOCK_SECTOR_SIZE)

/* swap 分区本身、槽位位图以及并发访问锁。 */
static struct block *swap_block;
static struct bitmap *swap_map;
static struct lock swap_lock;

/* 将块设备中扮演 swap 角色的分区初始化为按页分配的槽位数组。 */
void swap_init(void)
{
    swap_block = block_get_role(BLOCK_SWAP);
    lock_init(&swap_lock);

    /* 某些配置下可能没有 swap 分区，后续使用者会据此拒绝换出。 */
    if (swap_block == NULL)
    {
        swap_map = NULL;
        return;
    }

    swap_map = bitmap_create(block_size(swap_block) / SECTORS_PER_PAGE);
    ASSERT(swap_map != NULL);
}

size_t
swap_out(const void *kpage)
{
    size_t slot;
    size_t i;

    ASSERT(swap_map != NULL);

    /* 持锁仅用于原子分配槽位；block_write 耗时较长，放在锁外以免阻塞其他换出。 */
    lock_acquire(&swap_lock);
    slot = bitmap_scan_and_flip(swap_map, 0, 1, false);
    lock_release(&swap_lock);
    if (slot == BITMAP_ERROR)
        PANIC("swap partition full");

    for (i = 0; i < SECTORS_PER_PAGE; i++)
        block_write(swap_block, slot * SECTORS_PER_PAGE + i,
                    (const uint8_t *)kpage + i * BLOCK_SECTOR_SIZE);

    return slot;
}

void swap_in(size_t slot, void *kpage)
{
    size_t i;

    ASSERT(swap_map != NULL);
    ASSERT(slot != BITMAP_ERROR);

    /* 按写出时的顺序逐扇区读回整个页面。 */
    for (i = 0; i < SECTORS_PER_PAGE; i++)
        block_read(swap_block, slot * SECTORS_PER_PAGE + i,
                   (uint8_t *)kpage + i * BLOCK_SECTOR_SIZE);

    /* 页面已经换回内存，对应槽位即可重新利用。 */
    lock_acquire(&swap_lock);
    bitmap_reset(swap_map, slot);
    lock_release(&swap_lock);
}

void swap_free(size_t slot)
{
    /* 对已经丢弃或无需再读回的 swap 页面显式归还槽位。 */
    if (swap_map == NULL || slot == BITMAP_ERROR)
        return;

    lock_acquire(&swap_lock);
    bitmap_reset(swap_map, slot);
    lock_release(&swap_lock);
}
