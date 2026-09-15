# BYTE CACHING — SIZING, LATENCY AND OPTIMISATION
## Analysis Summary

**Platform:** NXP LS1046A-RDB — Cortex-A72 quad-core @ 1.8 GHz, 8 GB DDR4
**Current configuration:** 8 GB store, 4 KB chunks, 2,097,152 slots

---

# PART A — EXPLANATION IN PLAIN TERMS

## A.1 What byte caching does

Both ends of the link keep a **filing cabinet** of data they have seen
before. Instead of sending a document again, the sender says *"use the
copy in drawer 47"* — a short note instead of the whole document.

```
   WITHOUT byte caching          WITH byte caching (repeat data)

   Send the whole file           Send a short reference
   500 MB across the link        35 bytes per 4 KB block
```

## A.2 What it costs

Keeping that filing cabinet is not free. Two costs:

**Disk** — the cabinet itself. A 200 GB cache needs a little over
200 GB of disk.

**Memory** — the *card catalogue*. To find anything quickly, the
appliance keeps a small record in RAM for every item in the cabinet.
More items means a bigger catalogue.

```
   200 GB cabinet, small items (4 KB)  -> 52 million cards -> 12.7 GB RAM
   200 GB cabinet, large items (16 KB) -> 13 million cards ->  3.2 GB RAM
   200 GB cabinet, big items  (64 KB)  ->  3 million cards ->  0.8 GB RAM
```

**The key insight for a non-technical audience:** the disk size is set
by how much data you want to remember. The *memory* size is set by how
many separate pieces you cut it into. Cutting into bigger pieces means
far less memory for the same amount of remembered data.

## A.3 The time cost

Before data reaches the link, the appliance must:

1. Scan every byte to decide where blocks begin and end
2. Compute a fingerprint for each block
3. Look that fingerprint up in the catalogue
4. Either send a short reference, or send the data

Steps 1 and 2 happen **whether or not** the data turns out to be a
repeat. That is the floor on how fast the appliance can go.

```
   For one 4 KB block:  about 57 microseconds of processing
   Which works out to:  about 570 Mbps maximum, from this work alone
```

## A.4 The headline conclusions

```
+-------------------------------------------------------------------+
|                                                                   |
|  1. The current 8 GB cache uses only about 0.5 GB of RAM.         |
|     There is far more headroom on this board than assumed.        |
|                                                                   |
|  2. A 200 GB cache is achievable on this board -- but only if     |
|     the block size is increased from 4 KB to 16 KB or larger.     |
|                                                                   |
|  3. Larger blocks help three ways at once: less memory, fewer     |
|     fingerprints to compute, fewer disk reads.                    |
|                                                                   |
|  4. Known optimisations reduce memory by roughly 60% with no      |
|     loss of function.                                             |
|                                                                   |
+-------------------------------------------------------------------+
```

---

# PART B — TECHNICAL ANALYSIS

## B.1 Memory model

Four structures scale with chunk count `n`:

```
   n = STORE_BYTES / CHUNK_SIZE

   index array     n x 56 B     struct chunk_index, 8-byte aligned
   free-slot stack n x  4 B     uint32_t slot indices
   hash tables     2 x (2n) x 48 B
                                two rte_hash tables (hash->slot,
                                cid->slot), each sized 2x keys,
                                ~48 B per entry incl. bucket + key
   confirmed bitmap (4n/8) x 16 peers
                                4x cid headroom, 1 bit per cid

   disk file       n x (40 + CHUNK_SIZE)
```

`struct chunk_index` breakdown:

```
   chunk_id     4 B
   hash        32 B
   data_len     4 B
   ref_count    4 B
   last_use     8 B
   ref_bit      1 B
   is_used      1 B
   ---------------
   raw         54 B  ->  56 B aligned
```

## B.2 Sizing table — current design

```
 store  chunk       chunks  file GB    index     hash   bitmap   RAM GB
    8G     4K    2,097,152      8.1     0.11     0.38     0.02     0.51
   16G     4K    4,194,304     16.2     0.22     0.75     0.03     1.02
   16G    16K    1,048,576     16.0     0.05     0.19     0.01     0.25
   32G    16K    2,097,152     32.1     0.11     0.38     0.02     0.51
   64G    16K    4,194,304     64.2     0.22     0.75     0.03     1.02
  200G     4K   52,428,800    202.0     2.73     9.38     0.39    12.70
  200G    16K   13,107,200    200.5     0.68     2.34     0.10     3.17
  200G    64K    3,276,800    200.1     0.17     0.59     0.02     0.79
```

