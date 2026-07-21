#include <stdio.h>
#include <string.h>
#include <rte_hash.h>
#include <rte_jhash.h>
#include <rte_eal.h>
#include <rte_spinlock.h>
#include "module2.h"

static struct module2_state g_m2;

/* ---------------------------------------------------------------
 * O(1) NOTE -- why read_chunk/increment_ref don't need a scan OR a
 * second hash table:
 *
 * store_chunk always assigns  free_slot = g_m2.total_stored  (before
 * increment) and  cid = g_m2.next_chunk_id  (before increment), and
 * increments both together, every single insert, with no eviction
 * anywhere in this file. That means the invariant
 *
 *     slot == chunk_id - 1
 *
 * holds for every chunk_id this store ever hands out -- on a fresh
 * store AND after rebuilding the index from disk on startup (records
 * are replayed in the same cid order they were written). So a lookup
 * by chunk_id is a direct array index, not a scan.
 *
 * This is the O(1) fix for the linear-scan version: no rte_hash
 * needed for cid lookups, just bounds + is_used + chunk_id-match
 * validation (defensive -- if this invariant is ever broken by a
 * future eviction feature, these checks make it fail safe, i.e.
 * return 0, instead of serving the wrong chunk or reading OOB).
 *
 * find_by_hash is different: hash -> chunk_id is NOT sequential, so
 * that one genuinely needs the rte_hash table.
 * ------------------------------------------------------------- */

#if MODULE2_THREAD_SAFE
#  define M2_LOCK()    rte_spinlock_lock(&g_m2.lock)
#  define M2_UNLOCK()  rte_spinlock_unlock(&g_m2.lock)
#else
#  define M2_LOCK()    do {} while (0)
#  define M2_UNLOCK()  do {} while (0)
#endif

static inline int cid_to_slot(uint32_t cid)
{
    if (cid == 0 || cid > g_m2.next_chunk_id) return -1;
    uint32_t slot = cid - 1;
    if (slot >= MODULE2_MAX_CHUNKS) return -1;
    if (!g_m2.index[slot].is_used) return -1;
    if (g_m2.index[slot].chunk_id != cid) return -1;
    return (int)slot;
}

static uint32_t find_locked(const uint8_t *hash)
{
    void *data;
    int   slot;

    if (!hash || !g_m2.hash_table) return 0;
    if (rte_hash_lookup_data(g_m2.hash_table, hash, &data) < 0) return 0;

    slot = (int)(intptr_t)data;
    if (slot < 0 || slot >= MODULE2_MAX_CHUNKS || !g_m2.index[slot].is_used) return 0;
    if (memcmp(g_m2.index[slot].hash, hash, MODULE2_HASH_SIZE) != 0) return 0;

    return g_m2.index[slot].chunk_id;
}

void module2_init(void)
{
    long     off;
    uint32_t cid, dlen;
    uint8_t  h[MODULE2_HASH_SIZE];
    int      slot;
    struct rte_hash_parameters hp;

    memset(&g_m2, 0, sizeof(g_m2));
    g_m2.next_chunk_id = 1;
    rte_spinlock_init(&g_m2.lock);

    g_m2.fp = fopen(MODULE2_STORE_FILE, "r+b");
    if (!g_m2.fp) {
        g_m2.fp = fopen(MODULE2_STORE_FILE, "w+b");
        if (!g_m2.fp) { printf("cannot open chunk store file\n"); return; }
        printf("created new chunk store\n");
        goto build_hash;
    }

    printf("loading chunk store from disk...\n");
    fseek(g_m2.fp, 0, SEEK_SET);
    while (1) {
        off = ftell(g_m2.fp);
        if (off < 0) break;
        if (fread(&cid,  sizeof(uint32_t),  1, g_m2.fp) != 1) break;
        if (fread(&dlen, sizeof(uint32_t),  1, g_m2.fp) != 1) { printf("truncated record at %ld\n", off); break; }
        if (fread(h,     MODULE2_HASH_SIZE, 1, g_m2.fp) != 1) { printf("truncated hash at %ld\n",   off); break; }
        if (dlen == 0 || dlen > 65536) { printf("bad data length at %ld\n", off); break; }
        if (g_m2.total_stored >= MODULE2_MAX_CHUNKS) {
            printf("index full during rebuild, stopping at %u\n", g_m2.total_stored);
            break;
        }
        slot = g_m2.total_stored;
        g_m2.index[slot].chunk_id    = cid;
        g_m2.index[slot].data_len    = dlen;
        g_m2.index[slot].file_offset = off;
        g_m2.index[slot].ref_count   = 1;
        g_m2.index[slot].is_used     = 1;
        memcpy(g_m2.index[slot].hash, h, MODULE2_HASH_SIZE);
        if (cid >= g_m2.next_chunk_id) g_m2.next_chunk_id = cid + 1;
        g_m2.total_stored++;
        g_m2.total_bytes_stored += dlen;
        if (fseek(g_m2.fp, (long)dlen, SEEK_CUR) != 0) { printf("seek failed at chunk %u\n", cid); break; }
    }
    printf("done loading: %u chunks, next id %u\n", g_m2.total_stored, g_m2.next_chunk_id);

build_hash:
    memset(&hp, 0, sizeof(hp));
    hp.name      = "chunk_hash_table";
    hp.entries   = MODULE2_HASH_TABLE_SZ;
    hp.key_len   = MODULE2_HASH_SIZE;
    hp.hash_func = rte_jhash;
    hp.socket_id = rte_socket_id();
#if MODULE2_THREAD_SAFE
    hp.extra_flag = RTE_HASH_EXTRA_FLAGS_RW_CONCURRENCY
                  | RTE_HASH_EXTRA_FLAGS_MULTI_WRITER_ADD;
#endif

    g_m2.hash_table = rte_hash_create(&hp);
    if (!g_m2.hash_table) { printf("hash table creation failed\n"); return; }

    for (slot = 0; slot < (int)g_m2.total_stored; slot++) {
        intptr_t s = (intptr_t)slot;
        if (rte_hash_add_key_data(g_m2.hash_table, g_m2.index[slot].hash, (void *)s) < 0)
            printf("hash insert failed for chunk %u\n", g_m2.index[slot].chunk_id);
    }
    printf("hash table ready, %u entries\n", g_m2.total_stored);
}

