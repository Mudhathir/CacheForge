#include <vector>
#include <cstdint>
#include <iostream>
#include <cstdlib>
#include "../inc/champsim_crc2.h"

#define NUM_CORE    1
#define LLC_SETS    (NUM_CORE * 2048)
#define LLC_WAYS    16

// --- RRIP parameters ---
#define RRPV_BITS   2
#define RRPV_MAX    ((1 << RRPV_BITS) - 1)   // 3
// SRRIP insertion RRPV
#define SRRIP_RRPV  (RRPV_MAX - 1)           // 2
// BRRIP insertion: far (RRPV_MAX) with 31/32, MRU (0) with 1/32
#define BRRIP_PROB  32

// --- DIP parameters ---
#define PSEL_BITS   10
#define PSEL_MAX    ((1 << PSEL_BITS) - 1)    // 1023
#define PSEL_TH     (PSEL_MAX >> 1)           // 511
static uint16_t PSEL;                         // policy selection counter

// Replacement state
static uint8_t  repl_rrpv   [LLC_SETS][LLC_WAYS];
static uint64_t stat_hits, stat_misses;

// Initialize replacement state
void InitReplacementState() {
    stat_hits   = 0;
    stat_misses = 0;
    // Initialize all lines to max RRPV (cold)
    for (int s = 0; s < LLC_SETS; s++) {
        for (int w = 0; w < LLC_WAYS; w++) {
            repl_rrpv[s][w] = RRPV_MAX;
        }
    }
    // Initialize policy selector to midpoint
    PSEL = PSEL_TH;
    // Seed randomness for BRRIP sampling
    std::srand(0);
}

// Standard SRRIP victim search
static uint32_t FindVictimWay(uint32_t set) {
    while (true) {
        // look for RRPV == max
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            if (repl_rrpv[set][w] == RRPV_MAX) {
                return w;
            }
        }
        // age all if none found
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            if (repl_rrpv[set][w] < RRPV_MAX)
                repl_rrpv[set][w]++;
        }
    }
}

// Decide if this set is an SRRIP sample, BRRIP sample, or follower
static inline bool IsSRRIPSample(uint32_t set) {
    // sample 32 sets per 64-set region
    return ((set & 0x3F) < 32);
}
static inline bool IsBRRIPSample(uint32_t set) {
    return (((set & 0x3F) >= 32) && ((set & 0x3F) < 64));
}

// Find victim in the set
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

// Update replacement state on each access
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
        // Update PSEL on sample sets
        uint32_t idx = set & 0x3F;
        if (idx < 32) {
            // SRRIP sample: a hit here means SRRIP is good
            if (PSEL < PSEL_MAX) PSEL++;
        } else if (idx < 64) {
            // BRRIP sample: a hit here means BRRIP is good
            if (PSEL > 0) PSEL--;
        }
        return;
    }

    // On miss
    stat_misses++;

    // Determine which insertion policy to use
    uint8_t new_rrpv;
    uint32_t idx = set & 0x3F;
    bool use_srrip;
    if (IsSRRIPSample(set)) {
        use_srrip = true;
    } else if (IsBRRIPSample(set)) {
        use_srrip = false;
    } else {
        // follower sets follow the winner
        use_srrip = (PSEL <= PSEL_TH);
    }

    if (use_srrip) {
        // SRRIP: medium insertion
        new_rrpv = SRRIP_RRPV;
    } else {
        // BRRIP: mostly far insertion, rare MRU
        if ((std::rand() % BRRIP_PROB) == 0) {
            new_rrpv = 0;           // MRU
        } else {
            new_rrpv = RRPV_MAX;    // far
        }
    }
    repl_rrpv[set][way] = new_rrpv;
}

// Print end-of-simulation statistics
void PrintStats() {
    uint64_t total = stat_hits + stat_misses;
    double   hr    = total ? (100.0 * stat_hits / total) : 0.0;
    std::cout << "==== DRRIP Policy Statistics ====\n";
    std::cout << "Total refs      : " << total       << "\n";
    std::cout << "Hits            : " << stat_hits   << "\n";
    std::cout << "Misses          : " << stat_misses << "\n";
    std::cout << "Hit Rate (%)    : " << hr          << "\n";
    std::cout << "PSEL            : " << PSEL        << "\n";
}

// Print periodic (heartbeat) statistics
void PrintStats_Heartbeat() {
    PrintStats();
}