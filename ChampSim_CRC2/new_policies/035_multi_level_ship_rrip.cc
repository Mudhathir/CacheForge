#include <vector>
#include <cstdint>
#include <iostream>
#include "../inc/champsim_crc2.h"

#define NUM_CORE       1
#define LLC_SETS       (NUM_CORE * 2048)
#define LLC_WAYS       16

// --- RRIP parameters ---
#define RRPV_BITS      2
#define RRPV_MAX       ((1 << RRPV_BITS) - 1)   // 3

// --- Signature table parameters ---
#define SIG_BITS       14
#define SIG_SIZE       (1 << SIG_BITS)
#define SIG_MASK       (SIG_SIZE - 1)
#define CNTR_BITS      4
#define CNTR_MAX       ((1 << CNTR_BITS) - 1)   // 15

// We split [0..CNTR_MAX] into four bands:
//   [0 .. TH1-1]        → streaming/low reuse   → RRPV = 3
//   [TH1 .. TH2-1]      → low reuse             → RRPV = 2
//   [TH2 .. TH3-1]      → medium reuse          → RRPV = 1
//   [TH3 .. CNTR_MAX]   → high reuse            → RRPV = 0
#define TH1             ((CNTR_MAX >> 2) + 1)   // ≈ 15/4 +1 = 4
#define TH2             ((CNTR_MAX >> 1) + 1)   // ≈ 15/2 +1 = 8
#define TH3             (((CNTR_MAX * 3) >> 2) + 1) // ≈ 45/4 +1 = 12

// Replacement state
static uint8_t  repl_rrpv   [LLC_SETS][LLC_WAYS];
static uint8_t  sig_table   [SIG_SIZE];               // 4-bit saturating counters
static uint16_t block_sig   [LLC_SETS][LLC_WAYS];     // stored signature
static bool     block_refd  [LLC_SETS][LLC_WAYS];     // referenced since insert
static bool     block_valid [LLC_SETS][LLC_WAYS];     // indicates valid for feedback

// Stats
static uint64_t stat_hits, stat_misses;

// Initialize replacement state
void InitReplacementState() {
    stat_hits   = 0;
    stat_misses = 0;
    // Initialize each way to long re-reference prediction
    for (int s = 0; s < LLC_SETS; s++) {
        for (int w = 0; w < LLC_WAYS; w++) {
            repl_rrpv[s][w]   = RRPV_MAX;
            block_sig[s][w]   = 0;
            block_refd[s][w]  = false;
            block_valid[s][w] = false;
        }
    }
    // Initialize all PC signatures to the medium‐reuse band
    for (int i = 0; i < SIG_SIZE; i++) {
        sig_table[i] = TH2;  // signals “medium reuse” at startup
    }
}

// Find victim in the set using SRRIP eviction rules
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

// Update replacement state (on hit or miss+install)
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
    // PC-based signature index
    uint32_t sig = (PC >> 2) & SIG_MASK;

    if (hit) {
        // On hit: mark as MRU and record reuse
        stat_hits++;
        repl_rrpv[set][way]   = 0;
        block_refd[set][way]  = true;
        return;
    }

    // On miss: installing at [set][way]
    stat_misses++;

    // 1) If victim was valid, update its PC signature counter
    if (block_valid[set][way]) {
        uint32_t old = block_sig[set][way];
        if (block_refd[set][way]) {
            // useful → increment
            if (sig_table[old] < CNTR_MAX)
                sig_table[old]++;
        } else {
            // dead → decrement
            if (sig_table[old] > 0)
                sig_table[old]--;
        }
    }

    // 2) Decide insertion RRPV based on new block's signature
    uint8_t ctr = sig_table[sig];
    if (ctr < TH1) {
        repl_rrpv[set][way] = RRPV_MAX;        // streaming/very low reuse
    }
    else if (ctr < TH2) {
        repl_rrpv[set][way] = RRPV_MAX - 1;    // low reuse
    }
    else if (ctr < TH3) {
        repl_rrpv[set][way] = RRPV_MAX - 2;    // medium reuse
    }
    else {
        repl_rrpv[set][way] = 0;               // high reuse
    }

    // 3) Install metadata for the new block
    block_sig[set][way]   = sig;
    block_refd[set][way]  = false;
    block_valid[set][way] = true;
}

// Print end-of-simulation statistics
void PrintStats() {
    uint64_t total = stat_hits + stat_misses;
    double   hr    = total ? (100.0 * stat_hits / total) : 0.0;
    std::cout << "==== Multi-Level SHiP-RRIP Statistics ====\n";
    std::cout << "Total refs      : " << total       << "\n";
    std::cout << "Hits            : " << stat_hits   << "\n";
    std::cout << "Misses          : " << stat_misses << "\n";
    std::cout << "Hit Rate (%)    : " << hr          << "\n";
}

// Print periodic (heartbeat) statistics
void PrintStats_Heartbeat() {
    PrintStats();
}