#include <vector>
#include <cstdint>
#include <iostream>
#include "../inc/champsim_crc2.h"

#define NUM_CORE 1
#define LLC_SETS (NUM_CORE * 2048)
#define LLC_WAYS 16

// RRIP parameters
#define RRPV_BITS    3
#define RRPV_MAX     ((1 << RRPV_BITS) - 1)  // 7
#define SRRIP_INIT   (RRPV_MAX - 1)          // 6

// SHCT parameters
#define SHCT_CTR_BITS    3
#define SHCT_CTR_MAX     ((1 << SHCT_CTR_BITS) - 1)  // 7
#define SHCT_INIT        (SHCT_CTR_MAX >> 1)         // 3
#define SHCT_THRESHOLD   SHCT_INIT

// Signature table size (power of two)
#define SHCT_SIZE    4096

// Replacement state arrays
static uint8_t  repl_rrpv[LLC_SETS][LLC_WAYS];
static bool     repl_has_hit[LLC_SETS][LLC_WAYS];
static uint16_t repl_sig[LLC_SETS][LLC_WAYS];

// Signature History Counter Table
static uint8_t SHCT[SHCT_SIZE];

// Statistics
static uint64_t stat_hits, stat_misses, stat_evictions;

void InitReplacementState() {
    // Initialize RRIP state and per‐block metadata
    for (uint32_t s = 0; s < LLC_SETS; s++) {
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            repl_rrpv[s][w]     = RRPV_MAX;
            repl_has_hit[s][w]  = false;
            repl_sig[s][w]      = 0;
        }
    }
    // Initialize SHCT to neutral value
    for (uint32_t i = 0; i < SHCT_SIZE; i++) {
        SHCT[i] = SHCT_INIT;
    }
    // Reset statistics
    stat_hits      = 0;
    stat_misses    = 0;
    stat_evictions = 0;
}

uint32_t GetVictimInSet(
    uint32_t cpu,
    uint32_t set,
    const BLOCK *current_set,
    uint64_t PC,
    uint64_t paddr,
    uint32_t type
) {
    // Standard SRRIP victim selection
    while (true) {
        // Look for an RRPV == MAX
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            if (repl_rrpv[set][w] == RRPV_MAX) {
                return w;
            }
        }
        // Age all blocks
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            if (repl_rrpv[set][w] < RRPV_MAX) {
                repl_rrpv[set][w]++;
            }
        }
    }
    return 0; // unreachable
}

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
        // On hit: promote to MRU and update SHCT
        stat_hits++;
        repl_rrpv[set][way]    = 0;
        repl_has_hit[set][way] = true;
        uint16_t sig = repl_sig[set][way];
        if (SHCT[sig] < SHCT_CTR_MAX) {
            SHCT[sig]++;
        }
    } else {
        // On miss: eviction and insertion
        stat_misses++;
        stat_evictions++;
        // Update SHCT based on victim block's signature and reuse flag
        uint16_t old_sig = repl_sig[set][way];
        if (!repl_has_hit[set][way] && SHCT[old_sig] > 0) {
            SHCT[old_sig]--;
        }
        // Compute new block's signature from PC
        uint16_t new_sig = (uint32_t)((PC >> 2) & (SHCT_SIZE - 1));
        repl_sig[set][way]     = new_sig;
        repl_has_hit[set][way] = false;
        // Insertion decision based on SHCT counter
        uint8_t ctr = SHCT[new_sig];
        if (ctr > SHCT_THRESHOLD) {
            // Strong reuse → MRU insertion
            repl_rrpv[set][way] = 0;
        } else if (ctr == 0) {
            // Likely streaming → bypass
            repl_rrpv[set][way] = RRPV_MAX;
        } else {
            // Uncertain → SRRIP default
            repl_rrpv[set][way] = SRRIP_INIT;
        }
    }
}

void PrintStats() {
    uint64_t total = stat_hits + stat_misses;
    double   hr    = total ? double(stat_hits) / double(total) * 100.0 : 0.0;
    std::cout << "==== SHiP-RRIP Statistics ====\n";
    std::cout << "Total refs   : " << total        << "\n";
    std::cout << "Hits         : " << stat_hits    << "\n";
    std::cout << "Misses       : " << stat_misses  << "\n";
    std::cout << "Evictions    : " << stat_evictions << "\n";
    std::cout << "Hit rate (%) : " << hr           << "\n";
}

void PrintStats_Heartbeat() {
    PrintStats();
}