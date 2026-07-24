/*
 * MemPool.c -- multi-lcore-safe version
 *   CHANGE 1: rte_spinlock_init() in mpool_alloc
 *   CHANGE 2: lock/unlock around the list surgery in mpool_get and mpool_put
 *   CHANGE 3: mpool_put now UNLINKS the freed unit from its real position in
 *             the alloc list (old code assumed it was the head -> corruption,
 *             even single-threaded, whenever a non-head unit was freed)
 */
#include "malloc.h"
#include "MemPool.h"

void* mpool_alloc(struct MemPool *mpool, unsigned long ulUnitSize, unsigned long ulUnitNum)
{
	unsigned long i;

	mpool->m_pMemBlock      = NULL;
	mpool->m_pAllocMemBlock = NULL;
	mpool->m_pFreeMemBlock  = NULL;

	mpool->m_ulBlockSize = ulUnitNum * (ulUnitSize + sizeof(struct _unit));
	mpool->m_ulUnitSize  = ulUnitSize;

	mpool->m_pMemBlock = malloc(mpool->m_ulBlockSize);
	if (mpool->m_pMemBlock == NULL)
		return NULL;

	rte_spinlock_init(&mpool->m_lock);      /* CHANGE 1 */

	for (i = 0; i < ulUnitNum; i++) {
		struct _unit *pCurUnit = (struct _unit *)((char *)mpool->m_pMemBlock +
		                          i * (ulUnitSize + sizeof(struct _unit)));
		pCurUnit->pPrev = NULL;
		pCurUnit->pNext = mpool->m_pFreeMemBlock;
		if (mpool->m_pFreeMemBlock != NULL)
			mpool->m_pFreeMemBlock->pPrev = pCurUnit;
		mpool->m_pFreeMemBlock = pCurUnit;
	}

	mpool->m_ulLen_Freemem  = mpool->m_ulBlockSize;
	mpool->m_ulLen_Allocmem = 0;
	return mpool->m_pMemBlock;
}

void mpool_free(struct MemPool *mpool)
{
	free(mpool->m_pMemBlock);
}

void* mpool_get(struct MemPool *mpool, int iUseMemPool)
{
	struct _unit *pCurUnit;

	if (iUseMemPool == 0 || mpool->m_pMemBlock == NULL)
		return NULL;

	rte_spinlock_lock(&mpool->m_lock);       /* CHANGE 2 */

	if (mpool->m_pFreeMemBlock == NULL) {
		rte_spinlock_unlock(&mpool->m_lock);
		return NULL;
	}

	pCurUnit = mpool->m_pFreeMemBlock;
	mpool->m_pFreeMemBlock = pCurUnit->pNext;
	if (mpool->m_pFreeMemBlock != NULL)
		mpool->m_pFreeMemBlock->pPrev = NULL;

	pCurUnit->pPrev = NULL;
	pCurUnit->pNext = mpool->m_pAllocMemBlock;
	if (mpool->m_pAllocMemBlock != NULL)
		mpool->m_pAllocMemBlock->pPrev = pCurUnit;
	mpool->m_pAllocMemBlock = pCurUnit;

	mpool->m_ulLen_Allocmem += mpool->m_ulUnitSize;
	mpool->m_ulLen_Freemem  -= mpool->m_ulUnitSize;

	rte_spinlock_unlock(&mpool->m_lock);
	return (void *)((char *)pCurUnit + sizeof(struct _unit));
}

void mpool_put(struct MemPool *mpool, void* p)
{
	if (mpool->m_pMemBlock < p &&
	    p < (void *)((char *)mpool->m_pMemBlock + mpool->m_ulBlockSize)) {

		struct _unit *pCurUnit = (struct _unit *)((char *)p - sizeof(struct _unit));

		rte_spinlock_lock(&mpool->m_lock);   /* CHANGE 2 */

		/* CHANGE 3: unlink pCurUnit from wherever it really sits in the
		 * alloc list -- the old code did
		 *     m_pAllocMemBlock = pCurUnit->pNext
		 * unconditionally, which only works if it was the head. Otherwise
		 * the unit stayed linked in the alloc list AND got pushed onto the
		 * free list -- same buffer on both lists -> handed out twice. */
		if (pCurUnit->pPrev != NULL)
			pCurUnit->pPrev->pNext = pCurUnit->pNext;
		else
			mpool->m_pAllocMemBlock = pCurUnit->pNext;
		if (pCurUnit->pNext != NULL)
			pCurUnit->pNext->pPrev = pCurUnit->pPrev;

		pCurUnit->pPrev = NULL;
		pCurUnit->pNext = mpool->m_pFreeMemBlock;
		if (mpool->m_pFreeMemBlock != NULL)
			mpool->m_pFreeMemBlock->pPrev = pCurUnit;
		mpool->m_pFreeMemBlock = pCurUnit;

		mpool->m_ulLen_Freemem  += mpool->m_ulUnitSize;
		mpool->m_ulLen_Allocmem -= mpool->m_ulUnitSize;

		rte_spinlock_unlock(&mpool->m_lock);
	} else {
		free(p);
	}
}
