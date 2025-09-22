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
#define RRPV_INIT      (RRPV_MAX - 1)          // SRRIP initial = 2

// SHiP (Signature-based Hit Predictor) parameters
#define SHCT_BITS      14
#define SHCT_SIZE      (1 << SHCT_BITS)        // 16384 entries
#define SHCT_CTR_MAX   3
#define SHCT_CTR_INIT  (SHCT_CTR_MAX / 2)      // start at 1

// Replacement state
static uint8_t  repl_rrpv[LLC_SETS][LLC_WAYS];
static uint16_t repl_sig[LLC_SETS][LLC_WAYS];
static uint8_t  SHCT[SHCT_SIZE];
static uint64_t stat_hits, stat_misses;

// Utility: compute a signature from PC and address
static inline uint32_t
ComputeSignature(uint64_t PC, uint64_t paddr)
{
    // mix PC and block address bits to form a signature index
    return (uint32_t)(((PC >> 2) ^ (paddr >> 12)) & (SHCT_SIZE - 1));
}

// Find a victim via SRRIP scan/aging
static inline uint32_t
FindVictim(uint32_t set)
{
    while (true) {
        // search for RRPV == max
        for (uint32_t w = 0; w < LLC_WAYS; ++w) {
            if (repl_rrpv[set][w] == RRPV_MAX)
                return w;
        }
        // age all blocks
        for (uint32_t w = 0; w < LLC_WAYS; ++w) {
            if (repl_rrpv[set][w] < RRPV_MAX)
                repl_rrpv[set][w]++;
        }
    }
}

// Initialize replacement state
void InitReplacementState()
{
    stat_hits = stat_misses = 0;
    // initialize RRPVs and signatures
    for (uint32_t s = 0; s < LLC_SETS; ++s) {
        for (uint32_t w = 0; w < LLC_WAYS; ++w) {
            repl_rrpv[s][w] = RRPV_INIT;
            repl_sig[s][w]  = 0;
        }
    }
    // initialize SHCT
    for (uint32_t i = 0; i < SHCT_SIZE; ++i)
        SHCT[i] = SHCT_CTR_INIT;
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
        // On cache hit: promote to MRU
        stat_hits++;
        repl_rrpv[set][way] = 0;
        // strengthen reuse prediction for this signature
        uint16_t sig = repl_sig[set][way];
        if (SHCT[sig] < SHCT_CTR_MAX)
            SHCT[sig]++;
    } else {
        // On miss (new insertion): handle eviction learning
        stat_misses++;
        // decrement counter of the evicted block's signature
        uint16_t old_sig = repl_sig[set][way];
        if (SHCT[old_sig] > 0)
            SHCT[old_sig]--;
        // compute signature for the incoming block
        uint16_t new_sig = (uint16_t)ComputeSignature(PC, paddr);
        repl_sig[set][way] = new_sig;
        // decide insertion priority
        if (SHCT[new_sig] > (SHCT_CTR_MAX >> 1)) {
            // predicted reusable → MRU
            repl_rrpv[set][way] = 0;
        } else {
            // predicted non-reuse → near-LRU
            repl_rrpv[set][way] = RRPV_INIT;
        }
    }
}

// Print end-of-simulation statistics
void PrintStats() {
    uint64_t total = stat_hits + stat_misses;
    double hr = total ? (100.0 * stat_hits / total) : 0.0;
    std::cout << "==== SHiP-RRIP Policy Statistics ====\n";
    std::cout << "Total refs   : " << total        << "\n";
    std::cout << "Hits         : " << stat_hits    << "\n";
    std::cout << "Misses       : " << stat_misses  << "\n";
    std::cout << "Hit Rate (%) : " << hr           << "\n";
}

// Print periodic (heartbeat) statistics
void PrintStats_Heartbeat() {
    PrintStats();
}