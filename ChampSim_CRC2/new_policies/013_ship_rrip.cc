#include <vector>
#include <cstdint>
#include <iostream>
#include "../inc/champsim_crc2.h"

#define NUM_CORE          1
#define LLC_SETS          (NUM_CORE * 2048)
#define LLC_WAYS          16

// RRIP parameters
#define RRPV_BITS         2
#define RRPV_MAX          ((1 << RRPV_BITS) - 1)  // 3
#define SRRIP_INIT        (RRPV_MAX - 1)          // 2

// Signature-based Hit Counter Table (SHCT) parameters
#define SHCT_BITS         2
#define SHCT_SIZE         (1 << SHCT_BITS)        // 4 states: 0..3
#define SHCT_MAX          (SHCT_SIZE - 1)         // 3
#define SHCT_INIT         1                       // initial weakly‐taken
#define SHCT_THRESHOLD    2                       // >=2 means “reuse‐likely”

// Per-PC signature table size (must be power of two)
#define PHT_ENTRIES       16384
#define PHT_MASK          (PHT_ENTRIES - 1)
#define PHT_INDEX(pc)     (((pc) >> 4) & PHT_MASK)

// Replacement state
static uint8_t   repl_rrpv   [LLC_SETS][LLC_WAYS];
static uint16_t  repl_sig    [LLC_SETS][LLC_WAYS];
static bool      repl_has_hit[LLC_SETS][LLC_WAYS];
static uint8_t   SHCT        [PHT_ENTRIES];

// Statistics
static uint64_t stat_hits      = 0;
static uint64_t stat_misses    = 0;
static uint64_t stat_evictions = 0;

// Initialize replacement state and SHCT
void InitReplacementState() {
    for (uint32_t s = 0; s < LLC_SETS; s++) {
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            repl_rrpv[s][w]    = RRPV_MAX;
            repl_sig[s][w]     = 0;
            repl_has_hit[s][w] = false;
        }
    }
    for (uint32_t i = 0; i < PHT_ENTRIES; i++) {
        SHCT[i] = SHCT_INIT;
    }
    stat_hits      = 0;
    stat_misses    = 0;
    stat_evictions = 0;
}

// Find victim in the set using standard RRIP aging
uint32_t GetVictimInSet(
    uint32_t cpu,
    uint32_t set,
    const BLOCK *current_set,
    uint64_t PC,
    uint64_t paddr,
    uint32_t type
) {
    while (true) {
        // Look for an entry with RRPV == RRPV_MAX
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
        // On hit: promote to MRU, update PC's SHCT, mark referenced
        stat_hits++;
        repl_rrpv[set][way]     = 0;
        repl_has_hit[set][way]  = true;
        uint16_t sig = repl_sig[set][way];
        if (SHCT[sig] < SHCT_MAX) {
            SHCT[sig]++;
        }
    } else {
        // On miss & eviction: update stats
        stat_misses++;
        stat_evictions++;
        // Feedback: adjust SHCT for the evicted block's signature
        uint16_t old_sig = repl_sig[set][way];
        if (repl_has_hit[set][way]) {
            // block was reused => strengthen
            if (SHCT[old_sig] < SHCT_MAX) {
                SHCT[old_sig]++;
            }
        } else {
            // block was never hit => weaken
            if (SHCT[old_sig] > 0) {
                SHCT[old_sig]--;
            }
        }
        // Now insert new block: compute its signature
        uint16_t sig = PHT_INDEX(PC);
        repl_sig[set][way]     = sig;
        repl_has_hit[set][way] = false;
        // Choose insertion RRPV based on SHCT prediction
        if (SHCT[sig] >= SHCT_THRESHOLD) {
            // predicted heavy reuse
            repl_rrpv[set][way] = 0;
        }
        else if (SHCT[sig] == 0) {
            // predicted streaming/no reuse => bypass
            repl_rrpv[set][way] = RRPV_MAX;
        }
        else {
            // moderate uncertainty => SRRIP insertion
            repl_rrpv[set][way] = SRRIP_INIT;
        }
    }
}

// Print end-of-simulation statistics
void PrintStats() {
    uint64_t total = stat_hits + stat_misses;
    double   hr    = total ? (double)stat_hits / total * 100.0 : 0.0;
    std::cout << "==== SHiP-RRIP Statistics ====\n";
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