uint32_t module2_find_by_hash(uint8_t *hash)
{
    uint32_t cid;
    M2_LOCK();
    cid = find_locked(hash);
    M2_UNLOCK();
    return cid;
}

uint32_t module2_store_chunk(uint8_t *data, uint32_t dlen, uint8_t *hash)
{
    int      free_slot;
    long     off;
    uint32_t cid;

    if (!data || !hash || dlen == 0)                     return 0;
    if (!g_m2.fp)         { printf("file not open\n");        return 0; }
    if (!g_m2.hash_table) { printf("hash table not ready\n"); return 0; }

    M2_LOCK();

    cid = find_locked(hash);
    if (cid != 0) {
        int slot = cid_to_slot(cid);
        if (slot >= 0) { g_m2.index[slot].ref_count++; g_m2.duplicate_count++; }
        M2_UNLOCK();
        return cid;
    }

    if (g_m2.total_stored >= MODULE2_MAX_CHUNKS) { M2_UNLOCK(); return 0; }

    free_slot = g_m2.total_stored;
    cid       = g_m2.next_chunk_id;

    fseek(g_m2.fp, 0, SEEK_END);
    off = ftell(g_m2.fp);
    if (fwrite(&cid,  sizeof(uint32_t),  1, g_m2.fp) != 1 ||
        fwrite(&dlen, sizeof(uint32_t),  1, g_m2.fp) != 1 ||
        fwrite(hash,  MODULE2_HASH_SIZE, 1, g_m2.fp) != 1 ||
        fwrite(data,  dlen,              1, g_m2.fp) != 1) {
        printf("write failed for chunk %u\n", cid);
        M2_UNLOCK(); return 0;
    }
    fflush(g_m2.fp);

    g_m2.index[free_slot].chunk_id    = cid;
    g_m2.index[free_slot].data_len    = dlen;
    g_m2.index[free_slot].file_offset = off;
    g_m2.index[free_slot].ref_count   = 1;
    g_m2.index[free_slot].is_used     = 1;
    memcpy(g_m2.index[free_slot].hash, hash, MODULE2_HASH_SIZE);

    {
        intptr_t s = (intptr_t)free_slot;
        if (rte_hash_add_key_data(g_m2.hash_table, hash, (void *)s) < 0) {
            printf("hash insert failed for chunk %u, rolling back\n", cid);
            g_m2.index[free_slot].is_used = 0;
            M2_UNLOCK(); return 0;
        }
    }

    g_m2.next_chunk_id++;
    g_m2.total_stored++;
    g_m2.total_bytes_stored += dlen;
    M2_UNLOCK();
    return cid;
}

uint32_t module2_read_chunk(uint32_t cid, uint8_t *buf, uint32_t buf_size)
{
    int    slot;
    size_t n;

    if (!buf || cid == 0) return 0;
    if (!g_m2.fp) { printf("file not open\n"); return 0; }

    M2_LOCK();
    slot = cid_to_slot(cid);
    if (slot < 0) { M2_UNLOCK(); return 0; }

    if (buf_size < g_m2.index[slot].data_len) { M2_UNLOCK(); return 0; }

    fseek(g_m2.fp, g_m2.index[slot].file_offset + MODULE2_HDR_SIZE, SEEK_SET);
    n = fread(buf, 1, g_m2.index[slot].data_len, g_m2.fp);
    M2_UNLOCK();

    if (n != g_m2.index[slot].data_len) { printf("short read %zu/%u\n", n, g_m2.index[slot].data_len); return 0; }
    return (uint32_t)n;
}

void module2_increment_ref(uint32_t cid)
{
    int slot;
    M2_LOCK();
    slot = cid_to_slot(cid);
    if (slot >= 0) { g_m2.index[slot].ref_count++; g_m2.duplicate_count++; }
    M2_UNLOCK();
}

void module2_print_stats(void)
{
    uint32_t total   = g_m2.total_stored + g_m2.duplicate_count;
    uint32_t hit_pct = total ? (g_m2.duplicate_count * 100) / total : 0;
    printf("unique chunks:  %u\n", g_m2.total_stored);
    printf("duplicates:     %u\n", g_m2.duplicate_count);
    printf("bytes stored:   %u\n", g_m2.total_bytes_stored);
    printf("next chunk id:  %u\n", g_m2.next_chunk_id);
    printf("dedup ratio:    %u%%\n", hit_pct);
}

void module2_close(void)
{
    if (g_m2.hash_table) { rte_hash_free(g_m2.hash_table); g_m2.hash_table = NULL; }
    if (g_m2.fp)         { fclose(g_m2.fp);                 g_m2.fp         = NULL; }
}
