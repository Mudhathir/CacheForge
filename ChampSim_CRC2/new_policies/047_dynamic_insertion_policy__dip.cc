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

// Set dueling parameters
#define DUELED_SET_BITS  6                              // 64‐set granularity
#define LEADER_MASK      ((1 << DUELED_SET_BITS) - 1)

// PSEL (policy selection) parameters
#define PSEL_BITS        10
#define PSEL_MAX         ((1 << PSEL_BITS) - 1)
#define PSEL_INIT        (PSEL_MAX / 2)

// Bimodal Insertion Probability (1/BIP_PROB MRU inserts)
#define BIP_PROB         32

// Replacement metadata
static uint8_t  repl_rrpv[LLC_SETS][LLC_WAYS]; // RRPV per block

// Statistics
static uint64_t stat_hits;
static uint64_t stat_misses;

// Global policy selector
static uint32_t psel;

// Helper: identify leader sets
static inline bool is_lru_leader(uint32_t set) {
    return (set & LEADER_MASK) == 0;
}
static inline bool is_bip_leader(uint32_t set) {
    return (set & LEADER_MASK) == LEADER_MASK;
}

// SRRIP victim selection
static uint32_t FindVictimWay(uint32_t set) {
    while (true) {
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            if (repl_rrpv[set][w] == RRPV_MAX)
                return w;
        }
        // Age all candidates
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            if (repl_rrpv[set][w] < RRPV_MAX)
                repl_rrpv[set][w]++;
        }
    }
}

void InitReplacementState() {
    stat_hits   = 0;
    stat_misses = 0;
    psel        = PSEL_INIT;
    for (uint32_t s = 0; s < LLC_SETS; s++) {
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            repl_rrpv[s][w] = RRPV_MAX;
        }
    }
}

uint32_t GetVictimInSet(
    uint32_t /*cpu*/,
    uint32_t set,
    const BLOCK */*current_set*/,
    uint64_t /*PC*/,
    uint64_t /*paddr*/,
    uint32_t /*type*/
) {
    // Standard SRRIP victim selection
    return FindVictimWay(set);
}

void UpdateReplacementState(
    uint32_t /*cpu*/,
    uint32_t set,
    uint32_t way,
    uint64_t /*paddr*/,
    uint64_t PC,
    uint64_t /*victim_addr*/,
    uint32_t /*type*/,
    uint8_t hit
) {
    if (hit) {
        // On hit: always promote to MRU
        stat_hits++;
        repl_rrpv[set][way] = 0;
    } else {
        // On miss: choose insertion policy
        stat_misses++;
        bool use_LRU = false;

        if (is_lru_leader(set)) {
            // LRU leader: force LRU insertion, decrement PSEL
            use_LRU = true;
            if (psel > 0) psel--;
        }
        else if (is_bip_leader(set)) {
            // BIP leader: force BIP insertion, increment PSEL
            use_LRU = false;
            if (psel < PSEL_MAX) psel++;
        }
        else {
            // Follower: pick policy based on PSEL
            use_LRU = (psel < PSEL_INIT);
        }

        if (use_LRU) {
            // LRU insertion: RRPV = 0 (MRU)
            repl_rrpv[set][way] = 0;
        } else {
            // Bimodal insertion: most get RRPV = MAX-1, 1/BIP_PROB get MRU
            if ((PC & (BIP_PROB - 1)) == 0)
                repl_rrpv[set][way] = 0;
            else
                repl_rrpv[set][way] = RRPV_MAX - 1;
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