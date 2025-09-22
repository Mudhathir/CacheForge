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

// DRRIP set‐dueling parameters
// One in 64 sets is SRRIP sample, one in 64 is BIP sample
#define SAMPLE_DIV          64
#define SRRIP_SAMPLE_VAL    0
#define BIP_SAMPLE_VAL      1

// PSEL: saturating counter to choose policy in follower sets
#define PSEL_BITS           10
#define PSEL_MAX            ((1 << PSEL_BITS) - 1)
#define PSEL_INIT           (PSEL_MAX / 2)

// BIP insertion: 1 in 32 inserts like SRRIP, else at distant RRPV_MAX
#define BIP_PROB_SHIFT      5  // 2^5 = 32

// Replacement state arrays
static uint8_t  repl_rrpv[LLC_SETS][LLC_WAYS];
static uint32_t PSEL;
static uint32_t random_seed;

// Statistics
static uint64_t stat_hits;
static uint64_t stat_misses;

// Initialize replacement state
void InitReplacementState() {
    stat_hits   = 0;
    stat_misses = 0;
    PSEL        = PSEL_INIT;
    random_seed = 0xdeadbeef;
    for (uint32_t s = 0; s < LLC_SETS; s++) {
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            repl_rrpv[s][w] = RRPV_MAX;
        }
    }
}

// SRRIP victim selection: find RRPV == MAX or increment all until one appears
static uint32_t FindVictimWay(uint32_t set) {
    while (true) {
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            if (repl_rrpv[set][w] == RRPV_MAX) {
                return w;
            }
        }
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            if (repl_rrpv[set][w] < RRPV_MAX) {
                repl_rrpv[set][w]++;
            }
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
        // On hit: promote to MRU (RRPV=0)
        stat_hits++;
        repl_rrpv[set][way] = 0;
    } else {
        // Miss: choose insertion policy
        stat_misses++;
        uint32_t idx = set & (SAMPLE_DIV - 1);
        bool is_srrip_sample = (idx == SRRIP_SAMPLE_VAL);
        bool is_bip_sample   = (idx == BIP_SAMPLE_VAL);
        bool use_bip;

        // Sample‐set updates to PSEL
        if (is_srrip_sample) {
            use_bip = false;
            if (PSEL > 0) PSEL--;
        } else if (is_bip_sample) {
            use_bip = true;
            if (PSEL < PSEL_MAX) PSEL++;
        } else {
            // Follower sets follow the policy indicated by PSEL
            use_bip = (PSEL >= PSEL_INIT);
        }

        // Bimodal insertion randomness via simple LFSR
        random_seed = random_seed * 1103515245 + 12345;
        bool rare_srrip = (((random_seed >> 16) & ((1 << BIP_PROB_SHIFT) - 1)) == 0);

        if (use_bip) {
            // BIP: mostly distant re-reference, occasionally SRRIP‐style
            repl_rrpv[set][way] = rare_srrip ? (RRPV_MAX - 1) : RRPV_MAX;
        } else {
            // SRRIP insertion: near re-reference
            repl_rrpv[set][way] = RRPV_MAX - 1;
        }
    }
}

// Print end‐of‐simulation statistics
void PrintStats() {
    uint64_t total = stat_hits + stat_misses;
    double hr = total ? (100.0 * stat_hits / total) : 0.0;
    std::cout << "==== DRRIP Policy Statistics ====\n";
    std::cout << "Total refs   : " << total        << "\n";
    std::cout << "Hits         : " << stat_hits    << "\n";
    std::cout << "Misses       : " << stat_misses  << "\n";
    std::cout << "Hit Rate (%) : " << hr           << "\n";
}

// Print periodic (heartbeat) statistics
void PrintStats_Heartbeat() {
    PrintStats();
}