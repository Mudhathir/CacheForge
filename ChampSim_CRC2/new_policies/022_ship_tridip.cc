#include <vector>
#include <cstdint>
#include <iostream>
#include <cstdlib>
#include "../inc/champsim_crc2.h"

#define NUM_CORE        1
#define LLC_SETS        (NUM_CORE * 2048)
#define LLC_WAYS        16

// RRIP parameters
#define RRPV_BITS       3
#define RRPV_MAX        ((1 << RRPV_BITS) - 1)  // 7
#define SRRIP_INIT      (RRPV_MAX - 1)          // 6

// PC-based Counter Table (SHiP) parameters
#define PCT_BITS        2
#define PCT_CTR_MAX     ((1 << PCT_BITS) - 1)   // 3
#define PCT_SIZE        4096
#define PCT_INIT        (PCT_CTR_MAX >> 1)      // 1

// Triple-DIP sample parameters
#define SAMPLE_MODULO   128
#define SAMPLE_SIZE     8
#define RGN0_START      0                       // SRRIP sample for BRRIP training
#define RGN1_START      (RGN0_START + SAMPLE_SIZE)  // BRRIP sample
#define RGN2_START      (RGN1_START + SAMPLE_SIZE)  // SRRIP sample for LRU training
#define RGN3_START      (RGN2_START + SAMPLE_SIZE)  // LRU sample

// PSEL0: SRRIP vs BRRIP, PSEL1: SRRIP vs LRU
#define PSEL_BITS       8
#define PSEL_MAX        ((1 << PSEL_BITS) - 1)  // 255
#define PSEL_INIT       (PSEL_MAX >> 1)         // 127

// BRRIP probability (1/BRRIP_PROB MRU insertion)
#define BRRIP_PROB      32

// Replacement metadata
static uint8_t   repl_rrpv   [LLC_SETS][LLC_WAYS];
static bool      repl_has_hit[LLC_SETS][LLC_WAYS];
static uint16_t  repl_sig    [LLC_SETS][LLC_WAYS];

// Global tables and counters
static uint8_t   PCT[PCT_SIZE];
static uint8_t   PSEL0, PSEL1;
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
    PSEL0            = PSEL_INIT;
    PSEL1            = PSEL_INIT;
    last_miss_block  = 0;
    stat_hits        = stat_misses = stat_evictions = 0;
    std::srand(0);
}

// Victim selection: standard SRRIP scan-and-age
uint32_t GetVictimInSet(
    uint32_t /*cpu*/,
    uint32_t set,
    const BLOCK */* current_set */,
    uint64_t /* PC */,
    uint64_t /* paddr */,
    uint32_t /* type */
) {
    while (true) {
        // Find a line with RRPV == MAX
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            if (repl_rrpv[set][w] == RRPV_MAX) {
                return w;
            }
        }
        // Age all lines
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
        repl_rrpv[set][way]    = 0;
        repl_has_hit[set][way] = true;
        uint16_t sig = repl_sig[set][way];
        if (PCT[sig] < PCT_CTR_MAX) {
            PCT[sig]++;
        }
    } else {
        // ---- MISS & EVICTION PATH ----
        stat_misses++;
        stat_evictions++;
        // Update PCT if evicting a never-hit block
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
        // Sequential-stream detection
        uint64_t blk     = (paddr >> 6);
        bool sequential = (blk == last_miss_block + 1);
        last_miss_block  = blk;
        // Insertion logic
        if (sequential) {
            // Moderate priority for simple streams
            repl_rrpv[set][way] = SRRIP_INIT - 1;
        }
        else if (PCT[new_sig] == PCT_CTR_MAX) {
            // Hot PC → strong promotion (MRU)
            repl_rrpv[set][way] = 0;
        }
        else if (PCT[new_sig] == 0) {
            // Cold PC → bypass
            repl_rrpv[set][way] = RRPV_MAX;
        }
        else {
            // Ambiguous: tri-modal DIP
            uint32_t s_mod = set & (SAMPLE_MODULO - 1);
            // SRRIP sample for BRRIP training
            if (s_mod >= RGN0_START && s_mod < RGN0_START + SAMPLE_SIZE) {
                repl_rrpv[set][way] = SRRIP_INIT;
                if (PSEL0 > 0) PSEL0--;
            }
            // BRRIP sample
            else if (s_mod >= RGN1_START && s_mod < RGN1_START + SAMPLE_SIZE) {
                if ((std::rand() % BRRIP_PROB) == 0)
                    repl_rrpv[set][way] = 0;
                else
                    repl_rrpv[set][way] = RRPV_MAX;
                if (PSEL0 < PSEL_MAX) PSEL0++;
            }
            // SRRIP sample for LRU training
            else if (s_mod >= RGN2_START && s_mod < RGN2_START + SAMPLE_SIZE) {
                repl_rrpv[set][way] = SRRIP_INIT;
                if (PSEL1 > 0) PSEL1--;
            }
            // LRU sample
            else if (s_mod >= RGN3_START && s_mod < RGN3_START + SAMPLE_SIZE) {
                repl_rrpv[set][way] = 0;
                if (PSEL1 < PSEL_MAX) PSEL1++;
            }
            // Follower sets
            else {
                if (PSEL1 > (PSEL_MAX >> 1)) {
                    // LRU wins
                    repl_rrpv[set][way] = 0;
                }
                else if (PSEL0 > (PSEL_MAX >> 1)) {
                    // BRRIP wins
                    if ((std::rand() % BRRIP_PROB) == 0)
                        repl_rrpv[set][way] = 0;
                    else
                        repl_rrpv[set][way] = RRPV_MAX;
                }
                else {
                    // SRRIP wins
                    repl_rrpv[set][way] = SRRIP_INIT;
                }
            }
        }
    }
}

// Print end-of-simulation statistics
void PrintStats() {
    uint64_t total = stat_hits + stat_misses;
    double hr = total ? (double(stat_hits) / double(total) * 100.0) : 0.0;
    std::cout << "==== SHiP-TriDIP Statistics ====\n";
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