**Observation:** the hash tables dominate — 74% of total RAM at 4 KB
chunks. They are therefore the correct optimisation target.

## B.3 Latency budget

Cortex-A72 @ 1.8 GHz, 1 cycle = 0.556 ns.

### Per-byte cost — paid on every byte, HIT or MISS

```
   cdc_feed_byte()      ~12 cycles/byte  =  6.67 ns
   memcpy to chunk buf   ~1 cycle/byte   =  0.56 ns
                                            -------
                                            7.22 ns/byte

   Ceiling from CDC alone:  138 MB/s  =  1,108 Mbps
```

### Per-chunk cost — at each chunk boundary

```
                             4 KB chunk      16 KB chunk
   SHA-256 (software)          27.31 us        109.23 us
   SHA-256 (ARMv8 crypto)       4.32 us         17.29 us
   rte_hash lookup              0.08 us          0.08 us
   TLV mbuf build               0.22 us          0.22 us
```

### End-to-end, one 4 KB chunk, steady-state HIT (sender)

```
   CDC rolling hash (4096 B)       27.31 us
   memcpy into chunk buffer         2.28 us
   SHA-256 (software)              27.31 us   <-- 48% of total
   hash table lookup                0.08 us
   bitmap bit_test                  0.01 us
   build 35-byte HIT TLV            0.22 us
   append_pending (O(1))            0.01 us
   -------------------------------- --------
   SENDER TOTAL                    57.21 us

   Throughput ceiling = 4096 B / 57.21 us = 71.6 MB/s = 573 Mbps
```

### Receiver cost, per HIT

```
   fseek + fread, random, from disk     ~100-500 us   <-- DOMINANT
   from page cache / RAM                    ~2-5 us
```

**A single random disk read exceeds the entire sender-side pipeline by
2-10x.** At 1.17 million HITs per transfer this is the largest term in
the system.

### Measured vs modelled

```
   Observed        504,498,544 B in 26.0 s  =  18.7 MB/s  =  150 Mbps
   Sender ceiling                              71.6 MB/s  =  573 Mbps
```

Measured throughput is ~26% of the sender-side ceiling, which means
sender CPU is **not** the binding constraint. The gap is attributable
to receiver-side random reads and the proxy datapath.

## B.4 Optimisations identified

### O1 — Remove the cid->slot hash table

The HIT path currently performs two lookups:

```c
cid = module2_find_by_hash(val);          /* hash -> slot -> returns cid */
n   = module2_read_chunk(cid, buf, ...);  /* cid  -> slot  (AGAIN)      */
```

The first lookup resolves the slot and discards it; the second table
exists only to recover it. Returning the slot from `find_by_hash`
eliminates the table **and** one lookup per HIT.

```
   Saving: 50% of hash memory, plus 1.17M fewer lookups per transfer
```

### O2 — Truncated hash key with verification

`rte_hash` stores the full 32-byte key for comparison. Since the full
hash is already held in `index[slot].hash`, the table can be keyed on
an 8-byte prefix, with verification against the stored full hash:

```c
hp.key_len = 8;                        /* was 32 */
...
if (memcmp(index[slot].hash, full_hash, 32) != 0)
    return 0;                          /* prefix collision -> miss */
```

A prefix collision produces a false miss (chunk sent as MISS rather
than HIT) — never an incorrect resolution. **The memcmp verification
is mandatory**; without it a collision would resolve to the wrong
chunk.

```
   Saving: ~48 B/entry -> ~24 B/entry
```

### O3 — Load factor 1.2x instead of 2.0x

DPDK cuckoo hashing operates correctly at ~0.9 load factor.

```
   Saving: a further 40% of hash memory
```

### Cumulative effect at 200 GB

```
 chunk config                              hash GB   RAM GB
    4K current (2 tbl, 32B key, 2x)           9.38    12.70
       + drop cid map (1 table)               4.69     8.01
       + 8-byte key                           2.34     5.66
       + 1.2x load factor                     1.41     4.73

   16K current (2 tbl, 32B key, 2x)           2.34     3.17
       + drop cid map (1 table)               1.17     2.00
       + 8-byte key                           0.59     1.42
       + 1.2x load factor                     0.35     1.18

   64K current (2 tbl, 32B key, 2x)           0.59     0.79
       + drop cid map (1 table)               0.29     0.50
       + 8-byte key                           0.15     0.35
       + 1.2x load factor                     0.09     0.30
```

