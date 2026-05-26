#include "vm/page.h"
#include <debug.h>
#include <hash.h>
#include <round.h>
#include <string.h>
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "userprog/syscall.h"
#include "vm/frame.h"
#include "vm/swap.h"

/* hash 回调：按用户虚页地址索引 SPT。 */
static unsigned page_hash(const struct hash_elem *elem, void *aux UNUSED);
static bool page_less(const struct hash_elem *a, const struct hash_elem *b,
                      void *aux UNUSED);
static void page_destroy_action(struct hash_elem *elem, void *aux UNUSED);
static bool page_should_grow_stack(const void *uaddr);

/* 初始化某个线程的补充页表。 */
bool page_table_init(struct hash *spt)
{
    return hash_init(spt, page_hash, page_less, NULL);
}

/* 销毁 SPT，并按页面类型回收附带资源。 */
void page_table_destroy(struct hash *spt)
{
    hash_destroy(spt, page_destroy_action);
}

/* 按页向下取整后做哈希查询。 */
struct vm_page *
page_lookup(struct hash *spt, const void *upage)
{
    struct vm_page lookup;
    struct hash_elem *elem;

    /* SPT 以页为粒度存储，查找前先向下取整。 */
    lookup.upage = pg_round_down(upage);
    elem = hash_find(spt, &lookup.elem);
    return elem == NULL ? NULL : hash_entry(elem, struct vm_page, elem);
}

/* 在 SPT 中登记一个文件后备页，供后续缺页时懒加载。 */
bool page_register_file(struct hash *spt, void *upage, struct file *file,
                        off_t ofs, uint32_t read_bytes, uint32_t zero_bytes,
                        bool writable)
{
    struct vm_page *page;

    ASSERT(pg_ofs(upage) == 0);
    ASSERT(read_bytes + zero_bytes == PGSIZE);

    page = malloc(sizeof *page);
    if (page == NULL)
        return false;

    /* 这里只登记懒加载元数据，不立刻分配物理页。 */
    page->upage = upage;
    page->type = read_bytes == 0 ? VM_PAGE_ZERO : VM_PAGE_FILE;
    page->writable = writable;
    page->loaded = false;
    page->file = file;
    page->ofs = ofs;
    page->read_bytes = read_bytes;
    page->zero_bytes = zero_bytes;
    page->swap_index = SIZE_MAX; /* SIZE_MAX 作哨兵：表示该页当前不在 swap 中。 */
    page->busy = false;

    if (hash_insert(spt, &page->elem) != NULL)
    {
        free(page);
        return false;
    }

    return true;
}

/* 在 SPT 中登记一个栈页条目。 */
bool page_register_stack(struct hash *spt, void *upage, bool loaded)
{
    struct vm_page *page;

    ASSERT(pg_ofs(upage) == 0);

    page = malloc(sizeof *page);
    if (page == NULL)
        return false;

    /* 栈页默认可写，内容首次使用时全零。 */
    page->upage = upage;
    page->type = VM_PAGE_STACK;
    page->writable = true;
    page->loaded = loaded;
    page->file = NULL;
    page->ofs = 0;
    page->read_bytes = 0;
    page->zero_bytes = PGSIZE;
    page->swap_index = SIZE_MAX; /* SIZE_MAX 作哨兵：栈页初始不在 swap 中。 */
    page->busy = false;

    if (hash_insert(spt, &page->elem) != NULL)
    {
        free(page);
        return false;
    }

    return true;
}

/* 从 SPT 中移除一个页面条目，但不主动回收其物理页框。 */
void page_unregister(struct hash *spt, const void *upage)
{
    struct vm_page lookup;
    struct hash_elem *elem;

    lookup.upage = pg_round_down(upage);
    elem = hash_delete(spt, &lookup.elem);
    if (elem != NULL)
        free(hash_entry(elem, struct vm_page, elem));
}

/* 确保给定用户地址可被当前线程安全访问。 */
bool page_resolve(const void *uaddr, bool write)
{
    struct thread *cur = thread_current();
    struct vm_page *page;

    /* 内核地址和空指针永远不属于合法用户访问。 */
    if (uaddr == NULL || !is_user_vaddr(uaddr))
        return false;

    page = page_lookup(&cur->spt, uaddr);
    if (write && page != NULL && !page->writable)
        return false;

    /* 若页面原本不存在，但访问位置满足栈增长规则，则先登记栈页。 */
    if (page == NULL && page_should_grow_stack(uaddr))
    {
        if (!page_register_stack(&cur->spt, pg_round_down(uaddr), false))
            return false;
        page = page_lookup(&cur->spt, uaddr);
    }

    /* 页表里已经有映射时，说明无需再触发补页。 */
    if (pagedir_get_page(cur->pagedir, uaddr) != NULL)
        return true;

    if (page == NULL || (write && !page->writable))
        return false;

    return page_load(page);
}

