/*
 * MemPool.h -- multi-lcore-safe
 *   CHANGE: added rte_spinlock.h include + m_lock field
 */
#ifndef MEMPOOL_H_
#define MEMPOOL_H_

#include "stdbool.h"
#include <rte_spinlock.h>        /* per-pool lock */

struct MemPool
{
	struct _unit
	{
		struct _unit *pPrev;
		struct _unit *pNext;
	};

	void* m_pMemBlock;
	struct _unit* m_pAllocMemBlock;
	struct _unit* m_pFreeMemBlock;
	unsigned long m_ulUnitSize;
	unsigned long m_ulBlockSize;
	unsigned long m_ulLen_Allocmem;
	unsigned long m_ulLen_Freemem;
	rte_spinlock_t m_lock;          /* guards both linked lists */
};

void* mpool_alloc(struct MemPool *mpool, unsigned long ulUnitSize,
				  unsigned long ulUnitNum);
void  mpool_free(struct MemPool *mpool);
void* mpool_get(struct MemPool *mpool, int iUseMemPool);
void  mpool_put(struct MemPool *mpool, void* p);

#endif /* MEMPOOL_H_ */
