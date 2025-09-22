#include <vector>
#include <cstdint>
#include <iostream>
#include "../inc/champsim_crc2.h"

#define NUM_CORE      1
#define LLC_SETS      (NUM_CORE * 2048)
#define LLC_WAYS      16

// RRPV (RRIP) parameters
#define RRPV_BITS     2
#define RRPV_MAX      ((1 << RRPV_BITS) - 1)

// SHiP parameters
#define SHCT_BITS     2                  // 2-bit saturating counters
#define SHCT_SIZE     1024               // must be power of two
#define SHCT_MASK     (SHCT_SIZE - 1)

// Replacement state
static uint8_t  repl_rrpv    [LLC_SETS][LLC_WAYS];
static uint8_t  repl_sig     [LLC_SETS][LLC_WAYS];
static bool     repl_has_hit [LLC_SETS][LLC_WAYS];
static uint8_t  SHCT         [SHCT_SIZE];

// Statistics
static uint64_t stat_hits      = 0;
static uint64_t stat_misses    = 0;
static uint64_t stat_evictions = 0;

// Initialize replacement and predictor state
void InitReplacementState() {
    // Initialize per‐line RRIP and SHiP metadata
    for (uint32_t s = 0; s < LLC_SETS; s++) {
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            repl_rrpv[s][w]    = RRPV_MAX;
            repl_sig[s][w]     = 0;
            repl_has_hit[s][w] = false;
        }
    }
    // Initialize SHCT counters to 1 (weakly‐positive)
    for (uint32_t i = 0; i < SHCT_SIZE; i++) {
        SHCT[i] = 1;
    }
    // Clear stats
    stat_hits = stat_misses = stat_evictions = 0;
}

// Standard RRIP victim selection: find way with RRPV==MAX, aging if needed
uint32_t GetVictimInSet(
    uint32_t cpu,
    uint32_t set,
    const BLOCK *current_set,
    uint64_t PC,
    uint64_t paddr,
    uint32_t type
) {
    while (true) {
        // Look for a candidate
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            if (repl_rrpv[set][w] == RRPV_MAX) {
                return w;
            }
        }
        // Age all entries
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            if (repl_rrpv[set][w] < RRPV_MAX) {
                repl_rrpv[set][w]++;
            }
        }
    }
    // unreachable
    return 0;
}

// Update on hit or miss
void UpdateReplacementState(
    uint32_t cpu,
    uint32_t set,
    uint32_t way,
    uint64_t paddr,
    uint64_t PC,
    uint64_t victim_addr,
    uint32_t type,
    uint8_t hit
) {
    if (hit) {
        // LLC hit: rejuvenate and mark useful
        repl_rrpv[set][way]    = 0;
        repl_has_hit[set][way] = true;
        stat_hits++;
    } else {
        // LLC miss and fill: we are evicting the old line at [set][way]
        stat_misses++;
        stat_evictions++;
        // Update SHCT based on whether evicted block saw a hit
        uint8_t evict_sig = repl_sig[set][way];
        if (repl_has_hit[set][way]) {
            if (SHCT[evict_sig] < ((1 << SHCT_BITS) - 1)) {
                SHCT[evict_sig]++;
            }
        } else {
            if (SHCT[evict_sig] > 0) {
                SHCT[evict_sig]--;
            }
        }
        // Compute signature for the new fill (combine PC and block addr)
        uint32_t sig = (uint32_t)(((PC ^ (paddr >> 6)) & SHCT_MASK));
        // Install new block metadata
        repl_sig[set][way]     = sig;
        repl_has_hit[set][way] = false;
        // Insert hot if SHCT positive, else cold
        if (SHCT[sig] > 0) {
            repl_rrpv[set][way] = 0;
        } else {
            repl_rrpv[set][way] = RRPV_MAX;
        }
    }
}

// Print final statistics
void PrintStats() {
    uint64_t total = stat_hits + stat_misses;
    double   hr    = total ? (double)stat_hits / total * 100.0 : 0.0;
    std::cout << "==== SHiP-RRIP Replacement Stats ====\n";
    std::cout << "Total refs   : " << total         << "\n";
    std::cout << "Hits         : " << stat_hits     << "\n";
    std::cout << "Misses       : " << stat_misses   << "\n";
    std::cout << "Evictions    : " << stat_evictions<< "\n";
    std::cout << "Hit rate (%) : " << hr            << "\n";
}

// Print periodic (heartbeat) statistics
void PrintStats_Heartbeat() {
    PrintStats();
}