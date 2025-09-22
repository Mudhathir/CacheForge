#include <vector>
#include <cstdint>
#include <iostream>
#include <cstring>
#include <cstdlib>
#include "../inc/champsim_crc2.h"

#define NUM_CORE            1
#define LLC_SETS            (NUM_CORE * 2048)
#define LLC_WAYS            16

// RRIP parameters
#define RRPV_BITS           2
#define RRPV_MAX            ((1 << RRPV_BITS) - 1)  // 3
#define RRPV_INIT           (RRPV_MAX - 1)          // 2

// Per-PC classification
#define PC_TABLE_SIZE       1024
#define PC_MASK             (PC_TABLE_SIZE - 1)
#define RATIO_THRESHOLD     8   // miss_count > hit_count * RATIO_THRESHOLD => cold; vice versa => hot

// Stream detection
#define STREAM_THRESHOLD    2   // # of identical strides to mark as stream
#define BI_PROB             32  // bimodal insertion probability denominator

// Dead-block predictor (Bloom filter)
#define DBP_BITS            2048
#define DBP_BYTES           (DBP_BITS >> 3)

// Replacement metadata
static uint8_t    repl_rrpv    [LLC_SETS][LLC_WAYS];
static bool       repl_has_hit [LLC_SETS][LLC_WAYS];
static uint16_t   repl_pc_sig  [LLC_SETS][LLC_WAYS];

// PC hit/miss counters & state
struct PCEntry {
    uint16_t hit_count;
    uint16_t miss_count;
    uint8_t  state;    // 0 = cold, 1 = neutral, 2 = hot
};
static PCEntry pc_table[PC_TABLE_SIZE];

// Per-PC stride & stream tracking
struct StreamEntry {
    int64_t last_blk;
    int64_t last_stride;
    uint8_t stride_count;
};
static StreamEntry stream_table[PC_TABLE_SIZE];

// Bloom‐filter for dead‐block prediction
static uint8_t dbp_filter[DBP_BYTES];

// Statistics
static uint64_t stat_hits, stat_misses, stat_bypasses;

// Simple multiplicative hash for DBP
static inline uint32_t dbp_hash1(uint16_t sig) {
    return (sig * 2654435761u) & (DBP_BITS - 1);
}
static inline uint32_t dbp_hash2(uint16_t sig) {
    return ((sig ^ 0xdead) * 2654435761u) & (DBP_BITS - 1);
}
static inline void DBP_Insert(uint16_t sig) {
    uint32_t h1 = dbp_hash1(sig), h2 = dbp_hash2(sig);
    dbp_filter[h1 >> 3] |= (1 << (h1 & 7));
    dbp_filter[h2 >> 3] |= (1 << (h2 & 7));
}
static inline bool DBP_Predict(uint16_t sig) {
    uint32_t h1 = dbp_hash1(sig), h2 = dbp_hash2(sig);
    return (dbp_filter[h1 >> 3] & (1 << (h1 & 7))) &&
           (dbp_filter[h2 >> 3] & (1 << (h2 & 7)));
}

// Initialize replacement state
void InitReplacementState() {
    // Initialize per‐line metadata
    memset(repl_rrpv,    RRPV_INIT, sizeof(repl_rrpv));
    memset(repl_has_hit, 0,         sizeof(repl_has_hit));
    memset(repl_pc_sig,  0,         sizeof(repl_pc_sig));
    // Initialize PC table & stream trackers
    for (int i = 0; i < PC_TABLE_SIZE; i++) {
        pc_table[i].hit_count  = 0;
        pc_table[i].miss_count = 0;
        pc_table[i].state      = 1; // neutral
        stream_table[i].last_blk     = -1;
        stream_table[i].last_stride  = 0;
        stream_table[i].stride_count = 0;
    }
    // Clear dead‐block predictor
    memset(dbp_filter, 0, sizeof(dbp_filter));
    // Zero stats
    stat_hits     = stat_misses = stat_bypasses = 0;
    // Seed RNG for bimodal insertion
    srand(0);
}

