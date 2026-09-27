## 概述

从源码看，folio 通过 **“首页 + 尾页”** 把若干 `struct page` 组织成一个管理对象。以 `order = 2` 为例，它包含 `2² = 4` 个基础页：

```text
folio
  │
  ├─ page[0]  首页：__SetPageHead(page)->PG_head；存放 mapping、index、引用计数等信息
  ├─ page[1]  尾页：page->compound_head ─┐
  ├─ page[2]  尾页：page->compound_head ─┼──→ page[0]
  └─ page[3]  尾页：page->compound_head ─┘
```

**1. 分配并建立首页、尾页。** `__folio_alloc_noprof()` 带着 `__GFP_COMP` 向页分配器申请 `order` 阶内存。对于非零阶分配，`prep_compound_page()` 给首页设置 `PG_head`，逐个初始化尾页，并记录阶数；尾页的 `compound_head` 指回首页。对于零阶分配，则不设置`PG_head`。

**2. 用首页表示整个 folio。** `struct folio` 与首页的 `struct page` 重叠布局；部分大 folio 的附加信息还复用尾页描述符中的位置。`folio_order()` 和 `folio_nr_pages()` 通过访问器给出范围大小。`order = 0` 时没有尾页，`folio_nr_pages()` 返回 1。

**3. 在整体与子页之间转换。** `page_folio(page)` 先找到 compound 首页，再把它作为 folio；`folio_page(folio, n)` 则返回第 `n` 个 `struct page`。所以即使传入的是 `page[2]`，也能找到管理这四个 pages 的同一个 folio。

**4. 按 folio 管理，按需访问子页。** 例如，`folio_ref_count()` 读取首页的引用计数；若只需某个文件索引对应的基础页，则调用 `folio_file_page()`。在页缓存中，folio->index 表示这个 folio 覆盖的第一个文件页索引，单位是 PAGE_SIZE，不是字节地址，也不是物理页帧号。插入页缓存时，内核设置 folio->mapping 和 folio->index，并以该索引和 folio 的 order 把它放入 mapping->i_pages。
`i_pages` 可以理解为**一个文件的页缓存索引表**。它是 `struct address_space` 里的 `struct xarray`：键是文件页索引，条目通常指向覆盖该索引的 folio。插入大 folio 时，页缓存用**起始索引和 order** 将它存入 `i_pages`，所以多个连续文件页索引可以查到同一个 folio。查找时，`filemap_get_entry()` 按索引从 XArray 加载条目，并为查到的 folio 取得引用。

`i_pages` 也可能含有**已回收 folio 的 shadow 条目**，或 shmem/tmpfs 的 swap 条目；所以查找结果需要判断类型。它保存的是缓存对象的索引关系，文件数据则在 folio 管理的物理页中。[filemap.c:1899](/Users/jinqinghui/linux-study/linux/mm/filemap.c:1899)
因此，`folio` 给一组连续 pages 一个共同的管理入口，同时保留对组内每个 `struct page` 的访问能力。
