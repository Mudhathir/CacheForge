#include <vector>
#include <cstdint>
#include <iostream>
#include <cstring>
#include <cstdlib>
#include "../inc/champsim_crc2.h"

#define NUM_CORE           1
#define LLC_SETS           (NUM_CORE * 2048)
#define LLC_WAYS           16

// RRPV parameters
#define RRPV_BITS          3
#define RRPV_MAX           ((1 << RRPV_BITS) - 1)   // 7
#define RRPV_INIT          (RRPV_MAX - 1)           // 6

// SHiP signature table
#define SIG_TABLE_SIZE     4096
#define SIG_CTR_BITS       2
#define SIG_CTR_MAX        ((1 << SIG_CTR_BITS) - 1) // 3
#define SIG_CTR_INIT       (SIG_CTR_MAX >> 1)        // 1
#define HOT_THRESHOLD      (SIG_CTR_INIT + 1)        // 2

// Stream detection
#define STREAM_DETECT_COUNT 2
#define MAX_SMALL_STRIDE    8

// Thrash (miss) counters per signature
#define MISS_CTR_BITS      2
#define MISS_CTR_MAX       ((1 << MISS_CTR_BITS) - 1) // 3
#define MISS_THRESHOLD     (MISS_CTR_MAX)             // 3

// Dead‐Block Predictor (Bloom filter)
#define DBP_BITS           8192
#define DBP_BYTE_SIZE      (DBP_BITS >> 3)            // 1024 bytes

// Phase reset interval (references)
#define RESET_INTERVAL     (1ULL << 20) // ~1M refs

// Per-PC policy dueling
#define PC_DIP_BUCKETS     64
#define PC_DIP_MAX         (PC_DIP_BUCKETS - 1)        // 63
#define PC_DIP_THRESHOLD   (PC_DIP_MAX >> 1)           // 31

// ----------------------------------------------------------------------------
// Replacement metadata
static uint8_t   repl_rrpv      [LLC_SETS][LLC_WAYS];
static uint16_t  repl_sig       [LLC_SETS][LLC_WAYS];
static bool      repl_has_hit   [LLC_SETS][LLC_WAYS];
static bool      repl_used_ship [LLC_SETS][LLC_WAYS];

// SHiP tables & stream detection & thrash counters
static uint8_t   ship_ctr       [SIG_TABLE_SIZE];
static uint8_t   ship_miss_ctr  [SIG_TABLE_SIZE];
static int64_t   last_block     [SIG_TABLE_SIZE];
static int64_t   last_stride    [SIG_TABLE_SIZE];
static uint8_t   stride_run     [SIG_TABLE_SIZE];

// Dead‐Block Bloom filter
static uint8_t   dbp_filter     [DBP_BYTE_SIZE];

// Per-PC dueling counters
static uint8_t   pc_dip         [PC_DIP_BUCKETS];

// Statistics & ref counter
static uint64_t  stat_hits, stat_misses, stat_evictions, stat_bypasses;
static uint64_t  ref_count;

// ----------------------------------------------------------------------------
// Hash functions for DBP
static inline uint32_t dbp_hash1(uint16_t sig) {
    return (uint32_t(sig) * 2654435761u) & (DBP_BITS - 1);
}
static inline uint32_t dbp_hash2(uint16_t sig) {
    return ((uint32_t(sig) ^ 0xdead) * 2654435761u) & (DBP_BITS - 1);
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

// ----------------------------------------------------------------------------
// Initialize replacement state
void InitReplacementState() {
    // Clear per-line metadata
    for (uint32_t s = 0; s < LLC_SETS; s++) {
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            repl_rrpv[s][w]      = RRPV_MAX;
            repl_sig[s][w]       = 0;
            repl_has_hit[s][w]   = false;
            repl_used_ship[s][w] = false;
        }
    }
    // Init SHiP tables
    for (uint32_t i = 0; i < SIG_TABLE_SIZE; i++) {
        ship_ctr[i]      = SIG_CTR_INIT;
        ship_miss_ctr[i] = 0;
        last_block[i]    = -1;
        last_stride[i]   = 0;
        stride_run[i]    = 0;
    }
    // Clear dead-block predictor
    std::memset(dbp_filter, 0, sizeof(dbp_filter));
    // Init per-PC dueling counters
    for (uint32_t i = 0; i < PC_DIP_BUCKETS; i++)
        pc_dip[i] = PC_DIP_THRESHOLD;
    // Zero stats
    stat_hits      = stat_misses = stat_evictions = stat_bypasses = 0;
    ref_count      = 0;
}

// ----------------------------------------------------------------------------
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
        // find an RRPV == MAX
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            if (repl_rrpv[set][w] == RRPV_MAX)
                return w;
        }
        // otherwise age all lines
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            if (repl_rrpv[set][w] < RRPV_MAX)
                repl_rrpv[set][w]++;
        }
    }
    return 0;
}

