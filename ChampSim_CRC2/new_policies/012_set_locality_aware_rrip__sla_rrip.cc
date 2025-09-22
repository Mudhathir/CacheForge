#include <vector>
#include <cstdint>
#include <iostream>
#include "../inc/champsim_crc2.h"

#define NUM_CORE 1
#define LLC_SETS (NUM_CORE * 2048)
#define LLC_WAYS 16

// RRIP parameters
#define RRPV_BITS   2
#define RRPV_MAX    ((1 << RRPV_BITS) - 1)  // 3
#define SRRIP_INIT  (RRPV_MAX - 1)          // 2

// Per-set saturating counter parameters
#define SCOUNTER_BITS    4
#define SCOUNTER_MAX     ((1 << (SCOUNTER_BITS - 1)) - 1)  // +7
#define SCOUNTER_MIN     (-(1 << (SCOUNTER_BITS - 1)))     // -8
#define HOT_THRESHOLD    (SCOUNTER_MAX / 2)                // +3
#define COLD_THRESHOLD   0

// Replacement state
static uint8_t  repl_rrpv   [LLC_SETS][LLC_WAYS];
static int8_t   set_counter [LLC_SETS];

// Statistics
static uint64_t stat_hits      = 0;
static uint64_t stat_misses    = 0;
static uint64_t stat_evictions = 0;

// Initialize replacement state
void InitReplacementState() {
    for (uint32_t s = 0; s < LLC_SETS; s++) {
        set_counter[s] = 0;
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            repl_rrpv[s][w] = RRPV_MAX;
        }
    }
    stat_hits = stat_misses = stat_evictions = 0;
}

// Find victim in the set using standard RRIP aging
uint32_t GetVictimInSet(
    uint32_t cpu,
    uint32_t set,
    const BLOCK *current_set,
    uint64_t PC,
    uint64_t paddr,
    uint32_t type
) {
    while (true) {
        // Look for an entry with RRPV == RRPV_MAX
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            if (repl_rrpv[set][w] == RRPV_MAX) {
                return w;
            }
        }
        // Age all entries
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            if (repl_rrpv[set][w] < RRPV_MAX) {
                repl_rrpv[set][w]++;
            }
        }
    }
    // unreachable
    return 0;
}

// Update replacement state on hit or miss
void UpdateReplacementState(
    uint32_t cpu,
    uint32_t set,
    uint32_t way,
    uint64_t paddr,
    uint64_t PC,
    uint64_t victim_addr,
    uint32_t type,
    uint8_t hit
) {
    if (hit) {
        // On hit: promote to MRU and update set counter
        stat_hits++;
        repl_rrpv[set][way] = 0;
        if (set_counter[set] < SCOUNTER_MAX) {
            set_counter[set]++;
        }
    } else {
        // On miss & eviction: update stats and set counter
        stat_misses++;
        stat_evictions++;
        if (set_counter[set] > SCOUNTER_MIN) {
            set_counter[set]--;
        }
        // Decide insertion priority based on set counter
        if (set_counter[set] >= HOT_THRESHOLD) {
            // heavy reuse: insert at MRU
            repl_rrpv[set][way] = 0;
        } else if (set_counter[set] <= COLD_THRESHOLD) {
            // streaming/no reuse: approximate bypass
            repl_rrpv[set][way] = RRPV_MAX;
        } else {
            // moderate reuse: standard SRRIP insertion
            repl_rrpv[set][way] = SRRIP_INIT;
        }
    }
}

// Print end-of-simulation statistics
void PrintStats() {
    uint64_t total = stat_hits + stat_misses;
    double   hr    = total ? (double)stat_hits / total * 100.0 : 0.0;
    std::cout << "==== Set-Locality Aware RRIP Stats ====\n";
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