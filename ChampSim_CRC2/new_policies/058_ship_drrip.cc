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
#define RRPV_INIT      (RRPV_MAX - 1)          // 2

// SHiP parameters
#define SHCT_BITS      14
#define SHCT_SIZE      (1 << SHCT_BITS)        // 16384 entries
#define SHCT_CTR_MAX   3
#define SHCT_CTR_INIT  (SHCT_CTR_MAX / 2)      // 1

// DRRIP (set dueling) parameters
#define PSEL_BITS      10
#define PSEL_MAX       ((1 << PSEL_BITS) - 1)  // 1023
#define PSEL_INIT      (PSEL_MAX / 2)          // 511
#define SAMPLE_PERIOD  64                      // one SRRIP sample set when set%64==0, BRRIP when set%64==1
#define BRRIP_PROB     32                      // 1/32 chance to insert MRU in BRRIP

// Replacement state
static uint8_t  repl_rrpv[LLC_SETS][LLC_WAYS];
static uint16_t repl_sig[LLC_SETS][LLC_WAYS];
static uint8_t  SHCT[SHCT_SIZE];
static uint16_t PSEL;
static uint64_t stat_hits, stat_misses;

// Utility: compute a signature from PC and block address
static inline uint32_t
ComputeSignature(uint64_t PC, uint64_t paddr)
{
    return (uint32_t)(((PC >> 2) ^ (paddr >> 12)) & (SHCT_SIZE - 1));
}

// Victim selection by SRRIP scan/aging
static inline uint32_t
FindVictim(uint32_t set)
{
    while (true) {
        for (uint32_t w = 0; w < LLC_WAYS; ++w) {
            if (repl_rrpv[set][w] == RRPV_MAX)
                return w;
        }
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
    PSEL = PSEL_INIT;
    for (uint32_t s = 0; s < LLC_SETS; ++s) {
        for (uint32_t w = 0; w < LLC_WAYS; ++w) {
            repl_rrpv[s][w] = RRPV_INIT;
            repl_sig[s][w]  = 0;
        }
    }
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
    bool is_srrip_sample  = ((set % SAMPLE_PERIOD) == 0);
    bool is_brrip_sample  = ((set % SAMPLE_PERIOD) == 1);

    if (hit) {
        // Hit: promote to MRU and strengthen signature
        stat_hits++;
        repl_rrpv[set][way] = 0;
        uint16_t sig = repl_sig[set][way];
        if (SHCT[sig] < SHCT_CTR_MAX)
            SHCT[sig]++;
    } else {
        // Miss: learn from samples, update SHCT on eviction
        stat_misses++;
        if (is_srrip_sample) {
            if (PSEL < PSEL_MAX) PSEL++;
        } else if (is_brrip_sample) {
            if (PSEL > 0) PSEL--;
        }
        uint16_t old_sig = repl_sig[set][way];
        if (SHCT[old_sig] > 0)
            SHCT[old_sig]--;

        // Compute new signature and decide insertion
        uint16_t new_sig = (uint16_t)ComputeSignature(PC, paddr);
        repl_sig[set][way] = new_sig;

        if (SHCT[new_sig] > (SHCT_CTR_MAX >> 1)) {
            // Predicted reusable → MRU
            repl_rrpv[set][way] = 0;
        } else {
            // Predicted non-reuse → dynamic insertion
            if (is_srrip_sample) {
                // SRRIP policy
                repl_rrpv[set][way] = RRPV_INIT;
            } else if (is_brrip_sample) {
                // BRRIP policy
                uint32_t r = new_sig & (BRRIP_PROB - 1);
                repl_rrpv[set][way] = (r == 0) ? 0 : RRPV_MAX;
            } else {
                // Follower sets choose based on PSEL
                if (PSEL >= PSEL_INIT) {
                    // favor BRRIP
                    uint32_t r = new_sig & (BRRIP_PROB - 1);
                    repl_rrpv[set][way] = (r == 0) ? 0 : RRPV_MAX;
                } else {
                    // favor SRRIP
                    repl_rrpv[set][way] = RRPV_INIT;
                }
            }
        }
    }
}

// Print end-of-simulation statistics
void PrintStats() {
    uint64_t total = stat_hits + stat_misses;
    double hr = total ? (100.0 * stat_hits / total) : 0.0;
    std::cout << "==== SHiP-DRRIP Policy Statistics ====\n";
    std::cout << "Total refs   : " << total       << "\n";
    std::cout << "Hits         : " << stat_hits   << "\n";
    std::cout << "Misses       : " << stat_misses << "\n";
    std::cout << "Hit Rate (%) : " << hr          << "\n";
}

// Print periodic (heartbeat) statistics
void PrintStats_Heartbeat() {
    PrintStats();
}