### O4 — ARMv8 SHA-256 crypto extensions

LS1046A's Cortex-A72 implements the ARMv8 cryptographic extensions.
SHA-256 is 48% of per-chunk sender cost.

```
   software     12 cycles/byte  ->  27.31 us per 4 KB chunk
   crypto ext  1.9 cycles/byte  ->   4.32 us per 4 KB chunk   (6.3x)

   Sender total: 57.21 us  ->  34.22 us   (ceiling 573 -> 958 Mbps)
```

Verify with:  `openssl speed -evp sha256`
(~150 MB/s indicates software; ~900 MB/s indicates hardware)

### O5 — Increase CHUNK_SIZE from 4 KB to 16 KB

Independently justified: the `cdcstat` report showed **37.5% of chunk
boundaries were forced MAX-clamps** because `CDC_MAX_CHUNK` equals
`CDC_TARGET_AVG` (both 4096). Position-based cuts do not survive
edits and weaken resynchronisation.

```
   Effects of 4 KB -> 16 KB:
     chunk count            /4
     index + hash memory    /4
     SHA-256 calls per MB   /4
     disk reads per MB      /4
     forced MAX-clamps      37.5% -> near 0
```

## B.5 Maximum store size on LS1046A

Assuming a 4 GB budget for cache metadata (leaving 4 GB for OS, DPDK
mbuf pools, QoS and application):

```
 chunk    max store (current)    max store (optimised)
    4K                  63 GB                   169 GB
   16K                 252 GB                   677 GB
   64K                1008 GB                  2708 GB
```

**Conclusion:** 200 GB is achievable on this board at 16 KB chunks
using the current design (3.17 GB), and comfortably so with the
optimisations applied (1.18 GB).

A practical operating point, allowing generous headroom:

```
   +-------------------------------------------------------------+
   |  RECOMMENDED TARGET                                         |
   |                                                             |
   |     store size    200 GB                                    |
   |     chunk size     16 KB                                    |
   |     chunks         13,107,200                               |
   |     disk file      200.5 GB                                 |
   |     RAM (current design)      3.17 GB                       |
   |     RAM (with O1+O2+O3)       1.18 GB                       |
   |                                                             |
   |  Requires: storage device of 256 GB or larger.              |
   |  Random-read performance of that device becomes the         |
   |  primary throughput constraint -- NVMe strongly preferred.  |
   +-------------------------------------------------------------+
```

## B.6 Correction to an earlier estimate

An earlier verbal estimate placed the hash tables at approximately
1-1.5 GB for the 8 GB store, and concluded that a 16 GB store would
exceed available hugepages. The computed figure is **0.38 GB**, and a
16 GB store requires 1.02 GB total. The earlier estimate was
excessively conservative and the resulting guidance was wrong. The
figures in this document are computed from the actual structure
definitions.

## B.7 Priority of work

```
   +----+--------------------------------+----------+---------------+
   | #  | Action                         | Effort   | Benefit       |
   +----+--------------------------------+----------+---------------+
   | 1  | Confirm SHA-256 crypto ext     | 1 command| up to 6.3x on |
   |    | in use (openssl speed)         |          | 48% of cost   |
   +----+--------------------------------+----------+---------------+
   | 2  | CDC_MAX_CHUNK 4 KB -> 16 KB    | 1 constant| 4x memory,   |
   |    |                                | + retest | 4x SHA, fixes |
   |    |                                |          | 37.5% clamps  |
   +----+--------------------------------+----------+---------------+
   | 3  | O1: remove cid->slot table     | contained| 50% hash RAM, |
   |    |                                | refactor | 1 less lookup |
   +----+--------------------------------+----------+---------------+
   | 4  | Profile the datapath           | perf top | identifies    |
   |    | (measured 26% of modelled      |          | the actual    |
   |    |  ceiling -- gap unexplained)   |          | constraint    |
   +----+--------------------------------+----------+---------------+
   | 5  | O2 + O3: key and load factor   | small    | further 60%   |
   +----+--------------------------------+----------+---------------+
```

**Note on item 4:** measured throughput is 150 Mbps against a modelled
sender ceiling of 573 Mbps. Until that discrepancy is explained by
profiling, increasing store size risks amplifying an unidentified
bottleneck rather than improving performance.

---

**End of document.**
