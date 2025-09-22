#include <vector>
#include <cstdint>
#include <iostream>
#include "../inc/champsim_crc2.h"

#define NUM_CORE        1
#define LLC_SETS        (NUM_CORE * 2048)
#define LLC_WAYS        16

// RRIP parameters
#define RRPV_BITS       2
#define RRPV_MAX        ((1 << RRPV_BITS) - 1)
#define SRRIP_INIT      (RRPV_MAX - 1)  // SRRIP inserts at MAX-1
#define BIP_PROB        32              // 1/32 of BRRIP inserts at MAX-1

// PSEL (policy selection) parameters
#define PSEL_BITS       10
#define PSEL_MAX        ((1 << PSEL_BITS) - 1)
#define PSEL_INIT       (PSEL_MAX >> 1) // start in the middle

// Leader set selection: use low 5 bits of set index
#define LEADER_SRRIP(set)  (((set) & 0x1F) == 0)
#define LEADER_BRRIP(set)  (((set) & 0x1F) == 1)

// Replacement state
static uint8_t  repl_rrpv   [LLC_SETS][LLC_WAYS];
static uint32_t psel;             // global policy selector
static uint32_t random_counter;   // for BRRIP probabilistic choice

// Statistics
static uint64_t stat_hits;
static uint64_t stat_misses;

// Find a victim way by standard RRIP scan
static inline uint32_t FindVictimWay(uint32_t set) {
    while (true) {
        for (uint32_t w = 0; w < LLC_WAYS; ++w) {
            if (repl_rrpv[set][w] == RRPV_MAX) {
                return w;
            }
        }
        for (uint32_t w = 0; w < LLC_WAYS; ++w) {
            if (repl_rrpv[set][w] < RRPV_MAX) {
                repl_rrpv[set][w]++;
            }
        }
    }
}

// Initialize replacement state
void InitReplacementState() {
    stat_hits        = 0;
    stat_misses      = 0;
    psel             = PSEL_INIT;
    random_counter   = 0;
    for (uint32_t s = 0; s < LLC_SETS; ++s) {
        for (uint32_t w = 0; w < LLC_WAYS; ++w) {
            repl_rrpv[s][w] = RRPV_MAX;
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
    return FindVictimWay(set);
}

// Update replacement state on each reference
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
        // On miss/insertion
        stat_misses++;
        bool is_s_leader = LEADER_SRRIP(set);
        bool is_b_leader = LEADER_BRRIP(set);

        // Update PSEL only in leader sets
        if (is_s_leader) {
            if (psel > 0) psel--;
        } else if (is_b_leader) {
            if (psel < PSEL_MAX) psel++;
        }

        // Decide which insertion policy to use
        bool use_brrip;
        if      (is_s_leader)                     use_brrip = false;
        else if (is_b_leader)                     use_brrip = true;
        else                                      use_brrip = (psel >= PSEL_INIT);

        // Perform insertion
        if (!use_brrip) {
            // SRRIP insertion
            repl_rrpv[set][way] = SRRIP_INIT;
        } else {
            // BRRIP insertion: mostly MAX, occasionally MAX-1
            random_counter++;
            if ((random_counter % BIP_PROB) == 0) {
                repl_rrpv[set][way] = SRRIP_INIT;
            } else {
                repl_rrpv[set][way] = RRPV_MAX;
            }
        }
    }
}

// Print end-of-simulation statistics
void PrintStats() {
    uint64_t total = stat_hits + stat_misses;
    double hr = total ? (100.0 * stat_hits / total) : 0.0;
    std::cout << "==== DRRIP Policy Statistics ====\n";
    std::cout << "Total refs   : " << total       << "\n";
    std::cout << "Hits         : " << stat_hits   << "\n";
    std::cout << "Misses       : " << stat_misses << "\n";
    std::cout << "Hit Rate (%) : " << hr          << "\n";
}

// Print periodic (heartbeat) statistics
void PrintStats_Heartbeat() {
    PrintStats();
}