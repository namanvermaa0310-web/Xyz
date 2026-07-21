#ifndef MODULE2_H
#define MODULE2_H

#include <stdint.h>
#include <stdio.h>
#include <rte_hash.h>
#include <rte_spinlock.h>

/* ---------------------------------------------------------------
 * Size to your working set, not a magic number.
 *   Measured avg chunk (cdcstat.py on a real store): ~2771 B
 *   500 MB worst case (no redundancy): ~189,200 distinct chunks
 *   MAX_CHUNKS below gives ~1.4x headroom over that.
 *   HASH_TABLE_SZ = 1.33x MAX_CHUNKS, ALWAYS -- the table must never
 *   fill before the index, or cuckoo inserts start silently failing
 *   and the index/table drift apart (this was the original SIGSEGV).
 * ------------------------------------------------------------- */
#define MODULE2_MAX_CHUNKS      262144
#define MODULE2_HASH_TABLE_SZ   349525
#define MODULE2_HASH_SIZE          32
#define MODULE2_STORE_FILE   "Chunk_store.dat"
#define MODULE2_HDR_SIZE           40   /* chunk_id(4) + data_len(4) + hash(32) */

/* Set to 0 ONLY if you have confirmed every module2 call runs on ONE lcore. */
#define MODULE2_THREAD_SAFE         1

struct chunk_index {
    uint32_t chunk_id;
    uint8_t  hash[MODULE2_HASH_SIZE];
    long     file_offset;
    uint32_t data_len;
    uint32_t ref_count;
    int      is_used;
};

struct module2_state {
    struct chunk_index  index[MODULE2_MAX_CHUNKS];
    uint32_t            total_stored;
    uint32_t            next_chunk_id;
    uint32_t            total_bytes_stored;
    uint32_t            duplicate_count;
    FILE               *fp;
    struct rte_hash    *hash_table;
    rte_spinlock_t      lock;
};

void     module2_init(void);
uint32_t module2_store_chunk(uint8_t *data, uint32_t data_len, uint8_t *hash);
uint32_t module2_find_by_hash(uint8_t *hash);
uint32_t module2_read_chunk(uint32_t chunk_id, uint8_t *buf, uint32_t buf_size);
void     module2_increment_ref(uint32_t chunk_id);
void     module2_print_stats(void);
void     module2_close(void);

#endif /* MODULE2_H */
