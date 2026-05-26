#include "vm/frame.h"
#include <debug.h>
#include <list.h>
#include <stddef.h>
#include <string.h>
#include "threads/malloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "vm/page.h"
#include "vm/swap.h"

/* frame table 中的每一项都描述一个当前驻留的用户页框。 */
struct frame_entry
{
    void *kpage;           /* 物理页框对应的内核地址。 */
    void *upage;           /* 该页框当前映射到的用户虚页。 */
    struct thread *owner;  /* 当前拥有该页框的线程。 */
    bool pinned;           /* 被内核临时锁定时不可淘汰。 */
    bool busy;             /* 正在 fault/evict 过程中，避免并发修改。 */
    struct list_elem elem; /* frame table 链表节点。 */
};

static struct list frame_table;
static struct lock frame_lock;
static struct list_elem *clock_hand;

static struct frame_entry *frame_find(struct thread *owner, void *upage);
static struct frame_entry *frame_evict(enum palloc_flags flags, void *upage);
static struct frame_entry *frame_pick_victim(void);
static struct frame_entry *frame_create_entry(void *kpage, void *upage);
static struct list_elem *frame_next_clock(void);

/* 初始化全局 frame table 和时钟指针。 */
void frame_init(void)
{
    list_init(&frame_table);
    lock_init(&frame_lock);
    clock_hand = NULL;
}

void *
frame_allocate(enum palloc_flags flags, void *upage)
{
    struct frame_entry *entry;
    void *kpage;

    ASSERT(flags & PAL_USER);

    /* palloc 自身有内部池锁，这里不需要持 frame_lock；
       只有挂入 frame_table 时才短暂加锁，避免在分配期间阻塞其他 fault。 */
    kpage = palloc_get_page(flags);
    if (kpage != NULL)
    {
        entry = frame_create_entry(kpage, upage);
        if (entry == NULL)
        {
            palloc_free_page(kpage);
            return NULL;
        }

        lock_acquire(&frame_lock);
        list_push_back(&frame_table, &entry->elem);
        if (clock_hand == NULL)
            clock_hand = list_begin(&frame_table);
        lock_release(&frame_lock);
        return kpage;
    }

    /* 没有空闲页框时，使用全局 clock 算法挑选 victim。 */
    lock_acquire(&frame_lock);
    entry = frame_evict(flags, upage);
    lock_release(&frame_lock);
    if (entry == NULL)
        return NULL;

    return entry->kpage;
}

/* 释放一个页框；若该页框仍在 frame table 中，则同步删除其表项。 */
void frame_free(void *kpage)
{
    struct list_elem *elem;
    struct list_elem *next;

    if (kpage == NULL)
        return;

    /* 若该页框仍登记在 frame table 中，需要先删元数据再释放物理页。 */
    lock_acquire(&frame_lock);
    for (elem = list_begin(&frame_table); elem != list_end(&frame_table);
         elem = next)
    {
        struct frame_entry *entry = list_entry(elem, struct frame_entry, elem);
        next = list_next(elem);
        if (entry->kpage == kpage)
        {
            /* 先推进 clock_hand 再删节点，避免时钟指针指向已释放的内存。 */
            if (clock_hand == elem)
                clock_hand = next == list_end(&frame_table) ? list_begin(&frame_table) : next;
            list_remove(elem);
            lock_release(&frame_lock);
            palloc_free_page(kpage);
            free(entry);
            return;
        }
    }
    lock_release(&frame_lock);

    palloc_free_page(kpage);
}

/* 将当前线程某个用户页对应的 frame 标记为 pinned。 */
bool frame_pin(void *upage)
{
    struct frame_entry *entry;

    lock_acquire(&frame_lock);
    entry = frame_find(thread_current(), pg_round_down(upage));
    if (entry != NULL)
        entry->pinned = true;
    lock_release(&frame_lock);

    return entry != NULL;
}

/* 撤销 frame_pin() 的效果，使该页框重新允许被淘汰。 */
void frame_unpin(void *upage)
{
    struct frame_entry *entry;

    lock_acquire(&frame_lock);
    entry = frame_find(thread_current(), pg_round_down(upage));
    if (entry != NULL)
        entry->pinned = false;
    lock_release(&frame_lock);
}

/* 进程退出时删除其全部 frame table 表项。 */
void frame_release_process(struct thread *owner)
{
    struct list_elem *elem;
    struct list_elem *next;

    /* 进程退出时只删除 frame table 记录，物理页释放仍由 pagedir 销毁路径完成。 */
    lock_acquire(&frame_lock);
    for (elem = list_begin(&frame_table); elem != list_end(&frame_table); elem = next)
    {
        struct frame_entry *entry = list_entry(elem, struct frame_entry, elem);
        next = list_next(elem);
        if (entry->owner == owner)
        {
            /* 同 frame_free：先推进再删，防止时钟指针悬空。 */
            if (clock_hand == elem)
                clock_hand = next == list_end(&frame_table) ? list_begin(&frame_table) : next;
            list_remove(elem);
            free(entry);
        }
    }
    lock_release(&frame_lock);
}

