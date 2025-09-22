#include <vector>
#include <cstdint>
#include <iostream>
#include "../inc/champsim_crc2.h"

#define NUM_CORE    1
#define LLC_SETS    (NUM_CORE * 2048)
#define LLC_WAYS    16

// --- RRIP parameters ---
#define RRPV_BITS   2
#define RRPV_MAX    ((1 << RRPV_BITS) - 1)   // 3

// --- SHiP parameters ---
#define SHCT_BITS   3
#define SHCT_MAX    ((1 << SHCT_BITS) - 1)   // 7
#define SHCT_TH     (SHCT_MAX >> 1)          // 3
#define SHCT_INIT   SHCT_TH                  // start at threshold
#define PRT_SIZE    32768                    // must be power of two
#define PRT_MASK    (PRT_SIZE - 1)

// Replacement state
static uint8_t  repl_rrpv[LLC_SETS][LLC_WAYS];
static uint8_t  reused  [LLC_SETS][LLC_WAYS];   // did this block see a hit?
static uint16_t sigtbl  [LLC_SETS][LLC_WAYS];   // PC signature per line
static uint8_t  SHCT    [PRT_SIZE];             // signature counters
static uint64_t stat_hits, stat_misses;

// Initialize replacement state
void InitReplacementState() {
    stat_hits   = 0;
    stat_misses = 0;
    // Initialize RRIP and metadata
    for (int s = 0; s < LLC_SETS; s++) {
        for (int w = 0; w < LLC_WAYS; w++) {
            repl_rrpv[s][w] = RRPV_MAX;  // cold
            reused  [s][w] = 0;
            sigtbl  [s][w] = 0;
        }
    }
    // Initialize signature counters to midpoint
    for (int i = 0; i < PRT_SIZE; i++) {
        SHCT[i] = SHCT_INIT;
    }
}

// Standard RRIP victim search
static uint32_t FindVictimWay(uint32_t set) {
    while (true) {
        // 1) look for RRPV == max
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            if (repl_rrpv[set][w] == RRPV_MAX)
                return w;
        }
        // 2) age all if none found
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            if (repl_rrpv[set][w] < RRPV_MAX)
                repl_rrpv[set][w]++;
        }
    }
}

// Find victim and decay signature counter on non-reused blocks
uint32_t GetVictimInSet(
    uint32_t /*cpu*/,
    uint32_t set,
    const BLOCK */*current_set*/,
    uint64_t /*PC*/,
    uint64_t /*paddr*/,
    uint32_t /*type*/
) {
    uint32_t victim = FindVictimWay(set);
    // If the victim never saw a hit, decay its signature counter
    uint16_t sig = sigtbl[set][victim];
    if (!reused[set][victim] && SHCT[sig] > 0) {
        SHCT[sig]--;
    }
    // Clear metadata (will be overwritten on insertion)
    reused[set][victim] = 0;
    sigtbl[set][victim] = 0;
    return victim;
}

// Update replacement state on access or fill
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
        // On hit: perfect reuse, promote to MRU and credit signature
        stat_hits++;
        repl_rrpv[set][way] = 0;
        reused[set][way]   = 1;
        uint16_t sig = sigtbl[set][way];
        if (SHCT[sig] < SHCT_MAX) {
            SHCT[sig]++;
        }
    } else {
        // On miss: allocate new block
        stat_misses++;
        // Derive a signature from the PC (simple low-bit hash)
        uint16_t sig = static_cast<uint16_t>(PC) & PRT_MASK;
        sigtbl[set][way] = sig;
        reused[set][way] = 0;
        // Insert near or far based on learned reuse
        if (SHCT[sig] >= SHCT_TH) {
            repl_rrpv[set][way] = 0;          // MRU insertion
        } else {
            repl_rrpv[set][way] = RRPV_MAX;   // distant insertion
        }
    }
}

// Print end-of-simulation statistics
void PrintStats() {
    uint64_t total = stat_hits + stat_misses;
    double   hr    = total ? (100.0 * stat_hits / total) : 0.0;
    std::cout << "==== SHiP-RRIP Policy Statistics ====\n";
    std::cout << "Total refs      : " << total       << "\n";
    std::cout << "Hits            : " << stat_hits   << "\n";
    std::cout << "Misses          : " << stat_misses << "\n";
    std::cout << "Hit Rate (%)    : " << hr          << "\n";
}

// Print periodic (heartbeat) statistics
void PrintStats_Heartbeat() {
    PrintStats();
}