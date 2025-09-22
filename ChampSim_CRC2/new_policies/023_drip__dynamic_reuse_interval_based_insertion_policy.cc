#include <vector>
#include <cstdint>
#include <iostream>
#include "../inc/champsim_crc2.h"

#define NUM_CORE 1
#define LLC_SETS (NUM_CORE * 2048)
#define LLC_WAYS 16

// RRIP parameters
#define RRPV_BITS       3
#define RRPV_MAX        ((1 << RRPV_BITS) - 1)  // 7
#define RRPV_INIT       (RRPV_MAX - 1)          // 6

// Signature table size (power of two)
#define SIG_TABLE_SIZE  4096

// Thresholds for reuse‐interval and stream detection
#define HOT_THRESHOLD        64
#define MEDIUM_THRESHOLD     512
#define STREAM_DETECT_COUNT  2
#define MAX_SMALL_STRIDE     8

// Replacement metadata
static uint8_t  repl_rrpv   [LLC_SETS][LLC_WAYS];
static uint16_t repl_sig    [LLC_SETS][LLC_WAYS];
static bool     repl_has_hit[LLC_SETS][LLC_WAYS];

// Per‐signature reuse & stride tracking
static uint64_t last_access [SIG_TABLE_SIZE];
static int64_t  last_block  [SIG_TABLE_SIZE];
static int64_t  last_stride [SIG_TABLE_SIZE];
static uint8_t  stride_run  [SIG_TABLE_SIZE];

// Global timestamp and stats
static uint64_t global_timestamp;
static uint64_t stat_hits, stat_misses, stat_evictions;

// Initialize replacement state
void InitReplacementState() {
    for (uint32_t s = 0; s < LLC_SETS; s++) {
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            repl_rrpv[s][w]       = RRPV_MAX;
            repl_sig[s][w]        = 0;
            repl_has_hit[s][w]    = false;
        }
    }
    for (uint32_t i = 0; i < SIG_TABLE_SIZE; i++) {
        last_access[i] = 0;
        last_block[i]  = -1;
        last_stride[i] = 0;
        stride_run[i]  = 0;
    }
    global_timestamp = 0;
    stat_hits = stat_misses = stat_evictions = 0;
}

// Victim selection: SRRIP scan-and-age
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
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            if (repl_rrpv[set][w] < RRPV_MAX)
                repl_rrpv[set][w]++;
        }
    }
    return 0; // unreachable
}

// Update replacement state on every access
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
    // Advance global time (approximate reference count)
    global_timestamp++;

    if (hit) {
        // ---- HIT ----
        stat_hits++;
        repl_rrpv[set][way]    = 0;  // MRU
        repl_has_hit[set][way] = true;
        // refresh reuse epoch
        uint16_t sig = repl_sig[set][way];
        last_access[sig] = global_timestamp;
    } else {
        // ---- MISS & EVICTION ----
        stat_misses++;
        stat_evictions++;
        // compute PC signature
        uint16_t sig = (PC >> 2) & (SIG_TABLE_SIZE - 1);

        // Stride‐based streaming detection
        int64_t curr_block = paddr >> 6;
        if (last_block[sig] >= 0) {
            int64_t stride = curr_block - last_block[sig];
            if (stride == last_stride[sig]) {
                stride_run[sig]++;
            } else {
                last_stride[sig] = stride;
                stride_run[sig]  = 1;
            }
        } else {
            // first sighting
            last_stride[sig] = 0;
            stride_run[sig]  = 1;
        }
        last_block[sig] = curr_block;

        // Reuse‐interval estimation
        uint64_t last_time = last_access[sig];
        uint64_t reuse_dist = last_time
            ? (global_timestamp - last_time)
            : (uint64_t)(-1);

        // Insertion policy
        if (stride_run[sig] >= STREAM_DETECT_COUNT
            && last_stride[sig] >= -MAX_SMALL_STRIDE
            && last_stride[sig] <=  MAX_SMALL_STRIDE)
        {
            // small‐stride stream → moderate retention
            repl_rrpv[set][way] = RRPV_INIT - 1;
        }
        else if (reuse_dist <= HOT_THRESHOLD) {
            // hot PC → strong promotion
            repl_rrpv[set][way] = 0;
        }
        else if (reuse_dist <= MEDIUM_THRESHOLD) {
            // moderate reuse → default SRRIP insertion
            repl_rrpv[set][way] = RRPV_INIT;
        }
        else {
            // cold or unseen → bypass
            repl_rrpv[set][way] = RRPV_MAX;
        }

        // install metadata
        repl_sig[set][way]     = sig;
        repl_has_hit[set][way] = false;
        last_access[sig]       = global_timestamp;
    }
}

// Print end‐of‐simulation statistics
void PrintStats() {
    uint64_t total = stat_hits + stat_misses;
    double hr = total
        ? (double(stat_hits) * 100.0 / double(total))
        : 0.0;
    std::cout << "==== DRIP Statistics ====\n";
    std::cout << "Total refs   : " << total          << "\n";
    std::cout << "Hits         : " << stat_hits      << "\n";
    std::cout << "Misses       : " << stat_misses    << "\n";
    std::cout << "Evictions    : " << stat_evictions << "\n";
    std::cout << "Hit rate (%) : " << hr             << "\n";
}

// Print periodic (heartbeat) statistics
void PrintStats_Heartbeat() {
    PrintStats();
}