/* 选择一个 victim 页框并将其内容迁移到文件或 swap，随后复用该页框。 */
static struct frame_entry *
frame_evict(enum palloc_flags flags, void *upage)
{
    struct frame_entry *victim;
    struct vm_page *page;
    bool dirty;

    /* 先用 clock 算法选 victim，再把其内容转移到合适的后备存储。 */
    victim = frame_pick_victim();
    if (victim == NULL)
        return NULL;

    page = page_lookup(&victim->owner->spt, victim->upage);
    if (page == NULL)
        return NULL;

    victim->busy = true;
    victim->pinned = true;
    page->busy = true;

    dirty = pagedir_is_dirty(victim->owner->pagedir, victim->upage);
    pagedir_clear_page(victim->owner->pagedir, victim->upage);

    /* 文件页且未修改时可以直接丢弃；其余情况需要写入 swap。 */
    if (!(page->type == VM_PAGE_FILE && !dirty))
    {
        page->swap_index = swap_out(victim->kpage);
        page->type = VM_PAGE_SWAP;
    }

    page->loaded = false;
    /* busy 在重新分配给新 owner 前清零：旧 owner 从此可以重新 fault 该页，
       而新 owner 尚未建立映射，不存在两者同时访问同一 frame 的窗口。 */
    page->busy = false;

    victim->owner = thread_current();
    victim->upage = pg_round_down(upage);
    victim->pinned = false;
    victim->busy = false;

    if (flags & PAL_ZERO)
        memset(victim->kpage, 0, PGSIZE);

    return victim;
}

/* 使用 clock 算法寻找一个当前可以被淘汰的页框。 */
static struct frame_entry *
frame_pick_victim(void)
{
    size_t scanned;
    size_t total = list_size(&frame_table);

    if (total == 0)
        return NULL;

    if (clock_hand == NULL || clock_hand == list_end(&frame_table))
        clock_hand = list_begin(&frame_table);

    /* 扫描至多两轮：第一次清 accessed，第二次挑真正未访问页。 */
    for (scanned = 0; scanned < total * 2; scanned++)
    {
        struct frame_entry *entry;
        struct vm_page *page;
        bool accessed;

        if (clock_hand == list_end(&frame_table))
            clock_hand = list_begin(&frame_table);

        entry = list_entry(clock_hand, struct frame_entry, elem);
        clock_hand = frame_next_clock();

        /* 被 pin 或正在处理中的页不能作为淘汰目标。 */
        if (entry->pinned || entry->busy)
            continue;

        page = page_lookup(&entry->owner->spt, entry->upage);
        if (page == NULL || page->busy)
            continue;

        accessed = pagedir_is_accessed(entry->owner->pagedir, entry->upage);
        if (accessed)
        {
            pagedir_set_accessed(entry->owner->pagedir, entry->upage, false);
            continue;
        }

        return entry;
    }

    return NULL;
}

/* 为一个新分配的物理页框创建 frame table 表项。 */
static struct frame_entry *
frame_create_entry(void *kpage, void *upage)
{
    struct frame_entry *entry = malloc(sizeof *entry);
    if (entry == NULL)
        return NULL;

    entry->kpage = kpage;
    entry->upage = pg_round_down(upage);
    entry->owner = thread_current();
    entry->pinned = false;
    entry->busy = false;
    return entry;
}

/* 将时钟指针移动到下一项，必要时在链表首尾之间循环。 */
static struct list_elem *
frame_next_clock(void)
{
    struct list_elem *next;

    if (list_empty(&frame_table))
        return NULL;

    if (clock_hand == NULL || clock_hand == list_end(&frame_table))
        return list_begin(&frame_table);

    next = list_next(clock_hand);
    return next == list_end(&frame_table) ? list_begin(&frame_table) : next;
}

/* 在线性 frame table 中查找某线程某用户页对应的页框表项。 */
static struct frame_entry *
frame_find(struct thread *owner, void *upage)
{
    struct list_elem *elem;

    for (elem = list_begin(&frame_table); elem != list_end(&frame_table);
         elem = list_next(elem))
    {
        struct frame_entry *entry = list_entry(elem, struct frame_entry, elem);
        if (entry->owner == owner && entry->upage == upage)
            return entry;
    }

    return NULL;
}
