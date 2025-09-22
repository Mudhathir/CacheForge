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

// DRRIP/DIP parameters
#define PSEL_BITS        10
#define PSEL_MAX         ((1 << PSEL_BITS) - 1)
#define PSEL_INIT        (PSEL_MAX >> 1)
#define N_LEADER_SETS    64
#define LEADER_INTERVAL  (LLC_SETS / (2 * N_LEADER_SETS))
#define BIP_PROB         32   // 1/32 chance of mid‐range insertion in BRRIP

// Replacement metadata
static uint8_t repl_rrpv[LLC_SETS][LLC_WAYS];
// Global policy state
static uint32_t PSEL;
static uint64_t stat_hits;
static uint64_t stat_misses;

// Find a victim by classic SRRIP eviction
static uint32_t FindVictimWay(uint32_t set) {
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
}

void InitReplacementState() {
    stat_hits   = 0;
    stat_misses = 0;
    PSEL        = PSEL_INIT;
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
        // On hit: always promote to MRU (RRPV=0)
        stat_hits++;
        repl_rrpv[set][way] = 0;
    } else {
        // On miss: update miss stats and PSEL if in a leader set
        stat_misses++;
        bool is_srrip_leader = (set % (2 * LEADER_INTERVAL) == 0);
        bool is_brrip_leader = (set % (2 * LEADER_INTERVAL) == LEADER_INTERVAL);
        if (is_srrip_leader) {
            if (PSEL > 0) PSEL--;
        } else if (is_brrip_leader) {
            if (PSEL < PSEL_MAX) PSEL++;
        }
        // Determine which insertion policy to use
        bool use_srrip;
        if (is_srrip_leader) {
            use_srrip = true;
        } else if (is_brrip_leader) {
            use_srrip = false;
        } else {
            use_srrip = (PSEL > PSEL_INIT);
        }
        // Insert new block
        if (use_srrip) {
            // SRRIP: mid‐range insertion
            repl_rrpv[set][way] = RRPV_MAX - 1;
        } else {
            // BRRIP: far‐LRU insertion with rare mid‐range promotion
            if ((PC & (BIP_PROB - 1)) == 0)
                repl_rrpv[set][way] = RRPV_MAX - 1;
            else
                repl_rrpv[set][way] = RRPV_MAX;
        }
    }
}

void PrintStats() {
    uint64_t total = stat_hits + stat_misses;
    double hr = total ? (100.0 * stat_hits / total) : 0.0;
    std::cout << "==== DRRIP Policy Statistics ====\n";
    std::cout << "Total refs   : " << total       << "\n";
    std::cout << "Hits         : " << stat_hits   << "\n";
    std::cout << "Misses       : " << stat_misses << "\n";
    std::cout << "Hit Rate (%) : " << hr          << "\n";
}

void PrintStats_Heartbeat() {
    PrintStats();
}