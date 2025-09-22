#include <vector>
#include <cstdint>
#include <iostream>
#include "../inc/champsim_crc2.h"

#define NUM_CORE       1
#define LLC_SETS       (NUM_CORE * 2048)
#define LLC_WAYS       16

// RRIP parameters
#define RRPV_BITS      2
#define RRPV_MAX       ((1 << RRPV_BITS) - 1)  // 3
#define RRPV_INIT_MRU  0                       // MRU insertion
#define RRPV_INIT_LRU  RRPV_MAX                // near-LRU insertion

// DRRIP (DIP + Bimodal RRIP) parameters
#define PSEL_BITS      10
#define PSEL_MAX       ((1 << PSEL_BITS) - 1)  // 1023
#define PSEL_INIT      (PSEL_MAX / 2)          // start neutral
#define BIP_PROB       32                       // 1/32 MRU in BIP

// Sample‐set selection: ~32 sets each out of 2048
static inline bool IsSampleLRU(uint32_t set) { return (set % 64) == 0; }
static inline bool IsSampleBIP(uint32_t set) { return (set % 64) == 1; }

// Replacement state
static uint8_t  repl_rrpv[LLC_SETS][LLC_WAYS];
static int      PSEL;
static uint64_t stat_hits, stat_misses;

// Find a victim via RRIP scan/aging
static inline uint32_t FindVictim(uint32_t set) {
    while (true) {
        for (uint32_t w = 0; w < LLC_WAYS; ++w) {
            if (repl_rrpv[set][w] == RRPV_MAX)
                return w;
        }
        // Age all
        for (uint32_t w = 0; w < LLC_WAYS; ++w) {
            if (repl_rrpv[set][w] < RRPV_MAX)
                repl_rrpv[set][w]++;
        }
    }
}

// Initialize replacement state
void InitReplacementState() {
    PSEL = PSEL_INIT;
    stat_hits = stat_misses = 0;
    for (uint32_t s = 0; s < LLC_SETS; ++s) {
        for (uint32_t w = 0; w < LLC_WAYS; ++w) {
            repl_rrpv[s][w] = RRPV_INIT_LRU;
        }
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
    return FindVictim(set);
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
        // On hit: always promote to MRU
        stat_hits++;
        repl_rrpv[set][way] = RRPV_INIT_MRU;
        // Update PSEL for sample sets
        if (IsSampleLRU(set)) {
            if (PSEL < PSEL_MAX) PSEL++;
        } else if (IsSampleBIP(set)) {
            if (PSEL > 0)       PSEL--;
        }
    } else {
        // On miss: insertion decision
        stat_misses++;
        bool use_lru;
        if (IsSampleLRU(set)) {
            use_lru = true;
        } else if (IsSampleBIP(set)) {
            use_lru = false;
        } else {
            // followers adopt the policy favored by PSEL
            use_lru = (PSEL >= (PSEL_MAX / 2));
        }
        if (use_lru) {
            // strict MRU insertion
            repl_rrpv[set][way] = RRPV_INIT_MRU;
        } else {
            // Bimodal insertion: mostly near-LRU
            uint32_t rnd = ((PC ^ paddr) >> 3) & (BIP_PROB - 1);
            if (rnd == 0)
                repl_rrpv[set][way] = RRPV_INIT_MRU;
            else
                repl_rrpv[set][way] = RRPV_INIT_LRU;
        }
    }
}

// Print end-of-simulation statistics
void PrintStats() {
    uint64_t total = stat_hits + stat_misses;
    double hr = total ? (100.0 * stat_hits / total) : 0.0;
    std::cout << "==== DRRIP Policy Statistics ====\n";
    std::cout << "PSEL         : " << PSEL         << "\n";
    std::cout << "Total refs   : " << total        << "\n";
    std::cout << "Hits         : " << stat_hits    << "\n";
    std::cout << "Misses       : " << stat_misses  << "\n";
    std::cout << "Hit Rate (%) : " << hr           << "\n";
}

// Print periodic (heartbeat) statistics
void PrintStats_Heartbeat() {
    PrintStats();
}