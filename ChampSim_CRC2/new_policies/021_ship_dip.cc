#include <vector>
#include <cstdint>
#include <iostream>
#include <cstdlib>
#include "../inc/champsim_crc2.h"

#define NUM_CORE       1
#define LLC_SETS       (NUM_CORE * 2048)
#define LLC_WAYS       16

// RRIP parameters
#define RRPV_BITS      3
#define RRPV_MAX       ((1 << RRPV_BITS) - 1)  // 7
#define SRRIP_INIT     (RRPV_MAX - 1)          // 6

// PC‐based Counter Table (PCT) parameters (SHiP)
#define PCT_BITS       2
#define PCT_CTR_MAX    ((1 << PCT_BITS) - 1)   // 3
#define PCT_SIZE       4096
#define PCT_INIT       (PCT_CTR_MAX >> 1)      // 1

// DRRIP / DIP parameters
#define PSEL_BITS      10
#define PSEL_MAX       ((1 << PSEL_BITS) - 1)  // 1023
#define PSEL_INIT      (PSEL_MAX >> 1)         // 511
#define BRRIP_PROB     32                       // 1/32 probability for BRRIP

// Replacement metadata
static uint8_t   repl_rrpv   [LLC_SETS][LLC_WAYS];
static bool      repl_has_hit[LLC_SETS][LLC_WAYS];
static uint16_t  repl_sig    [LLC_SETS][LLC_WAYS];

// Global tables and counters
static uint8_t   PCT[ PCT_SIZE ];
static uint16_t  PSEL;
static uint64_t  last_miss_block;

// Statistics
static uint64_t stat_hits, stat_misses, stat_evictions;

// Initialize replacement state
void InitReplacementState() {
    for (uint32_t s = 0; s < LLC_SETS; s++) {
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            repl_rrpv[s][w]      = RRPV_MAX;
            repl_has_hit[s][w]   = false;
            repl_sig[s][w]       = 0;
        }
    }
    for (uint32_t i = 0; i < PCT_SIZE; i++) {
        PCT[i] = PCT_INIT;
    }
    PSEL             = PSEL_INIT;
    last_miss_block  = 0;
    stat_hits        = stat_misses = stat_evictions = 0;
}

// Victim selection: SRRIP scan‐and‐age
uint32_t GetVictimInSet(
    uint32_t /*cpu*/,
    uint32_t set,
    const BLOCK */* current_set */,
    uint64_t /* PC */,
    uint64_t /* paddr */,
    uint32_t /* type */
) {
    while (true) {
        // Look for a line with RRPV == MAX
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            if (repl_rrpv[set][w] == RRPV_MAX) {
                return w;
            }
        }
        // No suitable victim: age all lines
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            if (repl_rrpv[set][w] < RRPV_MAX) {
                repl_rrpv[set][w]++;
            }
        }
    }
    return 0; // unreachable
}

// Update replacement state on access
void UpdateReplacementState(
    uint32_t /*cpu*/,
    uint32_t set,
    uint32_t way,
    uint64_t paddr,
    uint64_t PC,
    uint64_t /* victim_addr */,
    uint32_t /* type */,
    uint8_t hit
) {
    if (hit) {
        // ---- HIT PATH ----
        stat_hits++;
        // Promote strongly
        repl_rrpv[set][way]    = 0;
        repl_has_hit[set][way] = true;
        // Update PC‐based counter
        uint16_t sig = repl_sig[set][way];
        if (PCT[sig] < PCT_CTR_MAX) {
            PCT[sig]++;
        }
    } else {
        // ---- MISS & EVICTION PATH ----
        stat_misses++;
        stat_evictions++;
        // On eviction decrement cold lines' counters
        if (!repl_has_hit[set][way]) {
            uint16_t old_sig = repl_sig[set][way];
            if (PCT[old_sig] > 0) {
                PCT[old_sig]--;
            }
        }
        // Compute new PC signature
        uint16_t new_sig = (PC >> 2) & (PCT_SIZE - 1);
        repl_sig[set][way]     = new_sig;
        repl_has_hit[set][way] = false;

        // Simple sequential‐stream detection
        uint64_t blk      = (paddr >> 6);
        bool sequential  = (blk == last_miss_block + 1);
        last_miss_block  = blk;

        // Insertion decision
        if (sequential) {
            // Moderate priority for simple streams
            repl_rrpv[set][way] = SRRIP_INIT - 1;
        } else if (PCT[new_sig] == PCT_CTR_MAX) {
            // Hot PC → strong promotion
            repl_rrpv[set][way] = 0;
        } else if (PCT[new_sig] == 0) {
            // Cold PC → bypass
            repl_rrpv[set][way] = RRPV_MAX;
        } else {
            // Ambiguous: use DIP (SRRIP vs. BRRIP) trained by sample sets
            uint32_t s_mod = set & 0x1F;
            if (s_mod == 0) {
                // SRRIP sample set: always SRRIP_INIT
                repl_rrpv[set][way] = SRRIP_INIT;
                // Train PSEL: miss in SRRIP sample biases towards BRRIP
                if (PSEL > 0) PSEL--;
            } else if (s_mod == 1) {
                // BRRIP sample set: mostly bypass, rare MRU
                if ((std::rand() & (BRRIP_PROB - 1)) == 0) {
                    repl_rrpv[set][way] = 0;
                } else {
                    repl_rrpv[set][way] = RRPV_MAX;
                }
                // Train PSEL: miss in BRRIP sample biases towards SRRIP
                if (PSEL < PSEL_MAX) PSEL++;
            } else {
                // Follower sets: consult PSEL
                if (PSEL >= (PSEL_MAX >> 1)) {
                    // Use BRRIP
                    if ((std::rand() & (BRRIP_PROB - 1)) == 0) {
                        repl_rrpv[set][way] = 0;
                    } else {
                        repl_rrpv[set][way] = RRPV_MAX;
                    }
                } else {
                    // Use SRRIP
                    repl_rrpv[set][way] = SRRIP_INIT;
                }
            }
        }
    }
}

// Print end‐of‐simulation statistics
void PrintStats() {
    uint64_t total = stat_hits + stat_misses;
    double hr = total ? (double(stat_hits) / double(total) * 100.0) : 0.0;
    std::cout << "==== SHiP-DIP Statistics ====\n";
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