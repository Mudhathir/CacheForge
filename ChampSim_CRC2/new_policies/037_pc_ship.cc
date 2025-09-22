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

// --- PC‐based reuse counter parameters ---
#define SIG_BITS    14
#define SIG_SIZE    (1 << SIG_BITS)
#define SIG_MASK    (SIG_SIZE - 1)
#define CTR_BITS    3
#define CTR_MAX     ((1 << CTR_BITS) - 1)    // 7
// Four insertion bands: [0..TH1-1], [TH1..TH2-1], [TH2..TH3-1], [TH3..CTR_MAX]
#define TH1         ((CTR_MAX >> 2) + 1)     // ≈2
#define TH2         ((CTR_MAX >> 1) + 1)     // ≈4
#define TH3         (((CTR_MAX * 3) >> 2) + 1) // ≈6

// Replacement state
static uint8_t  repl_rrpv   [LLC_SETS][LLC_WAYS];
static uint8_t  pc_table    [SIG_SIZE];               // per‐PC saturating counters
static uint16_t block_sig   [LLC_SETS][LLC_WAYS];     // stored PC signature
static bool     block_refd  [LLC_SETS][LLC_WAYS];     // whether block was referenced since insert
static bool     block_valid [LLC_SETS][LLC_WAYS];     // valid for feedback
static uint64_t stat_hits, stat_misses;

// Initialize replacement state
void InitReplacementState() {
    stat_hits   = 0;
    stat_misses = 0;
    // Initialize RRIP and block metadata
    for (int s = 0; s < LLC_SETS; s++) {
        for (int w = 0; w < LLC_WAYS; w++) {
            repl_rrpv[s][w]   = RRPV_MAX;
            block_sig[s][w]   = 0;
            block_refd[s][w]  = false;
            block_valid[s][w] = false;
        }
    }
    // Initialize all PC counters to “cold” (favor far insertion)
    for (int i = 0; i < SIG_SIZE; i++) {
        pc_table[i] = 0;
    }
}

// Find victim in the set (standard SRRIP victim search)
uint32_t GetVictimInSet(
    uint32_t /*cpu*/,
    uint32_t set,
    const BLOCK */*current_set*/,
    uint64_t /*PC*/,
    uint64_t /*paddr*/,
    uint32_t /*type*/
) {
    while (true) {
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            if (repl_rrpv[set][w] == RRPV_MAX)
                return w;
        }
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            if (repl_rrpv[set][w] < RRPV_MAX)
                repl_rrpv[set][w]++;
        }
    }
}

// Update replacement state on each access
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
    // On hit: promote to MRU
    if (hit) {
        stat_hits++;
        repl_rrpv[set][way]  = 0;
        block_refd[set][way] = true;
        return;
    }

    // On miss / install
    stat_misses++;

    // 1) Feedback: update the PC counter of the evicted block
    if (block_valid[set][way]) {
        uint16_t old_sig = block_sig[set][way];
        if (block_refd[set][way]) {
            if (pc_table[old_sig] < CTR_MAX)
                pc_table[old_sig]++;
        } else {
            if (pc_table[old_sig] > 0)
                pc_table[old_sig]--;
        }
    }

    // 2) Insert new block using its PC‐counter to pick insertion RRPV
    uint16_t sig = (PC >> 2) & SIG_MASK;
    uint8_t ctr  = pc_table[sig];
    if (ctr < TH1) {
        // low reuse → far insertion
        repl_rrpv[set][way] = RRPV_MAX;
    }
    else if (ctr < TH2) {
        // some reuse → medium‐long
        repl_rrpv[set][way] = RRPV_MAX - 1;
    }
    else if (ctr < TH3) {
        // moderate reuse → medium‐short
        repl_rrpv[set][way] = RRPV_MAX - 2;
    }
    else {
        // heavy reuse → MRU
        repl_rrpv[set][way] = 0;
    }
    block_sig[set][way]   = sig;
    block_refd[set][way]  = false;
    block_valid[set][way] = true;
}

// Print end‐of‐simulation statistics
void PrintStats() {
    uint64_t total = stat_hits + stat_misses;
    double   hr    = total ? (100.0 * stat_hits / total) : 0.0;
    std::cout << "==== PC-SHiP Policy Statistics ====\n";
    std::cout << "Total refs      : " << total       << "\n";
    std::cout << "Hits            : " << stat_hits   << "\n";
    std::cout << "Misses          : " << stat_misses << "\n";
    std::cout << "Hit Rate (%)    : " << hr          << "\n";
}

// Print periodic (heartbeat) statistics
void PrintStats_Heartbeat() {
    PrintStats();
}