// Victim selection (SRRIP)
uint32_t GetVictimInSet(
    uint32_t /*cpu*/,
    uint32_t set,
    const BLOCK */*current_set*/,
    uint64_t /*PC*/,
    uint64_t /*paddr*/,
    uint32_t /*type*/
) {
    while (true) {
        // Look for RRPV == MAX
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            if (repl_rrpv[set][w] == RRPV_MAX)
                return w;
        }
        // Otherwise age all
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            if (repl_rrpv[set][w] < RRPV_MAX)
                repl_rrpv[set][w]++;
        }
    }
}

// Update on each access
void UpdateReplacementState(
    uint32_t /*cpu*/,
    uint32_t set,
    uint32_t way,
    uint64_t paddr,
    uint64_t PC,
    uint64_t /*victim_addr*/,
    uint32_t /*type*/,
    uint8_t hit
) {
    // Map PC to entry
    uint16_t pc_idx = (PC >> 2) & PC_MASK;
    PCEntry &pc     = pc_table[pc_idx];

    if (hit) {
        // ---- HIT ----
        stat_hits++;
        repl_has_hit[set][way] = true;
        repl_rrpv[set][way]    = 0; // promote to MRU
        // update PC hit count
        if (pc.hit_count < 1023) pc.hit_count++;
        // re‐classify PC
        if (pc.hit_count > RATIO_THRESHOLD * pc.miss_count)        pc.state = 2;
        else if (pc.miss_count > RATIO_THRESHOLD * pc.hit_count)  pc.state = 0;
        else                                                        pc.state = 1;
    } else {
        // ---- MISS & INSERT ----
        stat_misses++;
        // Dead‐block predictor: evicted block
        if (!repl_has_hit[set][way]) {
            DBP_Insert(repl_pc_sig[set][way]);
        }
        // update PC miss count
        if (pc.miss_count < 1023) pc.miss_count++;
        // re‐classify PC
        if (pc.hit_count > RATIO_THRESHOLD * pc.miss_count)        pc.state = 2;
        else if (pc.miss_count > RATIO_THRESHOLD * pc.hit_count)  pc.state = 0;
        else                                                        pc.state = 1;

        // Stream detection
        int64_t curr_blk = paddr >> 6;
        StreamEntry &st = stream_table[pc_idx];
        if (st.last_blk != -1 && (curr_blk - st.last_blk) == st.last_stride) {
            st.stride_count++;
        } else {
            st.last_stride  = curr_blk - st.last_blk;
            st.stride_count = 1;
        }
        st.last_blk = curr_blk;
        bool is_stream = (st.stride_count >= STREAM_THRESHOLD);

        // Choose insertion RRPV
        if (is_stream) {
            // Bimodal insertion for streams
            if ((rand() & (BI_PROB - 1)) == 0) repl_rrpv[set][way] = 0;
            else                                repl_rrpv[set][way] = RRPV_MAX;
        }
        else if (pc.state == 2) {
            // hot PC -> MRU insertion
            repl_rrpv[set][way] = 0;
        }
        else if (pc.state == 0 || DBP_Predict(pc_idx)) {
            // cold PC or dead-block -> bypass/demote
            repl_rrpv[set][way] = RRPV_MAX;
            stat_bypasses++;
        }
        else {
            // neutral PC -> default SRRIP
            repl_rrpv[set][way] = RRPV_INIT;
        }

        // install metadata
        repl_has_hit[set][way]  = false;
        repl_pc_sig[set][way]   = pc_idx;
    }
}

// Print final statistics
void PrintStats() {
    uint64_t total = stat_hits + stat_misses;
    double   hr    = total ? (100.0 * stat_hits / total) : 0.0;
    std::cout << "==== PDIP Statistics ====\n";
    std::cout << "Total refs   : " << total        << "\n";
    std::cout << "Hits         : " << stat_hits    << "\n";
    std::cout << "Misses       : " << stat_misses  << "\n";
    std::cout << "Bypasses     : " << stat_bypasses<< "\n";
    std::cout << "Hit rate (%) : " << hr           << "\n";
}

// Print periodic (heartbeat) statistics
void PrintStats_Heartbeat() {
    PrintStats();
}