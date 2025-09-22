#include <vector>
#include <cstdint>
#include <iostream>
#include <cstring>
#include "../inc/champsim_crc2.h"

#define NUM_CORE           1
#define LLC_SETS           (NUM_CORE * 2048)
#define LLC_WAYS           16

// SRRIP parameters
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
#define MISS_THRESHOLD     (MISS_CTR_MAX)             // bypass when saturated

// Phase reset interval (references)
#define RESET_INTERVAL     (1ULL << 20) // ~1M refs

// DIP (Set‐Dueling Insertion Policy) parameters
#define DIP_BITS           10
#define DIP_MAX            ((1 << DIP_BITS) - 1)
#define DIP_THRESHOLD      (DIP_MAX >> 1)
#define DIP_SET_MASK       0x3F               // mod 64
#define DIP_LEADER_SIZE    8                  // 8 sets for each leader

// Bloom‐filter Dead‐Block Predictor (DBP)
#define DBP_BITS           8192
#define DBP_BYTE_SIZE      (DBP_BITS >> 3)    // 1024 bytes

// ----------------------------------------------------------------------------
// Replacement metadata
static uint8_t   repl_rrpv   [LLC_SETS][LLC_WAYS];
static uint16_t  repl_sig    [LLC_SETS][LLC_WAYS];
static bool      repl_has_hit[LLC_SETS][LLC_WAYS];

// SHiP tables & stream detection & thrash counters
static uint8_t   ship_ctr     [SIG_TABLE_SIZE];
static uint8_t   ship_miss_ctr[SIG_TABLE_SIZE];
static int64_t   last_block   [SIG_TABLE_SIZE];
static int64_t   last_stride  [SIG_TABLE_SIZE];
static uint8_t   stride_run   [SIG_TABLE_SIZE];

// DBP filter
static uint8_t   dbp_filter[DBP_BYTE_SIZE];

// DIP meta‐counter
static uint32_t  dip_counter;

// Statistics
static uint64_t  stat_hits, stat_misses, stat_evictions, stat_bypasses;
static uint64_t  ref_count;

// ----------------------------------------------------------------------------
// Helpers: DIP selectors
static inline bool is_leader_A(uint32_t set) {
    return (set & DIP_SET_MASK) < DIP_LEADER_SIZE;
}
static inline bool is_leader_B(uint32_t set) {
    uint32_t x = (set & DIP_SET_MASK);
    return x >= DIP_LEADER_SIZE && x < (DIP_LEADER_SIZE << 1);
}
static inline bool use_sig_policy(uint32_t set) {
    if (is_leader_A(set)) return true;
    if (is_leader_B(set)) return false;
    return (dip_counter > DIP_THRESHOLD);
}

// ----------------------------------------------------------------------------
// Helpers: DBP (Bloom filter) – 2‐hash scheme
static inline uint32_t dbp_hash1(uint16_t sig) {
    // Knuth's multiplicative hash, low bits
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
    // Clear RRPVs & metadata
    for (uint32_t s = 0; s < LLC_SETS; s++)
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            repl_rrpv[s][w]    = RRPV_MAX;
            repl_sig[s][w]     = 0;
            repl_has_hit[s][w] = false;
        }
    // Init SHiP structures
    for (uint32_t i = 0; i < SIG_TABLE_SIZE; i++) {
        ship_ctr[i]       = SIG_CTR_INIT;
        ship_miss_ctr[i]  = 0;
        last_block[i]     = -1;
        last_stride[i]    = 0;
        stride_run[i]     = 0;
    }
    // Clear DBP filter
    std::memset(dbp_filter, 0, sizeof(dbp_filter));
    // Initialize DIP
    dip_counter   = DIP_THRESHOLD;
    // Stats
    stat_hits      = stat_misses = stat_evictions = stat_bypasses = 0;
    ref_count      = 0;
}

// ----------------------------------------------------------------------------
// Victim selection (standard SRRIP scan)
uint32_t GetVictimInSet(
    uint32_t /*cpu*/,
    uint32_t set,
    const BLOCK */*current_set*/,
    uint64_t /*PC*/,
    uint64_t /*paddr*/,
    uint32_t /*type*/
) {
    while (true) {
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            if (repl_rrpv[set][w] == RRPV_MAX)
                return w;
        }
        for (uint32_t w = 0; w < LLC_WAYS; w++)
            if (repl_rrpv[set][w] < RRPV_MAX)
                repl_rrpv[set][w]++;
    }
    return 0;
}

