#include <vector>
#include <cstdint>
#include <iostream>
#include "../inc/champsim_crc2.h"

#define NUM_CORE            1
#define LLC_SETS            (NUM_CORE * 2048)
#define LLC_WAYS            16

// RRIP parameters
#define RRPV_BITS           2
#define RRPV_MAX            ((1 << RRPV_BITS) - 1)

// Signature table parameters
#define SIG_TABLE_BITS      10
#define SIG_TABLE_SIZE      (1 << SIG_TABLE_BITS)
#define SIG_MAX             3       // 2‐bit saturating counter max
#define SIG_THRESHOLD       2       // counters ≥ threshold ⇒ reuse

// Replacement state
static uint8_t  repl_rrpv[LLC_SETS][LLC_WAYS];
static uint16_t repl_sig[LLC_SETS][LLC_WAYS];  // stored PC signature per block
static uint8_t  sig_table[SIG_TABLE_SIZE];     // per‐PC reuse counters

// Statistics
static uint64_t stat_hits;
static uint64_t stat_misses;

// Helper to find an RRIP victim
static uint32_t FindVictimWay(uint32_t set) {
    while (true) {
        // Look for a way with RRPV == MAX
        for (uint32_t w = 0; w < LLC_WAYS; ++w) {
            if (repl_rrpv[set][w] == RRPV_MAX) {
                return w;
            }
        }
        // Increment RRPV of all until someone reaches MAX
        for (uint32_t w = 0; w < LLC_WAYS; ++w) {
            if (repl_rrpv[set][w] < RRPV_MAX) {
                repl_rrpv[set][w]++;
            }
        }
    }
}

// Initialize replacement state
void InitReplacementState() {
    stat_hits   = 0;
    stat_misses = 0;
    // Initialize RRPVs and stored signatures
    for (uint32_t s = 0; s < LLC_SETS; ++s) {
        for (uint32_t w = 0; w < LLC_WAYS; ++w) {
            repl_rrpv[s][w] = RRPV_MAX;
            repl_sig[s][w]  = 0;
        }
    }
    // Initialize signature table to neutral bias (threshold)
    for (uint32_t i = 0; i < SIG_TABLE_SIZE; ++i) {
        sig_table[i] = SIG_THRESHOLD;
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
    uint64_t PC,
    uint64_t /*victim_addr*/,
    uint32_t /*type*/,
    uint8_t hit
) {
    // Hash PC to a signature index
    uint16_t sig = (PC >> 2) & (SIG_TABLE_SIZE - 1);

    if (hit) {
        // On hit: promote to MRU and reinforce reuse predictor
        stat_hits++;
        repl_rrpv[set][way] = 0;
        if (sig_table[sig] < SIG_MAX) {
            sig_table[sig]++;
        }
        // Remember which signature brought this block in
        repl_sig[set][way] = sig;
    } else {
        // On miss: we have just evicted the block at [set][way]
        stat_misses++;
        // Penalize the evicted block’s PC signature
        uint16_t old_sig = repl_sig[set][way];
        if (sig_table[old_sig] > 0) {
            sig_table[old_sig]--;
        }
        // Decide insertion RRPV based on new PC’s predicted reuse
        if (sig_table[sig] >= SIG_THRESHOLD) {
            // Likely to be reused: insert with RRPV = 0 (MRU)
            repl_rrpv[set][way] = 0;
        } else {
            // Likely streaming: insert with RRPV = MAX
            repl_rrpv[set][way] = RRPV_MAX;
        }
        // Tag the new block with its PC signature
        repl_sig[set][way] = sig;
    }
}

// Print end-of-simulation statistics
void PrintStats() {
    uint64_t total = stat_hits + stat_misses;
    double hr = total ? (100.0 * stat_hits / total) : 0.0;
    std::cout << "==== SHiP Policy Statistics ====\n";
    std::cout << "Total refs   : " << total       << "\n";
    std::cout << "Hits         : " << stat_hits   << "\n";
    std::cout << "Misses       : " << stat_misses << "\n";
    std::cout << "Hit Rate (%) : " << hr          << "\n";
}

// Print periodic (heartbeat) statistics
void PrintStats_Heartbeat() {
    PrintStats();
}