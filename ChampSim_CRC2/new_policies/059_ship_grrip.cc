#include <vector>
#include <cstdint>
#include <iostream>
#include "../inc/champsim_crc2.h"

#define NUM_CORE      1
#define LLC_SETS      (NUM_CORE * 2048)
#define LLC_WAYS      16

// GRRIP parameters
#define RRPV_BITS     3
#define RRPV_MAX      ((1 << RRPV_BITS) - 1)    // 7
#define RRPV_INIT     RRPV_MAX                  // new blocks default to far-LRU

// SHiP parameters
#define SHCT_BITS     14
#define SHCT_SIZE     (1 << SHCT_BITS)          // 16384 entries
#define SHCT_CTR_MAX  3
#define SHCT_CTR_INIT (SHCT_CTR_MAX / 2)        // 1

// Replacement state
static uint8_t  repl_rrpv[LLC_SETS][LLC_WAYS];
static uint16_t repl_sig[LLC_SETS][LLC_WAYS];
static uint8_t  SHCT[SHCT_SIZE];
static uint64_t stat_hits, stat_misses;

// Utility: PC/paddr → SHCT index
static inline uint32_t
ComputeSignature(uint64_t PC, uint64_t paddr)
{
    return (uint32_t)(((PC >> 2) ^ (paddr >> 12)) & (SHCT_SIZE - 1));
}

// Victim selection: find a way with RRPV == RRPV_MAX, aging otherwise
static inline uint32_t
FindVictim(uint32_t set)
{
    while (true) {
        // look for an LRU candidate
        for (uint32_t w = 0; w < LLC_WAYS; ++w) {
            if (repl_rrpv[set][w] == RRPV_MAX)
                return w;
        }
        // age everyone
        for (uint32_t w = 0; w < LLC_WAYS; ++w) {
            if (repl_rrpv[set][w] < RRPV_MAX)
                repl_rrpv[set][w]++;
        }
    }
}

// Initialize all state
void InitReplacementState()
{
    stat_hits = stat_misses = 0;
    for (uint32_t s = 0; s < LLC_SETS; ++s) {
        for (uint32_t w = 0; w < LLC_WAYS; ++w) {
            repl_rrpv[s][w] = RRPV_INIT;
            repl_sig[s][w]  = 0;
        }
    }
    for (uint32_t i = 0; i < SHCT_SIZE; ++i)
        SHCT[i] = SHCT_CTR_INIT;
}

// Always use GRRIP victim selection
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

// On each access: update SHCT and RRPVs
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
        // Hit: promote to MRU and reinforce reuse prediction
        stat_hits++;
        repl_rrpv[set][way] = 0;
        uint16_t sig = repl_sig[set][way];
        if (SHCT[sig] < SHCT_CTR_MAX)
            SHCT[sig]++;
    } else {
        // Miss: evict way, update old SHCT, compute new signature
        stat_misses++;
        uint16_t old_sig = repl_sig[set][way];
        if (SHCT[old_sig] > 0)
            SHCT[old_sig]--;
        uint16_t new_sig = (uint16_t)ComputeSignature(PC, paddr);
        repl_sig[set][way] = new_sig;

        // Granular RRPV insertion: RRPVinit = RRPV_MAX - SHCT[new_sig]*(RRPV_MAX/SHCT_CTR_MAX)
        int gran = RRPV_MAX / SHCT_CTR_MAX;       // 7/3 = 2
        int rinit = RRPV_MAX - (SHCT[new_sig] * gran);
        if (rinit < 0) rinit = 0;
        if (rinit > RRPV_MAX) rinit = RRPV_MAX;
        repl_rrpv[set][way] = (uint8_t)rinit;
    }
}

// Print final statistics
void PrintStats() {
    uint64_t total = stat_hits + stat_misses;
    double hr = total ? (100.0 * stat_hits / total) : 0.0;
    std::cout << "==== SHiP-GRRIP Policy Statistics ====\n";
    std::cout << "Total refs   : " << total       << "\n";
    std::cout << "Hits         : " << stat_hits   << "\n";
    std::cout << "Misses       : " << stat_misses << "\n";
    std::cout << "Hit Rate (%) : " << hr          << "\n";
}

// Heartbeat prints the same
void PrintStats_Heartbeat() {
    PrintStats();
}