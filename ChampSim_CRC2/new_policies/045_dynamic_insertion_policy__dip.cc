#include <vector>
#include <cstdint>
#include <iostream>
#include "../inc/champsim_crc2.h"

#define NUM_CORE         1
#define LLC_SETS         (NUM_CORE * 2048)
#define LLC_WAYS         16

// RRIP parameters
#define RRPV_BITS        2
#define RRPV_MAX         ((1 << RRPV_BITS) - 1)

// Bimodal Insertion parameters
// 1 out of BIP_PROB_DIV inserts near MRU; else near LRU
#define BIP_PROB_DIV     32  

// PSEL (policy selector) parameters
#define PSEL_BITS        10
#define PSEL_MAX         ((1 << PSEL_BITS) - 1)
#define PSEL_THRESHOLD   (PSEL_MAX / 2)

// Replacement state
static uint8_t   repl_rrpv[LLC_SETS][LLC_WAYS];
static uint32_t  psel;
static uint32_t  bip_counter;

// Statistics
static uint64_t  stat_hits;
static uint64_t  stat_misses;

// Helper: RRIP victim selection
static uint32_t FindVictimWay(uint32_t set) {
    // Standard SRRIP victim search
    while (true) {
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            if (repl_rrpv[set][w] == RRPV_MAX)
                return w;
        }
        // Increment RRPVs (aging)
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            if (repl_rrpv[set][w] < RRPV_MAX)
                repl_rrpv[set][w]++;
        }
    }
}

void InitReplacementState() {
    stat_hits     = 0;
    stat_misses   = 0;
    // Initialize all lines to coldest
    for (uint32_t s = 0; s < LLC_SETS; s++) {
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            repl_rrpv[s][w] = RRPV_MAX;
        }
    }
    // Start PSEL at midpoint
    psel          = PSEL_MAX / 2;
    bip_counter   = 0;
}

uint32_t GetVictimInSet(
    uint32_t /*cpu*/,
    uint32_t set,
    const BLOCK */*current_set*/,
    uint64_t /*PC*/,
    uint64_t /*paddr*/,
    uint32_t /*type*/
) {
    // Use RRIP victim selection
    return FindVictimWay(set);
}

void UpdateReplacementState(
    uint32_t /*cpu*/,
    uint32_t set,
    uint32_t way,
    uint64_t /*paddr*/,
    uint64_t /*PC*/,
    uint64_t /*victim_addr*/,
    uint32_t /*type*/,
    uint8_t hit
) {
    if (hit) {
        // On hit: promote to MRU
        stat_hits++;
        repl_rrpv[set][way] = 0;
    } else {
        // On miss: choose insertion policy
        stat_misses++;
        // Determine set role by low bits of set index
        uint32_t low6 = set & (BIP_PROB_DIV - 1);
        bool is_lru_sample = (low6 == 0);
        bool is_bip_sample = (low6 == 1);
        // Update PSEL on sample‐set misses
        if (is_lru_sample) {
            if (psel < PSEL_MAX) psel++;
        } else if (is_bip_sample) {
            if (psel > 0)        psel--;
        }
        // Decide policy for this set
        bool use_bip;
        if (is_lru_sample)       use_bip = false;
        else if (is_bip_sample)  use_bip = true;
        else                     use_bip = (psel >= PSEL_THRESHOLD);
        // Apply insertion
        if (use_bip) {
            // Bimodal RRIP: mostly cold, some near-MRU
            bip_counter++;
            if ((bip_counter & (BIP_PROB_DIV - 1)) == 0) {
                // Near-MRU insertion
                repl_rrpv[set][way] = RRPV_MAX - 1;
            } else {
                // Cold insertion
                repl_rrpv[set][way] = RRPV_MAX;
            }
        } else {
            // LRU insertion: always MRU
            repl_rrpv[set][way] = 0;
        }
    }
}

void PrintStats() {
    uint64_t total = stat_hits + stat_misses;
    double hr = total ? (100.0 * stat_hits / total) : 0.0;
    std::cout << "==== DIP Policy Statistics ====\n";
    std::cout << "Total refs   : " << total       << "\n";
    std::cout << "Hits         : " << stat_hits   << "\n";
    std::cout << "Misses       : " << stat_misses << "\n";
    std::cout << "Hit Rate (%) : " << hr          << "\n";
}

void PrintStats_Heartbeat() {
    PrintStats();
}