/* 在 page_resolve() 成功基础上，再把相关页框 pin 住。 */
bool page_resolve_and_pin(const void *uaddr, bool write)
{
    /* 先确保用户页可访问，再把对应 frame 锁住，避免 I/O 期间被淘汰。 */
    if (!page_resolve(uaddr, write))
        return false;

    return frame_pin(pg_round_down(uaddr));
}

/* 对之前 pin 住的页面执行解 pin。 */
void page_unpin(const void *uaddr)
{
    if (uaddr != NULL && is_user_vaddr(uaddr))
        frame_unpin(pg_round_down(uaddr));
}

/* 按 vm_page 描述把页面真正装入内存并建立页表映射。 */
bool page_load(struct vm_page *page)
{
    struct thread *cur = thread_current();
    uint8_t *kpage;

    ASSERT(page != NULL);

    /* 同一个页可能被多个 fault 同时命中，busy 用于串行化加载过程。 */
    while (page->busy)
        thread_yield();

    if (page->loaded)
        return true;

    page->busy = true;

    /* 非文件页默认以零页方式分配，文件页则稍后再填充内容。 */
    kpage = frame_allocate((page->type != VM_PAGE_FILE ? PAL_ZERO : 0) | PAL_USER,
                           page->upage);
    if (kpage == NULL)
    {
        page->busy = false;
        return false;
    }

    if (page->type == VM_PAGE_FILE)
    {
        bool ok;

        /* 文件页在真正 fault 时才去读可执行文件。 */
        filesys_acquire();
        file_seek(page->file, page->ofs);
        ok = file_read(page->file, kpage, page->read_bytes) == (int)page->read_bytes;
        filesys_release();
        if (!ok)
        {
            page->busy = false;
            frame_free(kpage);
            return false;
        }
        memset(kpage + page->read_bytes, 0, page->zero_bytes);
    }
    else if (page->type == VM_PAGE_SWAP)
    {
        /* swap_in 内部已经 bitmap_reset 归还了槽位，这里只清除过期引用。 */
        swap_in(page->swap_index, kpage);
        page->swap_index = SIZE_MAX;
    }

    /* 并发 fault 可能让另一个线程抢先安装了映射；发现已映射则放弃本次安装并回退。 */
    if (pagedir_get_page(cur->pagedir, page->upage) != NULL ||
        !pagedir_set_page(cur->pagedir, page->upage, kpage, page->writable))
    {
        page->busy = false;
        frame_free(kpage);
        return false;
    }

    page->loaded = true;
    page->busy = false;
    return true;
}

/* 计算某个 SPT 条目的哈希值，键为用户虚页地址。 */
static unsigned
page_hash(const struct hash_elem *elem, void *aux UNUSED)
{
    const struct vm_page *page = hash_entry(elem, struct vm_page, elem);
    return hash_bytes(&page->upage, sizeof page->upage);
}

/* 比较两个 SPT 条目在用户虚页地址上的大小关系。 */
static bool
page_less(const struct hash_elem *a, const struct hash_elem *b,
          void *aux UNUSED)
{
    const struct vm_page *page_a = hash_entry(a, struct vm_page, elem);
    const struct vm_page *page_b = hash_entry(b, struct vm_page, elem);
    return page_a->upage < page_b->upage;
}

/* hash_destroy() 回调：释放页面元数据，并回收仍占用的 swap 槽位。 */
static void
page_destroy_action(struct hash_elem *elem, void *aux UNUSED)
{
    struct vm_page *page = hash_entry(elem, struct vm_page, elem);
    /* FILE/ZERO/STACK 类型的页面 swap_index == SIZE_MAX，不占槽位，无需释放。
       只有被换出后仍未读回的页面（VM_PAGE_SWAP）才持有有效槽位。 */
    if (page->type == VM_PAGE_SWAP)
        swap_free(page->swap_index);
    free(page);
}

/* 判断一个尚未登记的地址是否应被解释为合法栈增长。 */
static bool
page_should_grow_stack(const void *uaddr)
{
    struct thread *cur = thread_current();
    const uint8_t *stack_bottom = (const uint8_t *)PHYS_BASE - 8 * 1024 * 1024;
    const uint8_t *esp = cur->saved_esp;
    const uint8_t *addr = uaddr;

    /* 没有可参考的用户栈顶时，不允许猜测性扩栈。 */
    if (esp == NULL)
        return false;

    /* 使用 Pintos 常见启发式：访问地址位于 esp 下方 32 字节内，且不超过 8 MB 栈界限。 */
    return addr >= stack_bottom && addr >= esp - 32;
}
