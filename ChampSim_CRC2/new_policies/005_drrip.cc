#include <vector>
#include <cstdint>
#include <iostream>
#include "../inc/champsim_crc2.h"

#define NUM_CORE         1
#define LLC_SETS         (NUM_CORE * 2048)
#define LLC_WAYS         16

// RRIP parameters
#define RRPV_BITS        2
#define RRPV_MAX         ((1 << RRPV_BITS) - 1)

// DRRIP parameters
#define DRRIP_PSEL_BITS  10
#define DRRIP_PSEL_MAX   ((1 << DRRIP_PSEL_BITS) - 1)
#define DRRIP_PSEL_INIT  (DRRIP_PSEL_MAX / 2)
#define BIP_PROBABILITY  32          // 1/32 MRU insertion in BIP
#define LEADER_SETS      32          // number of sets per leader group

// Replacement state
static uint8_t   repl_rrpv   [LLC_SETS][LLC_WAYS];
static uint32_t  PSEL;                   // policy selector counter
static uint32_t  bip_counter;            // for BIP probabilistic choice

// Statistics
static uint64_t stat_hits      = 0;
static uint64_t stat_misses    = 0;
static uint64_t stat_evictions = 0;

// Initialize replacement state
void InitReplacementState() {
    // Initialize all RRPVs to long (MAX)
    for (uint32_t s = 0; s < LLC_SETS; s++) {
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            repl_rrpv[s][w] = RRPV_MAX;
        }
    }
    // Initialize DRRIP selector and BIP counter
    PSEL = DRRIP_PSEL_INIT;
    bip_counter = 0;
    // Clear stats
    stat_hits = stat_misses = stat_evictions = 0;
}

// Victim selection using standard RRIP
uint32_t GetVictimInSet(
    uint32_t cpu,
    uint32_t set,
    const BLOCK *current_set,
    uint64_t PC,
    uint64_t paddr,
    uint32_t type
) {
    while (true) {
        // Look for an entry with RRPV == MAX
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

// Update on hit or miss
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
        // On hit: promote to MRU
        repl_rrpv[set][way] = 0;
        stat_hits++;
    } else {
        // On miss and fill: we evict the old line at [set][way]
        stat_misses++;
        stat_evictions++;
        // Determine set role: leader_SRRIP, leader_BIP, or follower
        uint32_t group_id = set % (2 * LEADER_SETS);
        bool is_leader_s = (group_id < LEADER_SETS);
        bool is_leader_b = (group_id >= LEADER_SETS && group_id < 2 * LEADER_SETS);
        bool use_SRRIP;
        // Update PSEL in leader sets
        if (is_leader_s) {
            use_SRRIP = true;
            if (PSEL > 0) PSEL--;
        } else if (is_leader_b) {
            use_SRRIP = false;
            if (PSEL < DRRIP_PSEL_MAX) PSEL++;
        } else {
            // follower sets adopt current champion
            use_SRRIP = (PSEL <= (DRRIP_PSEL_MAX / 2));
        }
        // BIP probabilistic MRU insertion
        bip_counter++;
        bool bip_mru = ((bip_counter & (BIP_PROBABILITY - 1)) == 0);
        // Insert new block with chosen policy
        if (use_SRRIP) {
            // SRRIP: insert at RRPV = MAX-1 (moderate retention)
            repl_rrpv[set][way] = RRPV_MAX - 1;
        } else {
            // BIP: mostly insert at MAX (quick eviction), rare MRU
            repl_rrpv[set][way] = bip_mru ? 0 : RRPV_MAX;
        }
    }
}

// Print final statistics
void PrintStats() {
    uint64_t total = stat_hits + stat_misses;
    double   hr    = total ? (double)stat_hits / total * 100.0 : 0.0;
    std::cout << "==== DRRIP Replacement Stats ====\n";
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