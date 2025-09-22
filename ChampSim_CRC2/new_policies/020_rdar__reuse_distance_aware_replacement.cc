#include <vector>
#include <cstdint>
#include <iostream>
#include "../inc/champsim_crc2.h"

#define NUM_CORE      1
#define LLC_SETS      (NUM_CORE * 2048)
#define LLC_WAYS      16

// RRIP parameters
#define RRPV_BITS     3
#define RRPV_MAX      ((1 << RRPV_BITS) - 1)  // 7
#define SRRIP_INIT    (RRPV_MAX - 1)          // 6

// RDCT (Reuse Distance Counter Table) parameters
#define RDCT_CTR_BITS 2
#define RDCT_CTR_MAX  ((1 << RDCT_CTR_BITS) - 1) // 3
#define RDCT_SIZE     8192
#define RDCT_INIT     (RDCT_CTR_MAX >> 1)        // 1

// Threshold for classifying reuse as "short"
#define DIST_THRESHOLD 1024

// Replacement metadata
static uint8_t   repl_rrpv[LLC_SETS][LLC_WAYS];
static bool      repl_has_hit[LLC_SETS][LLC_WAYS];
static uint16_t  repl_sig[LLC_SETS][LLC_WAYS];
static uint64_t  repl_last_ref[LLC_SETS][LLC_WAYS];

// RDCT table
static uint8_t RDCT[RDCT_SIZE];

// Stream detection
static uint64_t last_miss_block;

// Global reference counter to measure reuse intervals
static uint64_t global_ref_counter;

// Statistics
static uint64_t stat_hits, stat_misses, stat_evictions;

// Initialize replacement state
void InitReplacementState() {
    for (uint32_t s = 0; s < LLC_SETS; s++) {
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            repl_rrpv[s][w]      = RRPV_MAX;
            repl_has_hit[s][w]   = false;
            repl_sig[s][w]       = 0;
            repl_last_ref[s][w]  = 0;
        }
    }
    for (uint32_t i = 0; i < RDCT_SIZE; i++) {
        RDCT[i] = RDCT_INIT;
    }
    last_miss_block    = 0;
    global_ref_counter = 0;
    stat_hits          = 0;
    stat_misses        = 0;
    stat_evictions     = 0;
}

// Victim selection: SRRIP scan-and-age
uint32_t GetVictimInSet(
    uint32_t /*cpu*/,
    uint32_t set,
    const BLOCK */* current_set */,
    uint64_t /* PC */,
    uint64_t /* paddr */,
    uint32_t /* type */
) {
    while (true) {
        // find an RRPV == MAX
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            if (repl_rrpv[set][w] == RRPV_MAX) {
                return w;
            }
        }
        // age all entries
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            if (repl_rrpv[set][w] < RRPV_MAX) {
                repl_rrpv[set][w]++;
            }
        }
    }
    return 0; // unreachable
}

// Update replacement state on access
void UpdateReplacementState(
    uint32_t /*cpu*/,
    uint32_t set,
    uint32_t way,
    uint64_t paddr,
    uint64_t PC,
    uint64_t /* victim_addr */,
    uint32_t /* type */,
    uint8_t hit
) {
    // advance global reference counter
    global_ref_counter++;

    if (hit) {
        // ---- HIT PATH ----
        stat_hits++;
        // measure reuse distance
        uint64_t dist = global_ref_counter - repl_last_ref[set][way];
        uint16_t sig  = repl_sig[set][way];
        // update RDCT
        if (dist < DIST_THRESHOLD) {
            if (RDCT[sig] < RDCT_CTR_MAX) {
                RDCT[sig]++;
            }
        } else {
            if (RDCT[sig] > 0) {
                RDCT[sig]--;
            }
        }
        // promote to MRU
        repl_rrpv[set][way]     = 0;
        repl_has_hit[set][way]  = true;
        repl_last_ref[set][way] = global_ref_counter;
    } else {
        // ---- MISS & EVICTION PATH ----
        stat_misses++;
        stat_evictions++;
        // demote old signature if it never hit
        uint16_t old_sig = repl_sig[set][way];
        if (!repl_has_hit[set][way] && RDCT[old_sig] > 0) {
            RDCT[old_sig]--;
        }
        // compute new signature
        uint16_t new_sig = (uint32_t)((PC >> 2) & (RDCT_SIZE - 1));
        repl_sig[set][way]      = new_sig;
        repl_has_hit[set][way]  = false;
        repl_last_ref[set][way] = global_ref_counter;
        // detect simple sequential stream
        uint64_t blk      = (paddr >> 6);
        bool sequential  = (blk == last_miss_block + 1);
        last_miss_block   = blk;
        // insertion policy
        if (sequential) {
            // moderate priority for streams
            repl_rrpv[set][way] = SRRIP_INIT - 1;
        } else {
            uint8_t ctr = RDCT[new_sig];
            if (ctr == RDCT_CTR_MAX) {
                // predicted high reuse
                repl_rrpv[set][way] = 0;
            } else if (ctr == 0) {
                // predicted no reuse: bypass
                repl_rrpv[set][way] = RRPV_MAX;
            } else {
                // default weak insertion
                repl_rrpv[set][way] = SRRIP_INIT;
            }
        }
    }
}

// Print end-of-simulation statistics
void PrintStats() {
    uint64_t total = stat_hits + stat_misses;
    double hr = total ? (double(stat_hits) / double(total) * 100.0) : 0.0;
    std::cout << "==== RDAR Statistics ====\n";
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