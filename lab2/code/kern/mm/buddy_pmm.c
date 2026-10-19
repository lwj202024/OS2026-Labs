#include <pmm.h>
#include <list.h>
#include <buddy_pmm.h>
#include <stdio.h>
#include <string.h>

// A compact buddy allocator built on the ucore Page descriptor array.
// The free lists are indexed by block order: order o contains blocks of
// 2^o pages.  Only the first page of a free block is linked into the list;
// its property field stores the block size in pages.

#define BUDDY_MAX_ORDER 12
#define BUDDY_MAX_BLOCK (1UL << BUDDY_MAX_ORDER)

static free_area_t buddy_free_area[BUDDY_MAX_ORDER + 1];
static size_t buddy_nr_free = 0;
static struct Page *buddy_region_start = NULL;
static struct Page *buddy_region_end = NULL;

static int
buddy_order_for(size_t n) {
    int order = 0;
    while ((1UL << order) < n) {
        order ++;
        if (order > BUDDY_MAX_ORDER) {
            return -1;
        }
    }
    return order;
}

static void
buddy_insert_block(struct Page *block, int order) {
    block->property = 1UL << order;
    SetPageProperty(block);
    list_add_before(&buddy_free_area[order].free_list, &(block->page_link));
    buddy_free_area[order].nr_free ++;
}

static void
buddy_init(void) {
    for (int i = 0; i <= BUDDY_MAX_ORDER; i ++) {
        list_init(&buddy_free_area[i].free_list);
        buddy_free_area[i].nr_free = 0;
    }
    buddy_nr_free = 0;
    buddy_region_start = NULL;
    buddy_region_end = NULL;
}

static void
buddy_init_memmap(struct Page *base, size_t n) {
    assert(n > 0);

    for (struct Page *p = base; p != base + n; p ++) {
        assert(PageReserved(p));
        p->flags = 0;
        p->property = 0;
        set_page_ref(p, 0);
    }

    if (buddy_region_start == NULL || base < buddy_region_start) {
        buddy_region_start = base;
    }
    if (buddy_region_end == NULL || base + n > buddy_region_end) {
        buddy_region_end = base + n;
    }

    // Decompose the initial region into naturally aligned power-of-two blocks.
    size_t offset = 0;
    while (offset < n) {
        size_t index = (base - pages) + offset;
        size_t remain = n - offset;
        int order = 0;
        while (order < BUDDY_MAX_ORDER &&
               (1UL << (order + 1)) <= remain &&
               (index & ((1UL << (order + 1)) - 1)) == 0) {
            order ++;
        }
        buddy_insert_block(base + offset, order);
        offset += 1UL << order;
    }
    buddy_nr_free += n;
}

static struct Page *
buddy_alloc_pages(size_t n) {
    assert(n > 0);
    if (n > buddy_nr_free) {
        return NULL;
    }

    int need = buddy_order_for(n);
    if (need < 0) {
        return NULL;
    }

    int order = need;
    while (order <= BUDDY_MAX_ORDER &&
           list_empty(&buddy_free_area[order].free_list)) {
        order ++;
    }
    if (order > BUDDY_MAX_ORDER) {
        return NULL;
    }

    list_entry_t *le = list_next(&buddy_free_area[order].free_list);
    struct Page *block = le2page(le, page_link);
    list_del(&(block->page_link));
    buddy_free_area[order].nr_free --;

    // Split larger blocks until the requested order is reached.
    while (order > need) {
        order --;
        size_t half = 1UL << order;
        struct Page *buddy = block + half;
        buddy->flags = 0;
        buddy_insert_block(buddy, order);
        block->property = half;
    }

    size_t alloc_size = 1UL << need;
    for (struct Page *p = block; p != block + alloc_size; p ++) {
        p->flags = 0;
        p->property = 0;
        set_page_ref(p, 0);
    }
    // Keep the allocated size in the head descriptor so free_pages() can
    // recover the buddy order even if the caller passes a smaller n.
    block->property = alloc_size;
    ClearPageProperty(block);
    buddy_nr_free -= alloc_size;
    return block;
}

