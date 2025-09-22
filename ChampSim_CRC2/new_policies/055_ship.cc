#include <vector>
#include <cstdint>
#include <iostream>
#include "../inc/champsim_crc2.h"

#define NUM_CORE        1
#define LLC_SETS        (NUM_CORE * 2048)
#define LLC_WAYS        16

// RRIP parameters
#define RRPV_BITS       2
#define RRPV_MAX        ((1 << RRPV_BITS) - 1)  // e.g., 3
#define REUSE_RRPV_INIT 0                       // MRU for predicted reusable
#define STREAM_RRPV_INIT (RRPV_MAX - 1)         // near-eviction for streaming

// SHiP parameters
#define SHCT_SIZE       1024                   // must be power-of-two
#define SHCT_CTR_MAX    7
#define SHCT_INIT       1                      // weakly not-reuse

// Replacement state
static uint8_t repl_rrpv   [LLC_SETS][LLC_WAYS];
static uint8_t block_sig   [LLC_SETS][LLC_WAYS];
static uint8_t block_refd  [LLC_SETS][LLC_WAYS];
static uint8_t SHCT        [SHCT_SIZE];

// Statistics
static uint64_t stat_hits;
static uint64_t stat_misses;

// Find a victim way by RRIP scan/aging
static inline uint32_t FindVictimWay(uint32_t set) {
    while (true) {
        for (uint32_t w = 0; w < LLC_WAYS; ++w) {
            if (repl_rrpv[set][w] == RRPV_MAX)
                return w;
        }
        // age all if no candidate
        for (uint32_t w = 0; w < LLC_WAYS; ++w) {
            if (repl_rrpv[set][w] < RRPV_MAX)
                repl_rrpv[set][w]++;
        }
    }
}

// Initialize replacement state
void InitReplacementState() {
    stat_hits      = 0;
    stat_misses    = 0;
    // init RRPVs, signatures, ref bits
    for (uint32_t s = 0; s < LLC_SETS; ++s) {
        for (uint32_t w = 0; w < LLC_WAYS; ++w) {
            repl_rrpv[s][w]  = RRPV_MAX;
            block_sig[s][w]  = 0;
            block_refd[s][w] = 0;
        }
    }
    // init SHCT
    for (uint32_t i = 0; i < SHCT_SIZE; ++i) {
        SHCT[i] = SHCT_INIT;
    }
}

// Choose a victim way in the set
uint32_t GetVictimInSet(
    uint32_t /*cpu*/,
    uint32_t set,
    const BLOCK */*current_set*/,
    uint64_t /*PC*/,
    uint64_t /*paddr*/,
    uint32_t /*type*/
) {
    return FindVictimWay(set);
}

// Update replacement state on each reference
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
    if (hit) {
        // On hit: promote to MRU and record re-reference
        stat_hits++;
        repl_rrpv[set][way]    = REUSE_RRPV_INIT;
        block_refd[set][way]   = 1;
    } else {
        // On miss/insertion: update stats
        stat_misses++;
        // 1) update SHCT for the evicted block
        uint8_t old_sig = block_sig[set][way];
        uint8_t was_ref = block_refd[set][way];
        uint8_t ctr     = SHCT[old_sig];
        if (was_ref) {
            if (ctr < SHCT_CTR_MAX) SHCT[old_sig]++;
        } else {
            if (ctr > 0)           SHCT[old_sig]--;
        }
        // 2) compute insertion signature
        uint32_t sig = (uint32_t)((PC ^ (paddr >> 12)) & (SHCT_SIZE - 1));
        block_sig[set][way]  = (uint8_t)sig;
        block_refd[set][way] = 0;
        // 3) insert with RRPV based on prediction
        if (SHCT[sig] > (SHCT_CTR_MAX >> 1)) {
            // predicted hot
            repl_rrpv[set][way] = REUSE_RRPV_INIT;
        } else {
            // predicted cold
            repl_rrpv[set][way] = STREAM_RRPV_INIT;
        }
    }
}

// Print end-of-simulation statistics
void PrintStats() {
    uint64_t total = stat_hits + stat_misses;
    double hr = total ? (100.0 * stat_hits / total) : 0.0;
    std::cout << "==== SHiP Policy Statistics ====\n";
    std::cout << "Total refs   : " << total       << "\n";
    std::cout << "Hits         : " << stat_hits   << "\n";
    std::cout << "Misses       : " << stat_misses << "\n";
    std::cout << "Hit Rate (%) : " << hr          << "\n";
}

// Print periodic (heartbeat) statistics
void PrintStats_Heartbeat() {
    PrintStats();
}