#ifndef VM_PAGE_H
#define VM_PAGE_H

#include <stdbool.h>
#include <hash.h>
#include <stdint.h>
#include "filesys/file.h"

struct hash;

/* 补充页表中的页面类型：文件页、零页、栈页、换出页。 */
enum vm_page_type
{
    VM_PAGE_FILE,
    VM_PAGE_ZERO,
    VM_PAGE_STACK,
    VM_PAGE_SWAP
};

/* 描述一个用户虚页的后备来源与当前状态。 */
struct vm_page
{
    void *upage;            /* 用户虚页起始地址。 */
    enum vm_page_type type; /* 当前页面的后备来源类型。 */
    bool writable;          /* 该页是否允许用户写入。 */
    bool loaded;            /* 该页当前是否已装入内存。 */
    struct hash_elem elem;  /* SPT 哈希表节点。 */

    struct file *file;   /* 文件页对应的文件对象。 */
    off_t ofs;           /* 文件页在文件中的起始偏移。 */
    uint32_t read_bytes; /* 首次装载时需要读入的字节数。 */
    uint32_t zero_bytes; /* 首次装载时需要补零的字节数。 */
    size_t swap_index;   /* 换出后所在的 swap slot。 */
    bool busy;           /* 防止并发 fault/evict 重复操作同一页。 */
};

/* 初始化某个进程的补充页表哈希结构。 */
bool page_table_init(struct hash *spt);

/* 销毁某个进程的补充页表，并释放其关联的页面元数据。 */
void page_table_destroy(struct hash *spt);

/* 按用户虚页地址查找页元数据。 */
struct vm_page *page_lookup(struct hash *spt, const void *upage);

/* 在 SPT 中登记一个来自可执行文件的懒加载页面。 */
bool page_register_file(struct hash *spt, void *upage, struct file *file,
                        off_t ofs, uint32_t read_bytes, uint32_t zero_bytes,
                        bool writable);

/* 在 SPT 中登记一个栈页。 */
bool page_register_stack(struct hash *spt, void *upage, bool loaded);

/* 从 SPT 中删除一个页面条目。 */
void page_unregister(struct hash *spt, const void *upage);

/* 确保某个用户地址可访问，必要时触发补页或栈增长。 */
bool page_resolve(const void *uaddr, bool write);

/* 在补页成功后将该页对应的 frame 临时 pin 住。 */
bool page_resolve_and_pin(const void *uaddr, bool write);

/* 解除之前对页面对应 frame 的 pin。 */
void page_unpin(const void *uaddr);

/* 按 vm_page 描述真正把页面内容装入内存。 */
bool page_load(struct vm_page *page);

#endif /* vm/page.h */