// ----------------------------------------------------------------------------
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
    // Periodic reset
    if (++ref_count && (ref_count & (RESET_INTERVAL - 1)) == 0) {
        for (uint32_t i = 0; i < SIG_TABLE_SIZE; i++) {
            ship_ctr[i]      >>= 1;
            ship_miss_ctr[i] = 0;
        }
        std::memset(dbp_filter, 0, sizeof(dbp_filter));
    }

    // Determine PC bucket
    uint32_t pc_bucket = (uint32_t)(PC >> 6) & (PC_DIP_BUCKETS - 1);

    if (hit) {
        // ---- HIT ----
        stat_hits++;
        uint16_t psig = repl_sig[set][way];
        // first hit on this block?
        if (!repl_has_hit[set][way]) {
            if (ship_ctr[psig] < SIG_CTR_MAX) ship_ctr[psig]++;
            ship_miss_ctr[psig] = 0;
            repl_has_hit[set][way] = true;
        }
        // promote
        repl_rrpv[set][way] = 0;
        // update per-PC dueling
        if (repl_used_ship[set][way]) {
            if (pc_dip[pc_bucket] < PC_DIP_MAX) pc_dip[pc_bucket]++;
        } else {
            if (pc_dip[pc_bucket] > 0)        pc_dip[pc_bucket]--;
        }
    } else {
        // ---- MISS & EVICTION ----
        stat_misses++;
        stat_evictions++;
        // handle eviction of old block
        {
            uint16_t old_sig = repl_sig[set][way];
            bool     old_hit = repl_has_hit[set][way];
            if (!old_hit) {
                if (ship_miss_ctr[old_sig] < MISS_CTR_MAX)
                    ship_miss_ctr[old_sig]++;
                if (ship_ctr[old_sig] > 0)
                    ship_ctr[old_sig]--;
                DBP_Insert(old_sig);
            }
        }
        // compute new signature
        uint16_t sig = ((PC >> 2) ^ (PC >> 12)) & (SIG_TABLE_SIZE - 1);
        // stream detection
        int64_t curr_blk = paddr >> 6;
        bool    is_stream = false;
        if (last_block[sig] >= 0) {
            int64_t stride = curr_blk - last_block[sig];
            if (stride == last_stride[sig])
                stride_run[sig]++;
            else {
                last_stride[sig] = stride;
                stride_run[sig]  = 1;
            }
            if (stride_run[sig] >= STREAM_DETECT_COUNT &&
                std::llabs(last_stride[sig]) <= MAX_SMALL_STRIDE) {
                is_stream = true;
            }
        } else {
            stride_run[sig]  = 1;
            last_stride[sig] = 0;
        }
        last_block[sig] = curr_blk;

        // choose insertion policy per-PC
        bool use_ship = (pc_dip[pc_bucket] > PC_DIP_THRESHOLD);
        repl_used_ship[set][way] = use_ship;

        // Dead-block bypass or thrash bypass
        if (DBP_Predict(sig) || ship_miss_ctr[sig] >= MISS_THRESHOLD) {
            repl_rrpv[set][way] = RRPV_MAX;
            stat_bypasses++;
        }
        else if (use_ship) {
            // SHiP-guided
            if (ship_ctr[sig] >= HOT_THRESHOLD) {
                repl_rrpv[set][way] = 0;
            }
            else if (is_stream) {
                repl_rrpv[set][way] = (RRPV_INIT > 0 ? RRPV_INIT - 1 : 0);
            }
            else {
                repl_rrpv[set][way] = RRPV_INIT;
            }
        } else {
            // Baseline SRRIP
            repl_rrpv[set][way] = RRPV_INIT;
        }

        // install metadata
        repl_sig[set][way]     = sig;
        repl_has_hit[set][way] = false;

        // update per-PC dueling for miss
        if (use_ship) {
            if (pc_dip[pc_bucket] > 0)         pc_dip[pc_bucket]--;
        } else {
            if (pc_dip[pc_bucket] < PC_DIP_MAX) pc_dip[pc_bucket]++;
        }
    }
}

// ----------------------------------------------------------------------------
// Print final statistics
void PrintStats() {
    uint64_t total = stat_hits + stat_misses;
    double   hr    = total ? (100.0 * double(stat_hits) / double(total)) : 0.0;
    std::cout << "==== PARP Statistics ====\n";
    std::cout << "Total refs    : " << total          << "\n";
    std::cout << "Hits          : " << stat_hits      << "\n";
    std::cout << "Misses        : " << stat_misses    << "\n";
    std::cout << "Evictions     : " << stat_evictions << "\n";
    std::cout << "Bypasses      : " << stat_bypasses  << "\n";
    std::cout << "Hit rate (%)  : " << hr             << "\n";
}

// ----------------------------------------------------------------------------
// Print periodic (heartbeat) statistics
void PrintStats_Heartbeat() {
    PrintStats();
}