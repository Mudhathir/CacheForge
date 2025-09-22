#include <vector>
#include <cstdint>
#include <iostream>
#include "../inc/champsim_crc2.h"

#define NUM_CORE       1
#define LLC_SETS       (NUM_CORE * 2048)
#define LLC_WAYS       16

// RRIP parameters
#define RRPV_BITS      2
#define RRPV_MAX       ((1 << RRPV_BITS) - 1)

// SHCT (Signature History Counter Table) parameters
#define SHCT_BITS      10
#define SHCT_SIZE      (1 << SHCT_BITS)
#define SHCT_MAX       7    // 3-bit saturating
#define SHCT_INIT      (SHCT_MAX / 2)
#define SHCT_THRESH    SHCT_INIT

// Replacement metadata
static uint8_t   repl_rrpv[LLC_SETS][LLC_WAYS];
static uint16_t  repl_sig [LLC_SETS][LLC_WAYS]; // signature per block
static uint8_t   shct     [SHCT_SIZE];          // per-signature counter

// Statistics
static uint64_t stat_hits;
static uint64_t stat_misses;

// Helper: SRRIP victim selection
static uint32_t FindVictimWay(uint32_t set) {
    while (true) {
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            if (repl_rrpv[set][w] == RRPV_MAX)
                return w;
        }
        // Aging: increment all until someone reaches MAX
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            if (repl_rrpv[set][w] < RRPV_MAX)
                repl_rrpv[set][w]++;
        }
    }
}

void InitReplacementState() {
    stat_hits   = 0;
    stat_misses = 0;
    // Initialize RRPVs and signatures
    for (uint32_t s = 0; s < LLC_SETS; s++) {
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            repl_rrpv[s][w] = RRPV_MAX;
            repl_sig [s][w] = 0;
        }
    }
    // Initialize SHCT to mid‐point
    for (uint32_t i = 0; i < SHCT_SIZE; i++) {
        shct[i] = SHCT_INIT;
    }
}

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

void UpdateReplacementState(
    uint32_t /*cpu*/,
    uint32_t set,
    uint32_t way,
    uint64_t paddr,
    uint64_t PC,
    uint64_t victim_addr,
    uint32_t type,
    uint8_t hit
) {
    if (hit) {
        // On hit: promote block and strengthen its signature
        stat_hits++;
        repl_rrpv[set][way] = 0;
        uint16_t sig = repl_sig[set][way];
        if (shct[sig] < SHCT_MAX)
            shct[sig]++;
    } else {
        // On miss: we are replacing way -> eviction of old block
        stat_misses++;
        uint16_t old_sig = repl_sig[set][way];
        // If this block was promoted (hot) but never reused, punish its signature
        if (repl_rrpv[set][way] == 0 && shct[old_sig] > 0) {
            shct[old_sig]--;
        }
        // Compute new signature from PC
        uint16_t sig = (uint16_t)(((PC >> 2) ^ (PC >> (2 + SHCT_BITS))) & (SHCT_SIZE - 1));
        repl_sig[set][way] = sig;
        // Decide insertion RRPV based on SHCT prediction
        if (shct[sig] > SHCT_THRESH) {
            repl_rrpv[set][way] = 0;           // likely reused → hot insert
        } else {
            repl_rrpv[set][way] = RRPV_MAX;    // likely one‐time → cold insert
        }
    }
}

void PrintStats() {
    uint64_t total = stat_hits + stat_misses;
    double hr = total ? (100.0 * stat_hits / total) : 0.0;
    std::cout << "==== SHiP Policy Statistics ====\n";
    std::cout << "Total refs   : " << total       << "\n";
    std::cout << "Hits         : " << stat_hits   << "\n";
    std::cout << "Misses       : " << stat_misses << "\n";
    std::cout << "Hit Rate (%) : " << hr          << "\n";
}

void PrintStats_Heartbeat() {
    PrintStats();
}