# Phase 2 — Epoch / Generation

Goal: when a peer's store is wiped/recreated, the sender detects it ONCE
at session start and sends everything as MISS (cold) instead of
discovering staleness one bounced NAK at a time.

Plug-in points confirmed in your code:
- module2.c:21  fopen("r+b") success = store existed; fallback to
                 "w+b" = store created fresh. This IS the wipe signal.
- tcp_input.c:5249  tcp_finalize_options() — runs at SYN/SYN-ACK
                 completion, already negotiates session_dedup_flags.
                 This is where generation is exchanged (HELLO).

================================================================
 PIECE 1 — module2: persist a generation, bump on fresh store
================================================================

module2.h  — add to struct module2_state:
    uint32_t generation;

module2.c  — new file "Chunk_store.gen" alongside the store.
Add near the top:

    #define MODULE2_GEN_FILE  "Chunk_store.gen"

Replace the fopen block in module2_init (around line 21) with:

    int store_existed = 1;
    g_m2.fp = fopen(MODULE2_STORE_FILE, "r+b");
    if (!g_m2.fp) {
        store_existed = 0;                       /* fresh store */
        g_m2.fp = fopen(MODULE2_STORE_FILE, "w+b");
        if (!g_m2.fp) { printf("cannot open chunk store\n"); return; }
    }

    /* --- generation handling --- */
    {
        FILE *gf = fopen(MODULE2_GEN_FILE, "r");
        uint32_t g = 0;
        if (gf) { if (fscanf(gf, "%u", &g) != 1) g = 0; fclose(gf); }

        if (!store_existed) {
            /* store was wiped/created fresh -> bump generation so
             * peers know our contents are gone */
            g = g + 1;
            FILE *gw = fopen(MODULE2_GEN_FILE, "w");
            if (gw) { fprintf(gw, "%u\n", g); fclose(gw); }
        } else if (g == 0) {
            /* store existed but no gen file yet (first run of this
             * feature) -> initialise to 1 */
            g = 1;
            FILE *gw = fopen(MODULE2_GEN_FILE, "w");
            if (gw) { fprintf(gw, "%u\n", g); fclose(gw); }
        }
        g_m2.generation = g;
        printf("module2: generation = %u (store %s)\n",
               g, store_existed ? "loaded" : "fresh");
    }

module2.c — accessor (add at end, and prototype in module2.h):

    uint32_t module2_get_generation(void) { return g_m2.generation; }

================================================================
 PIECE 2 — carry peer's generation + a "cold" flag per connection
================================================================

tcp_var.h — add to struct TCPlookuptable (near session_dedup_flags):

    uint32_t peer_generation;   /* generation peer advertised at setup */
    uint8_t  peer_is_cold;      /* 1 = peer's store stale/empty ->
                                   send MISS only, never HIT */

================================================================
 PIECE 3 — exchange generation in the handshake (HELLO)
================================================================

The clean way in THIS codebase: generation travels as a TCP option
alongside the existing dedup-capability negotiation, so it is present
by the time tcp_finalize_options() runs. Two sub-steps:

 (a) When building SYN / SYN-ACK options (wherever session_dedup_flags
     capability is advertised), also write module2_get_generation()
     into a new 4-byte option field.

 (b) In tcp_finalize_options() (tcp_input.c:5249), after the dedup
     flag is confirmed, store the peer's advertised generation into
     tp->t_tcblut->peer_generation, then set the cold flag by
     comparing against what we knew before:

        /* peer_prev = generation we last recorded for this peer_id
         * (site_num). Look it up in a small per-site table indexed
         * by tp->t_tcblut->site_num. If different (or unknown),
         * the peer's store changed -> treat as cold. */
        uint16_t sid = tp->t_tcblut->site_num;
        if (peer_gen_table[sid] != peer_advertised_gen) {
            tp->t_tcblut->peer_is_cold = 1;   /* force MISS this session */
            peer_gen_table[sid] = peer_advertised_gen;
        } else {
            tp->t_tcblut->peer_is_cold = 0;
        }
        tp->t_tcblut->peer_generation = peer_advertised_gen;

     peer_gen_table[] is a new global: uint32_t peer_gen_table[MAX_SITES];
     persisted if you want cold-detection to survive a sender restart,
     in-memory-only if per-run is enough for now.

================================================================
 PIECE 4 — honor the cold flag in the HIT/MISS decision
================================================================

The encode path currently calls module4_lookup_and_build_tlv(hash,
data, len) which decides HIT vs MISS from the local store. Thread the
cold flag in so a cold peer forces MISS:

module4.h:
    struct mbuf *module4_lookup_and_build_tlv(uint8_t *hash,
                     uint8_t *data, uint32_t data_len, uint8_t peer_cold);

module4.c — in module4_lookup_and_build_tlv:

    g_m4.total_lookups++;
    cid = module2_find_by_hash(hash);
    if (cid != 0 && !peer_cold) {          /* CHANGE: && !peer_cold */
        g_m4.total_hits++;
        module2_increment_ref(cid);
        return build_tlv_mbuf(TLV_TYPE_HIT, hash, MODULE4_HASH_SIZE);
    } else {
        g_m4.total_misses++;
        if (cid == 0)
            cid = module2_store_chunk(data, dlen, hash);
        return build_tlv_mbuf(TLV_TYPE_MISS, data, dlen);
    }

Caller (module1's process_chunk, which calls
module4_lookup_and_build_tlv) passes
    tp->t_tcblut->peer_is_cold
down as the new argument. module1 already has the connection's tp in
scope at that point.

================================================================
 RESULT
================================================================
Wipe test now: B wiped -> B's generation bumps -> B advertises new gen
at session start -> A sees mismatch -> A sets peer_is_cold for that
session -> A sends ALL MISS, zero HITs, zero NAKs. B refills in one
pass. NAK (Phase 1) remains as the safety net for anything epoch
misses (mid-session eviction, races).

================================================================
 WHAT I NEED TO WRITE PIECE 3 EXACTLY
================================================================
The TCP-option plumbing is the one part I cannot write blind -- it
depends on how session_dedup_flags is currently encoded into options.
Send:
  - the code that SETS session_dedup_flags into a SYN/SYN-ACK option
    (grep: "session_dedup_flags =" and the tcpopt building code)
  - struct tcpopt definition (likely in tcp_var.h)
  - tcp_finalize_options() body (tcp_input.c:5249 onward)
Then Piece 3 becomes exact instead of pseudocode.
