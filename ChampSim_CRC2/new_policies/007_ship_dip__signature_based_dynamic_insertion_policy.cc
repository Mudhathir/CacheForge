#include <vector>
#include <cstdint>
#include <iostream>
#include "../inc/champsim_crc2.h"

#define NUM_CORE       1
#define LLC_SETS       (NUM_CORE * 2048)
#define LLC_WAYS       16

// RRIP parameters
#define RRPV_BITS      2
#define RRPV_MAX       ((1 << RRPV_BITS) - 1)

// SHiP parameters
#define SHCT_CTR_BITS  2
#define SHCT_MAX       ((1 << SHCT_CTR_BITS) - 1)   // 0..3
#define SHCT_INIT      (SHCT_MAX / 2)               // start at 1
#define SHCT_SIZE      1024                         // must be power of two

// Bimodal Insertion rate: 1 in BIP_RATE inserted as MRU
#define BIP_RATE       32

// Replacement state
static uint8_t  repl_rrpv   [LLC_SETS][LLC_WAYS];
static uint8_t  repl_sig    [LLC_SETS][LLC_WAYS];
static bool     repl_reuse  [LLC_SETS][LLC_WAYS];
static uint8_t  SHCT        [SHCT_SIZE];
static uint32_t bip_counter;

// Statistics
static uint64_t stat_hits      = 0;
static uint64_t stat_misses    = 0;
static uint64_t stat_evictions = 0;

// Initialize replacement state
void InitReplacementState() {
    // Initialize RRPVs, signatures, reuse flags
    for (uint32_t s = 0; s < LLC_SETS; s++) {
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            repl_rrpv[s][w]   = RRPV_MAX;
            repl_reuse[s][w]  = false;
            repl_sig[s][w]    = 0;
        }
    }
    // Initialize SHCT
    for (uint32_t i = 0; i < SHCT_SIZE; i++) {
        SHCT[i] = SHCT_INIT;
    }
    bip_counter     = 0;
    stat_hits       = 0;
    stat_misses     = 0;
    stat_evictions  = 0;
}

// Victim selection using RRIP
uint32_t GetVictimInSet(
    uint32_t cpu,
    uint32_t set,
    const BLOCK *current_set,
    uint64_t PC,
    uint64_t paddr,
    uint32_t type
) {
    // Search for a line with RRPV == MAX
    while (true) {
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            if (repl_rrpv[set][w] == RRPV_MAX) {
                return w;
            }
        }
        // Age all lines if none at MAX
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            if (repl_rrpv[set][w] < RRPV_MAX) {
                repl_rrpv[set][w]++;
            }
        }
    }
    return 0; // unreachable
}

// Update replacement state on hit or miss
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
        // On hit: promote to MRU and mark reuse
        stat_hits++;
        repl_rrpv[set][way]  = 0;
        repl_reuse[set][way] = true;
    } else {
        // On miss: we have just evicted 'way'
        stat_misses++;
        stat_evictions++;
        // 1) Update SHCT based on whether the evicted line saw a hit
        uint8_t old_sig = repl_sig[set][way];
        if (repl_reuse[set][way]) {
            if (SHCT[old_sig] < SHCT_MAX) {
                SHCT[old_sig]++;
            }
        } else {
            if (SHCT[old_sig] > 0) {
                SHCT[old_sig]--;
            }
        }
        // 2) Compute new signature and reset reuse flag
        uint8_t signature = (uint32_t)(PC) & (SHCT_SIZE - 1);
        repl_sig[set][way]    = signature;
        repl_reuse[set][way]  = false;
        // 3) Hybrid insertion: SRRIP for hot PCs, BIP for cold PCs
        if (SHCT[signature] >= (SHCT_INIT + 1)) {
            // Hot: MRU insertion
            repl_rrpv[set][way] = 0;
        } else {
            // Cold: Bimodal Insertion
            bip_counter++;
            if ((bip_counter & (BIP_RATE - 1)) == 0) {
                // Rarely give MRU chance
                repl_rrpv[set][way] = 0;
            } else {
                // Most get the worst RRPV to avoid pollution
                repl_rrpv[set][way] = RRPV_MAX;
            }
        }
    }
}

// Print end-of-simulation statistics
void PrintStats() {
    uint64_t total = stat_hits + stat_misses;
    double   hr    = total ? (double)stat_hits / total * 100.0 : 0.0;
    std::cout << "==== SHiP-DIP Replacement Stats ====\n";
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