// ----------------------------------------------------------------------------
// Update state on every access
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
    // Phase reset
    if (++ref_count && (ref_count & (RESET_INTERVAL - 1)) == 0) {
        for (uint32_t i = 0; i < SIG_TABLE_SIZE; i++) {
            ship_ctr[i]       >>= 1;
            ship_miss_ctr[i] = 0;
        }
        std::memset(dbp_filter, 0, sizeof(dbp_filter));
    }

    // DIP counter update on leader sets
    if (is_leader_A(set)) {
        if (hit) {
            if (dip_counter < DIP_MAX) dip_counter++;
        } else {
            if (dip_counter > 0) dip_counter--;
        }
    } else if (is_leader_B(set)) {
        if (hit) {
            if (dip_counter > 0) dip_counter--;
        } else {
            if (dip_counter < DIP_MAX) dip_counter++;
        }
    }

    if (hit) {
        // ---- HIT ----
        stat_hits++;
        uint16_t psig = repl_sig[set][way];
        if (!repl_has_hit[set][way]) {
            // first hit for this line
            if (ship_ctr[psig] < SIG_CTR_MAX)
                ship_ctr[psig]++;
            ship_miss_ctr[psig] = 0;
            repl_has_hit[set][way] = true;
        }
        // promote to MRU
        repl_rrpv[set][way] = 0;
    } else {
        // ---- MISS & EVICTION ----
        stat_misses++;
        stat_evictions++;
        // thrash / DBP insert on evicted signature
        {
            uint16_t old_sig = repl_sig[set][way];
            bool     old_hit = repl_has_hit[set][way];
            if (!old_hit) {
                // SHiP miss counter
                if (ship_miss_ctr[old_sig] < MISS_CTR_MAX)
                    ship_miss_ctr[old_sig]++;
                if (ship_ctr[old_sig] > 0)
                    ship_ctr[old_sig]--;
            }
            // DBP learns dead block patterns
            if (!old_hit) {
                DBP_Insert(old_sig);
            }
        }

        // compute signature
        uint16_t sig = ((PC >> 2) ^ (PC >> 12)) & (SIG_TABLE_SIZE - 1);

        // small‐stride stream detection
        int64_t curr_block = paddr >> 6;
        bool    is_stream  = false;
        if (last_block[sig] >= 0) {
            int64_t stride = curr_block - last_block[sig];
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
            stride_run[sig]   = 1;
            last_stride[sig]  = 0;
        }
        last_block[sig] = curr_block;

        // Decide insertion policy: DIP chooses signature‐guided vs baseline SRRIP
        bool choose_sig = use_sig_policy(set);

        // Bypass if DBP or thrash signature
        if (DBP_Predict(sig) || ship_miss_ctr[sig] >= MISS_THRESHOLD) {
            // bypass
            repl_rrpv[set][way] = RRPV_MAX;
            stat_bypasses++;
        }
        else if (choose_sig) {
            // signature‐guided insertion (TASR style)
            if (ship_ctr[sig] >= HOT_THRESHOLD) {
                repl_rrpv[set][way] = 0;                   // hot
            }
            else if (is_stream) {
                repl_rrpv[set][way] = (RRPV_INIT > 0 ? RRPV_INIT-1 : 0);
            }
            else {
                repl_rrpv[set][way] = RRPV_INIT;
            }
        }
        else {
            // baseline SRRIP insertion
            repl_rrpv[set][way] = RRPV_INIT;
        }

        // install metadata
        repl_sig[set][way]     = sig;
        repl_has_hit[set][way] = false;
    }
}

// ----------------------------------------------------------------------------
// Print final statistics
void PrintStats() {
    uint64_t total = stat_hits + stat_misses;
    double   hr    = total ? (100.0 * double(stat_hits) / double(total)) : 0.0;
    std::cout << "==== DDSH Statistics ====\n";
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