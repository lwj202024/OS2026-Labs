#ifndef __KERN_MM_BUDDY_PMM_H__
#define __KERN_MM_BUDDY_PMM_H__

#include <pmm.h>

extern const struct pmm_manager buddy_pmm_manager;

// Run a self-test on a temporary region taken from the current pmm manager.
void buddy_self_test(void);

#endif /* !__KERN_MM_BUDDY_PMM_H__ */
