#ifndef MODULE1_H
#define MODULE1_H

#include <stdint.h>
#include <rte_mempool.h>
#include <rte_spinlock.h>          /* per-connection lock */
#include "TCP/mbuf.h"
#include "cdc_engine.h"

#define CHUNK_SIZE          CDC_MAX_CHUNK
#define INACTIVITY_TICKS    200
#define TIMER_RESOLUTION      1
#define TIMER_BYTE_CACHING  INACTIVITY_TICKS

#define CHUNK_POOL_COUNT    2047U   /* raised from 511 -- see note below */
#define CHUNK_POOL_CACHE    32U

struct chunk {
    uint8_t  *data;
    uint32_t  bytes_used;
};

#define TLV_RX_VAL_MAX  CHUNK_SIZE

struct tlv_rx_state {
    int      active;
    uint8_t  hdr[3];
    uint8_t  hdr_got;
    uint8_t  val[TLV_RX_VAL_MAX + 8];
    uint32_t val_need;
    uint32_t val_got;
    uint8_t  piggy_buf[16];
    uint8_t  piggy_got;
    uint8_t  piggy_pending_dispatch;
};

struct module1_state {
    struct chunk            current;
    uint32_t                chunk_to;
    struct mbuf             *tlv_pending;
    struct tlv_rx_state      rx;
    struct cdc_rabin_state   rabin;

    rte_spinlock_t          lock;     /* guards current + tlv_pending */

    uint32_t total_bytes_received;
    uint32_t full_chunks_sent;
    uint32_t partial_chunks_sent;
    uint32_t max_chunks_sent;
    uint32_t total_chunks_sent;
};

void     module1_init(void);
void     module1_state_init(struct module1_state *s);
uint32_t module1_add_data(struct module1_state *s, struct mbuf *m);
void     byte_caching_timer_check(struct module1_state *s);
void     module1_flush(struct module1_state *s);

/* atomic detach of the pending TLV chain -- caller sends/frees OUTSIDE the lock */
struct mbuf *module1_take_pending(struct module1_state *s);

#endif /* MODULE1_H */