static void
buddy_free_pages(struct Page *base, size_t n) {
    assert(n > 0);
    size_t alloc_size = base->property;
    if (alloc_size == 0) {
        int order = buddy_order_for(n);
        assert(order >= 0);
        alloc_size = 1UL << order;
    }
    int order = buddy_order_for(alloc_size);
    assert(order >= 0 && (1UL << order) == alloc_size);

    for (struct Page *p = base; p != base + alloc_size; p ++) {
        assert(!PageReserved(p));
        p->flags = 0;
        p->property = 0;
        set_page_ref(p, 0);
    }
    base->property = alloc_size;
    SetPageProperty(base);

    size_t size = alloc_size;
    while (order < BUDDY_MAX_ORDER) {
        size_t index = base - pages;
        size_t buddy_index = index ^ size;
        struct Page *buddy = pages + buddy_index;
        if (buddy < buddy_region_start || buddy >= buddy_region_end ||
            buddy + size > buddy_region_end) {
            break;
        }
        if (!PageProperty(buddy) || buddy->property != size) {
            break;
        }

        list_del(&(buddy->page_link));
        buddy_free_area[order].nr_free --;

        if (buddy < base) {
            ClearPageProperty(base);
            base = buddy;
        } else {
            ClearPageProperty(buddy);
        }
        order ++;
        size <<= 1;
        base->property = size;
        SetPageProperty(base);
    }

    list_add_before(&buddy_free_area[order].free_list, &(base->page_link));
    buddy_free_area[order].nr_free ++;
    buddy_nr_free += alloc_size;
}

static size_t
buddy_nr_free_pages(void) {
    return buddy_nr_free;
}

static void
buddy_check(void) {
    size_t total = 0;
    for (int order = 0; order <= BUDDY_MAX_ORDER; order ++) {
        size_t count = 0;
        list_entry_t *le = &buddy_free_area[order].free_list;
        while ((le = list_next(le)) != &buddy_free_area[order].free_list) {
            struct Page *page = le2page(le, page_link);
            assert(PageProperty(page));
            assert(page->property == (1UL << order));
            count ++;
            total += page->property;

            // Two free buddy blocks of the same order must have been merged.
            size_t size = 1UL << order;
            size_t buddy_index = (page - pages) ^ size;
            struct Page *buddy = pages + buddy_index;
            if (buddy >= buddy_region_start && buddy < buddy_region_end &&
                buddy + size <= buddy_region_end) {
                assert(!PageProperty(buddy) || buddy->property != size);
            }
        }
        assert(count == buddy_free_area[order].nr_free);
    }
    assert(total == buddy_nr_free);
}

const struct pmm_manager buddy_pmm_manager = {
    .name = "buddy_pmm_manager",
    .init = buddy_init,
    .init_memmap = buddy_init_memmap,
    .alloc_pages = buddy_alloc_pages,
    .free_pages = buddy_free_pages,
    .nr_free_pages = buddy_nr_free_pages,
    .check = buddy_check,
};

void
buddy_self_test(void) {
    const size_t test_pages = 64;
    struct Page *base = alloc_pages(test_pages);
    if (base == NULL) {
        cprintf("[Buddy] self-test FAIL (cannot reserve test region)\n");
        return;
    }

    // The region is owned by the current manager, but the buddy manager
    // expects its inputs to start in the reserved state.
    for (struct Page *p = base; p != base + test_pages; p ++) {
        SetPageReserved(p);
    }

    buddy_init();
    buddy_init_memmap(base, test_pages);
    assert(buddy_nr_free_pages() == test_pages);

    const size_t sizes[] = {1, 2, 4, 8, 16, 32};
    const int nr_blocks = sizeof(sizes) / sizeof(sizes[0]);
    struct Page *blocks[6];
    unsigned char used[64];

    memset(used, 0, sizeof(used));
    for (int i = 0; i < nr_blocks; i ++) {
        blocks[i] = buddy_alloc_pages(sizes[i]);
        assert(blocks[i] != NULL);

        size_t size = 1UL << buddy_order_for(sizes[i]);
        assert(((blocks[i] - pages) & (size - 1)) == 0);
        for (struct Page *p = blocks[i]; p != blocks[i] + size; p ++) {
            size_t idx = p - base;
            assert(idx < test_pages);
            assert(used[idx] == 0);
            used[idx] = 1;
        }
    }
    assert(buddy_nr_free_pages() + 63 == test_pages);

    // Free in reverse order, exercising both sides of the buddy merge.
    for (int i = nr_blocks - 1; i >= 0; i --) {
        size_t size = 1UL << buddy_order_for(sizes[i]);
        buddy_free_pages(blocks[i], sizes[i]);
    }
    assert(buddy_nr_free_pages() == test_pages);
    buddy_check();

    // Return the region to the real manager.  Reset the descriptors first
    // because the best-fit free routine expects clean allocated pages.
    for (struct Page *p = base; p != base + test_pages; p ++) {
        p->flags = 0;
        p->property = 0;
        set_page_ref(p, 0);
    }
    free_pages(base, test_pages);

    cprintf("[Buddy] self-test PASS\n");
}
