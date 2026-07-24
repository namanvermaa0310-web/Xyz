#include <stdio.h>
#include <string.h>

#include <rte_mempool.h>
#include <rte_errno.h>
#include <rte_lcore.h>
#include <rte_spinlock.h>

#include <global_main_var.h>
#include <main.h>

#include "module1.h"
#include "module2.h"
#include "module3.h"
#include "module4.h"
#include "cdc_engine.h"
#include "TCP/mbuf.h"
#include "TCP/MemPool.h"

static struct rte_mempool *g_chunk_pool = NULL;

static int chunk_alloc(struct chunk *c)
{
    void *obj;
    if (c->data != NULL) return 1;
    if (g_chunk_pool == NULL) { printf("chunk_alloc: pool not initialised\n"); return 0; }
    if (rte_mempool_get(g_chunk_pool, &obj) < 0) {
        printf("chunk_alloc: pool exhausted (%u in use)\n",
               rte_mempool_in_use_count(g_chunk_pool));
        return 0;
    }
    c->data = (uint8_t *)obj;
    c->bytes_used = 0;
    return 1;
}

static void chunk_free(struct chunk *c)
{
    if (c->data == NULL) return;
    rte_mempool_put(g_chunk_pool, (void *)c->data);
    c->data = NULL;
    c->bytes_used = 0;
}

/* INTERNAL: assumes s->lock already held */
static void process_chunk(struct module1_state *s, int chunk_type)
{
    uint8_t      digest[32];
    struct mbuf *pkt;

    if (s->current.bytes_used == 0) return;
    if (s->current.data == NULL) { s->current.bytes_used = 0; return; }

    module3_compute_hash(s->current.data, s->current.bytes_used, digest);
    pkt = module4_lookup_and_build_tlv(digest, s->current.data, s->current.bytes_used);

    if (pkt != NULL) {
        if (s->tlv_pending == NULL) {
            s->tlv_pending = pkt;
        } else {
            struct mbuf *tail = s->tlv_pending;
            while (tail->m_next != NULL) tail = tail->m_next;
            tail->m_next = pkt;
        }
    }

    if (chunk_type == 0)      s->full_chunks_sent++;
    else if (chunk_type == 1) s->partial_chunks_sent++;
    else                      s->max_chunks_sent++;
    s->total_chunks_sent++;

    chunk_free(&s->current);
    s->chunk_to = 0;
    cdc_state_reset(&s->rabin);
}

void module1_init(void)
{
    cdc_init_tables();
    module3_init();
    module2_init();
    module4_init();

    g_chunk_pool = rte_mempool_create(
        "chunk_buf_pool", CHUNK_POOL_COUNT, CHUNK_SIZE, CHUNK_POOL_CACHE, 0,
        NULL, NULL, NULL, NULL, rte_socket_id(), 0);

    if (g_chunk_pool == NULL) {
        printf("rte_mempool_create failed: %s\n", rte_strerror(rte_errno));
        return;
    }
    printf("chunk pool ready: %u x %u bytes  socket=%d cache=%u\n",
           CHUNK_POOL_COUNT, CHUNK_SIZE, rte_socket_id(), CHUNK_POOL_CACHE);
    printf("CDC: min=%u avg=%u max=%u  inactivity=%u ticks\n",
           CDC_MIN_CHUNK, CDC_TARGET_AVG, CDC_MAX_CHUNK, INACTIVITY_TICKS);
}

void module1_state_init(struct module1_state *s)
{
    if (!s) return;
    if (s->tlv_pending) { m_freem(s->tlv_pending); s->tlv_pending = NULL; }
    chunk_free(&s->current);
    memset(s, 0, sizeof(*s));
    rte_spinlock_init(&s->lock);
    cdc_init_state(&s->rabin);
}

uint32_t module1_add_data(struct module1_state *s, struct mbuf *m)
{
    struct mbuf  *seg;
    uint8_t      *ptr;
    uint32_t      seg_len, pos, total = 0;
    uint8_t       b;
    cdc_result_t  result;

    if (!s || !m) return 0;

    rte_spinlock_lock(&s->lock);        /* guard current + tlv_pending */

    for (seg = m; seg != NULL; seg = seg->m_next) {
        seg_len = seg->m_len;
        if (seg_len == 0) continue;
        ptr = mtod(seg, uint8_t *);

        for (pos = 0; pos < seg_len; pos++) {
            b = ptr[pos];

            if (s->current.data == NULL) {
                if (!chunk_alloc(&s->current)) {
                    rte_spinlock_unlock(&s->lock);
                    return total;
                }
            }
            if (s->current.bytes_used >= CHUNK_SIZE) {
                process_chunk(s, 2);
                if (!chunk_alloc(&s->current)) {
                    rte_spinlock_unlock(&s->lock);
                    return total;
                }
            }

            s->current.data[s->current.bytes_used] = b;
            s->current.bytes_used++;
            total++;
            s->total_bytes_received++;

            result = cdc_feed_byte(&s->rabin, b, s->current.bytes_used);
            switch (result) {
            case CDC_BOUNDARY:
            case CDC_BOUNDARY_STRONG: process_chunk(s, 0); break;
            case CDC_BOUNDARY_MAX:    process_chunk(s, 2); break;
            case CDC_CONTINUE:
            default: break;
            }
        }
    }

    if (s->current.bytes_used > 0 && s->chunk_to == 0)
        s->chunk_to = INACTIVITY_TICKS;

    rte_spinlock_unlock(&s->lock);
    return total;
}

void byte_caching_timer_check(struct module1_state *s)
{
    if (!s) return;
    rte_spinlock_lock(&s->lock);
    if (s->chunk_to == 0) { rte_spinlock_unlock(&s->lock); return; }
    s->chunk_to--;
    if (s->chunk_to == 0 && s->current.bytes_used > 0)
        process_chunk(s, 1);
    rte_spinlock_unlock(&s->lock);
}

void module1_flush(struct module1_state *s)
{
    if (!s) return;
    rte_spinlock_lock(&s->lock);
    if (s->current.bytes_used != 0)
        process_chunk(s, 1);
    rte_spinlock_unlock(&s->lock);
}

/* atomic detach -- caller sends/frees OUTSIDE the lock */
struct mbuf *module1_take_pending(struct module1_state *s)
{
    struct mbuf *m;
    if (!s) return NULL;
    rte_spinlock_lock(&s->lock);
    m = s->tlv_pending;
    s->tlv_pending = NULL;
    rte_spinlock_unlock(&s->lock);
    return m;
}
