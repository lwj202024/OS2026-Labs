#include <pmm.h>
#include <slub.h>
#include <stdio.h>
#include <string.h>

// A deliberately small SLUB-like allocator.  The first layer obtains pages
// from the current pmm_manager; the second layer slices each page into
// fixed-size objects and keeps a per-cache free list.

#define SLUB_NR_CLASSES 8
#define SLUB_MAX_SLABS  64

struct slub_slab {
    struct Page *page;
    size_t objsize;
    void *free_list;
    unsigned int inuse;
    unsigned int total;
    struct slub_slab *next;
};

static const size_t slub_sizes[SLUB_NR_CLASSES] = {
    16, 32, 64, 128, 256, 512, 1024, 2048
};

static struct slub_slab slub_slabs[SLUB_MAX_SLABS];
static struct slub_slab *slub_classes[SLUB_NR_CLASSES];

static int
slub_class_for(size_t size) {
    for (int i = 0; i < SLUB_NR_CLASSES; i ++) {
        if (size <= slub_sizes[i]) {
            return i;
        }
    }
    return -1;
}

static char *
slub_slab_base(struct slub_slab *slab) {
    return (char *)(page2pa(slab->page) + va_pa_offset);
}

static struct slub_slab *
slub_new_slab(int class_index) {
    struct slub_slab *slab = NULL;
    for (int i = 0; i < SLUB_MAX_SLABS; i ++) {
        if (slub_slabs[i].page == NULL) {
            slab = &slub_slabs[i];
            break;
        }
    }
    if (slab == NULL) {
        return NULL;
    }

    struct Page *page = alloc_pages(1);
    if (page == NULL) {
        return NULL;
    }

    size_t objsize = slub_sizes[class_index];
    slab->page = page;
    slab->objsize = objsize;
    slab->free_list = NULL;
    slab->inuse = 0;
    slab->total = PGSIZE / objsize;

    char *base = slub_slab_base(slab);
    for (char *p = base; p + objsize <= base + PGSIZE; p += objsize) {
        *(void **)p = slab->free_list;
        slab->free_list = p;
    }

    slab->next = slub_classes[class_index];
    slub_classes[class_index] = slab;
    return slab;
}

void
slub_init(void) {
    for (int i = 0; i < SLUB_NR_CLASSES; i ++) {
        slub_classes[i] = NULL;
    }
    for (int i = 0; i < SLUB_MAX_SLABS; i ++) {
        slub_slabs[i].page = NULL;
        slub_slabs[i].objsize = 0;
        slub_slabs[i].free_list = NULL;
        slub_slabs[i].inuse = 0;
        slub_slabs[i].total = 0;
        slub_slabs[i].next = NULL;
    }
}

void *
slub_alloc(size_t size) {
    if (size == 0) {
        return NULL;
    }
    int class_index = slub_class_for(size);
    if (class_index < 0) {
        return NULL;
    }

    struct slub_slab *slab = slub_classes[class_index];
    while (slab != NULL && slab->free_list == NULL) {
        slab = slab->next;
    }
    if (slab == NULL) {
        slab = slub_new_slab(class_index);
        if (slab == NULL) {
            return NULL;
        }
    }

    void *object = slab->free_list;
    slab->free_list = *(void **)object;
    slab->inuse ++;
    return object;
}

void
slub_free(void *ptr) {
    if (ptr == NULL) {
        return;
    }
    for (int c = 0; c < SLUB_NR_CLASSES; c ++) {
        struct slub_slab *prev = NULL;
        struct slub_slab *slab = slub_classes[c];
        while (slab != NULL) {
            char *base = slub_slab_base(slab);
            char *p = (char *)ptr;
            if (p >= base && p < base + PGSIZE &&
                (size_t)(p - base) % slab->objsize == 0) {
                *(void **)ptr = slab->free_list;
                slab->free_list = ptr;
                assert(slab->inuse > 0);
                slab->inuse --;

                if (slab->inuse == 0) {
                    // Return an empty slab to the page allocator.
                    if (prev == NULL) {
                        slub_classes[c] = slab->next;
                    } else {
                        prev->next = slab->next;
                    }
                    struct Page *page = slab->page;
                    slab->page = NULL;
                    slab->free_list = NULL;
                    slab->next = NULL;
                    free_pages(page, 1);
                }
                return;
            }
            prev = slab;
            slab = slab->next;
        }
    }
    panic("slub_free: pointer does not belong to any slab");
}

// Verify internal consistency of every non-empty slab list.
static void
slub_check(void) {
    for (int c = 0; c < SLUB_NR_CLASSES; c ++) {
        struct slub_slab *slab = slub_classes[c];
        while (slab != NULL) {
            assert(slab->page != NULL);
            assert(slab->objsize == slub_sizes[c]);
            assert(slab->inuse <= slab->total);
            slab = slab->next;
        }
    }
}

void
slub_self_test(void) {
    const int per_class = 8;
    void *objects[SLUB_NR_CLASSES][8];
    size_t baseline = nr_free_pages();

    for (int c = 0; c < SLUB_NR_CLASSES; c ++) {
        for (int i = 0; i < per_class; i ++) {
            objects[c][i] = slub_alloc(slub_sizes[c]);
            assert(objects[c][i] != NULL);
            assert(((uintptr_t)objects[c][i] % slub_sizes[c]) == 0);
            memset(objects[c][i], 0x5a + c, slub_sizes[c]);
        }
    }
    slub_check();

    // All objects must remain distinct and keep their contents.
    for (int c = 0; c < SLUB_NR_CLASSES; c ++) {
        for (int i = 0; i < per_class; i ++) {
            unsigned char *p = (unsigned char *)objects[c][i];
            for (size_t k = 0; k < slub_sizes[c]; k ++) {
                assert(p[k] == (unsigned char)(0x5a + c));
            }
            for (int j = i + 1; j < per_class; j ++) {
                assert(objects[c][i] != objects[c][j]);
            }
        }
    }

    for (int c = 0; c < SLUB_NR_CLASSES; c ++) {
        for (int i = 0; i < per_class; i ++) {
            slub_free(objects[c][i]);
        }
    }
    slub_check();
    assert(nr_free_pages() == baseline);
    assert(slub_alloc(4096) == NULL);

    cprintf("[SLUB] self-test PASS\n");
}
