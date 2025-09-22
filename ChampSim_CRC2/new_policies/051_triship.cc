#include <vector>
#include <cstdint>
#include <cstring>
#include <iostream>
#include "../inc/champsim_crc2.h"

#define NUM_CORE       1
#define LLC_SETS       (NUM_CORE * 2048)
#define LLC_WAYS       16

// RRIP parameters
#define RRPV_BITS      2
#define RRPV_MAX       ((1 << RRPV_BITS) - 1)

// SHiP parameters
#define SHCT_CTR_BITS  3
#define SHCT_CTR_MAX   ((1 << SHCT_CTR_BITS) - 1)
#define SHCT_THRESHOLD (SHCT_CTR_MAX >> 1)   // high‐reuse threshold
#define SHCT_SIZE      8192                  // must be power‐of‐two

// Replacement state arrays
static uint8_t  repl_rrpv[LLC_SETS][LLC_WAYS];
static bool     reuse_flag[LLC_SETS][LLC_WAYS];
static uint32_t sig_table[LLC_SETS][LLC_WAYS];
static uint8_t  SHCT[SHCT_SIZE];

// Statistics
static uint64_t stat_hits;
static uint64_t stat_misses;

// Initialize replacement state
void InitReplacementState() {
    stat_hits   = 0;
    stat_misses = 0;
    // Initialize per‐way RRPV, reuse flags, and signatures
    for (uint32_t s = 0; s < LLC_SETS; s++) {
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            repl_rrpv[s][w]   = RRPV_MAX;
            reuse_flag[s][w]  = false;
            sig_table[s][w]   = 0;
        }
    }
    // Initialize SHCT to neutral (midpoint)
    for (uint32_t i = 0; i < SHCT_SIZE; i++) {
        SHCT[i] = SHCT_THRESHOLD;
    }
}

// Find a victim way using SRRIP-style search
static uint32_t FindVictimWay(uint32_t set) {
    while (true) {
        // 1) look for an RRPV == MAX
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            if (repl_rrpv[set][w] == RRPV_MAX) {
                return w;
            }
        }
        // 2) increment all RRPVs < MAX
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            if (repl_rrpv[set][w] < RRPV_MAX) {
                repl_rrpv[set][w]++;
            }
        }
    }
}

// Choose a victim in a set (always replace something)
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
    uint64_t PC,
    uint64_t /*victim_addr*/,
    uint32_t /*type*/,
    uint8_t hit
) {
    if (hit) {
        // On hit: strong promotion and mark reuse
        stat_hits++;
        repl_rrpv[set][way]  = 0;      // MRU
        reuse_flag[set][way] = true;
    } else {
        // On miss: install new block at [set][way], but first update SHCT
        stat_misses++;
        uint32_t old_sig = sig_table[set][way];
        // Update predictor based on whether the evicted block was ever reused
        if (reuse_flag[set][way]) {
            if (SHCT[old_sig] < SHCT_CTR_MAX) SHCT[old_sig]++;
        } else {
            if (SHCT[old_sig] > 0)           SHCT[old_sig]--;
        }
        // Compute new signature from PC
        uint32_t new_sig = (uint32_t)PC & (SHCT_SIZE - 1);
        sig_table[set][way]   = new_sig;
        reuse_flag[set][way]  = false;
        // Tri‐modal insertion based on SHCT counter value
        uint8_t ctr = SHCT[new_sig];
        if (ctr > SHCT_THRESHOLD) {
            // Frequently reused → insert at MRU
            repl_rrpv[set][way] = 0;
        } else if (ctr > 0) {
            // Occasional reuse → insert at mid-range (near reuse window)
            repl_rrpv[set][way] = RRPV_MAX - 1;
        } else {
            // Predicted cold/streaming → insert at LRU (bypass‐like)
            repl_rrpv[set][way] = RRPV_MAX;
        }
    }
}

// Print end‐of‐simulation statistics
void PrintStats() {
    uint64_t total = stat_hits + stat_misses;
    double hr = total ? (100.0 * stat_hits / total) : 0.0;
    std::cout << "==== TriSHiP Policy Statistics ====\n";
    std::cout << "Total refs   : " << total       << "\n";
    std::cout << "Hits         : " << stat_hits   << "\n";
    std::cout << "Misses       : " << stat_misses << "\n";
    std::cout << "Hit Rate (%) : " << hr          << "\n";
}

// Print periodic (heartbeat) statistics
void PrintStats_Heartbeat() {
    PrintStats();
}