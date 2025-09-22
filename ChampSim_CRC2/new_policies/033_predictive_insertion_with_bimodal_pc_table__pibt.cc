#include <vector>
#include <cstdint>
#include <iostream>
#include "../inc/champsim_crc2.h"

#define NUM_CORE       1
#define LLC_SETS       (NUM_CORE * 2048)
#define LLC_WAYS       16

// SRRIP parameters
#define RRPV_BITS      2
#define RRPV_MAX       ((1 << RRPV_BITS) - 1)
#define RRPV_INIT      RRPV_MAX

// PC‐table parameters
#define PC_TABLE_BITS  11
#define PC_TABLE_SIZE  (1 << PC_TABLE_BITS)
#define PC_TABLE_MASK  (PC_TABLE_SIZE - 1)
#define MIN_REF        8    // Minimum samples before trusting hit rate

struct PCTEntry {
    uint16_t hits;
    uint16_t total;
};

// Global replacement state
static uint8_t     repl_rrpv[LLC_SETS][LLC_WAYS];
static PCTEntry    pc_table[PC_TABLE_SIZE];
static uint64_t    stat_hits, stat_misses;

// Initialize replacement state
void InitReplacementState() {
    // Initialize all RRPVs to MAX (long re-reference)
    for (int s = 0; s < LLC_SETS; s++) {
        for (int w = 0; w < LLC_WAYS; w++) {
            repl_rrpv[s][w] = RRPV_INIT;
        }
    }
    // Zero out PC‐table
    for (int i = 0; i < PC_TABLE_SIZE; i++) {
        pc_table[i].hits  = 0;
        pc_table[i].total = 0;
    }
    stat_hits   = 0;
    stat_misses = 0;
}

// SRRIP victim selection
uint32_t GetVictimInSet(
    uint32_t /*cpu*/,
    uint32_t set,
    const BLOCK */*current_set*/,
    uint64_t /*PC*/,
    uint64_t /*paddr*/,
    uint32_t /*type*/
) {
    while (true) {
        // 1) Look for any block with RRPV == MAX
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            if (repl_rrpv[set][w] == RRPV_MAX)
                return w;
        }
        // 2) Age all entries
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            if (repl_rrpv[set][w] < RRPV_MAX)
                repl_rrpv[set][w]++;
        }
    }
}

// Update on every access (hit or miss+install)
void UpdateReplacementState(
    uint32_t /*cpu*/,
    uint32_t set,
    uint32_t way,
    uint64_t /*paddr*/,
    uint64_t PC,
    uint64_t /*victim_addr*/,
    uint32_t /*type*/,
    uint8_t hit
) {
    // Index into PC table (simple low‐bits hash)
    uint32_t pc_idx = (PC >> 2) & PC_TABLE_MASK;

    if (hit) {
        // On hit: promote to MRU (RRPV=0), update PC counters
        stat_hits++;
        repl_rrpv[set][way] = 0;
        pc_table[pc_idx].total++;
        if (pc_table[pc_idx].hits < 0xFFFF)           // avoid overflow
            pc_table[pc_idx].hits++;
        return;
    }

    // On miss: will install at [set][way]
    stat_misses++;
    // Update PC‐table total references
    if (pc_table[pc_idx].total < 0xFFFF)             // avoid overflow
        pc_table[pc_idx].total++;

    // Decide insertion priority
    bool high_reuse = false;
    PCTEntry &e = pc_table[pc_idx];
    if (e.total >= MIN_REF && (2 * e.hits >= e.total)) {
        // >50% past hit rate and enough samples
        high_reuse = true;
    }
    // Insert with low RRPV if high reuse, else max RRPV
    repl_rrpv[set][way] = high_reuse ? 0 : RRPV_MAX;
}

// Print final statistics
void PrintStats() {
    uint64_t total = stat_hits + stat_misses;
    double   hr    = total ? (100.0 * stat_hits / total) : 0.0;
    std::cout << "==== PIBT Statistics ====\n";
    std::cout << "Total refs      : " << total       << "\n";
    std::cout << "Hits            : " << stat_hits   << "\n";
    std::cout << "Misses          : " << stat_misses << "\n";
    std::cout << "Hit Rate (%)    : " << hr          << "\n";
}

// Heartbeat prints same stats
void PrintStats_Heartbeat() {
    PrintStats();
}