#ifndef __KERN_MM_SLUB_H__
#define __KERN_MM_SLUB_H__

#include <defs.h>

void slub_init(void);
void *slub_alloc(size_t size);
void slub_free(void *ptr);

// Allocate and release objects of several size classes, checking that the
// page allocator returns to its original free-page count afterwards.
void slub_self_test(void);

#endif /* !__KERN_MM_SLUB_H__ */
