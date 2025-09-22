#include <vector>
#include <cstdint>
#include <iostream>
#include "../inc/champsim_crc2.h"

#define NUM_CORE 1
#define LLC_SETS (NUM_CORE * 2048)
#define LLC_WAYS 16

// RRPV parameters
#define RRPV_BITS    3
#define RRPV_MAX     ((1 << RRPV_BITS) - 1) // 7
#define RRPV_INIT    (RRPV_MAX - 1)         // 6

// SHiP signature table
#define SIG_TABLE_SIZE      4096
#define SIG_CTR_BITS        2
#define SIG_CTR_MAX         ((1 << SIG_CTR_BITS) - 1) // 3
#define SIG_CTR_INIT        (SIG_CTR_MAX / 2)         // 1
#define HOT_THRESHOLD       (SIG_CTR_MAX / 2 + 1)      // 2

// Stream detection
#define STREAM_DETECT_COUNT 2
#define MAX_SMALL_STRIDE    8

// Thrash (miss) counters
#define MISS_CTR_BITS       2
#define MISS_CTR_MAX        ((1 << MISS_CTR_BITS) - 1) // 3
#define MISS_THRESHOLD      (MISS_CTR_MAX)             // bypass when saturated

// Phase reset interval (references)
#define RESET_INTERVAL      (1ULL << 20) // ~1M refs

// ----------------------------------------------------------------------------
// Replacement metadata
static uint8_t  repl_rrpv   [LLC_SETS][LLC_WAYS];
static uint16_t repl_sig    [LLC_SETS][LLC_WAYS];
static bool     repl_has_hit[LLC_SETS][LLC_WAYS];

// SHiP tables & stream detection & thrash counters
static uint8_t  ship_ctr     [SIG_TABLE_SIZE];
static uint8_t  ship_miss_ctr[SIG_TABLE_SIZE];
static int64_t  last_block   [SIG_TABLE_SIZE];
static int64_t  last_stride  [SIG_TABLE_SIZE];
static uint8_t  stride_run   [SIG_TABLE_SIZE];

// Statistics
static uint64_t stat_hits, stat_misses, stat_evictions, stat_bypasses;
static uint64_t ref_count;

// ----------------------------------------------------------------------------
// Initialize replacement state
void InitReplacementState() {
    for (uint32_t s = 0; s < LLC_SETS; s++) {
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            repl_rrpv[s][w]    = RRPV_MAX;
            repl_sig[s][w]     = 0;
            repl_has_hit[s][w] = false;
        }
    }
    for (uint32_t i = 0; i < SIG_TABLE_SIZE; i++) {
        ship_ctr[i]       = SIG_CTR_INIT;
        ship_miss_ctr[i]  = 0;
        last_block[i]     = -1;
        last_stride[i]    = 0;
        stride_run[i]     = 0;
    }
    stat_hits      = stat_misses = stat_evictions = stat_bypasses = 0;
    ref_count      = 0;
}

// Victim selection using SRRIP
uint32_t GetVictimInSet(
    uint32_t /*cpu*/,
    uint32_t set,
    const BLOCK */*current_set*/,
    uint64_t /*PC*/,
    uint64_t /*paddr*/,
    uint32_t /*type*/
) {
    while (true) {
        // Look for RRPV == max
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            if (repl_rrpv[set][w] == RRPV_MAX)
                return w;
        }
        // Age all entries
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            if (repl_rrpv[set][w] < RRPV_MAX)
                repl_rrpv[set][w]++;
        }
    }
    return 0; // unreachable
}

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
    // Phase reset: decay SHiP and clear miss counters periodically
    ref_count++;
    if ((ref_count & (RESET_INTERVAL - 1)) == 0) {
        for (uint32_t i = 0; i < SIG_TABLE_SIZE; i++) {
            // decay reuse counters by half to adapt to new phase
            ship_ctr[i] >>= 1;
            ship_miss_ctr[i] = 0;
        }
    }

    if (hit) {
        // ---- HIT ----
        stat_hits++;
        uint16_t psig = repl_sig[set][way];
        if (!repl_has_hit[set][way]) {
            // first hit updates reuse & clear any thrash history
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
        // update thrash counter on evicted block
        uint16_t old_sig = repl_sig[set][way];
        bool old_hit     = repl_has_hit[set][way];
        if (!old_hit) {
            if (ship_miss_ctr[old_sig] < MISS_CTR_MAX)
                ship_miss_ctr[old_sig]++;
        }
        // also decay reuse if eviction without any hit
        if (!old_hit && ship_ctr[old_sig] > 0)
            ship_ctr[old_sig]--;

        // compute new signature
        uint16_t sig = ((PC >> 2) ^ (PC >> 12)) & (SIG_TABLE_SIZE - 1);

        // small‐stride stream detection
        int64_t curr_block = paddr >> 6;
        bool is_stream = false;
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
            stride_run[sig]  = 1;
            last_stride[sig] = 0;
        }
        last_block[sig] = curr_block;

        // insertion decision
        if (ship_ctr[sig] >= HOT_THRESHOLD) {
            // hot → MRU
            repl_rrpv[set][way] = 0;
        }
        else if (is_stream) {
            // steady small‐stride → near‐MRU
            repl_rrpv[set][way] = (RRPV_INIT > 0 ? RRPV_INIT - 1 : 0);
        }
        else if (ship_miss_ctr[sig] >= MISS_THRESHOLD) {
            // thrashing PC → bypass
            repl_rrpv[set][way] = RRPV_MAX;
            stat_bypasses++;
        }
        else {
            // default insertion (SRRIP)
            repl_rrpv[set][way] = RRPV_INIT;
        }

        // install metadata for new block
        repl_sig[set][way]     = sig;
        repl_has_hit[set][way] = false;
    }
}

// Print final statistics
void PrintStats() {
    uint64_t total = stat_hits + stat_misses;
    double hr = total ? (100.0 * double(stat_hits) / double(total)) : 0.0;
    std::cout << "==== TASR Statistics ====\n";
    std::cout << "Total refs    : " << total          << "\n";
    std::cout << "Hits          : " << stat_hits      << "\n";
    std::cout << "Misses        : " << stat_misses    << "\n";
    std::cout << "Evictions     : " << stat_evictions << "\n";
    std::cout << "Bypasses      : " << stat_bypasses  << "\n";
    std::cout << "Hit rate (%)  : " << hr             << "\n";
}

// Print heartbeat stats
void PrintStats_Heartbeat() {
    PrintStats();
}