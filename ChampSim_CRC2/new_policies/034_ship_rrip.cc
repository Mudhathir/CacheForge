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

// SHiP signature table parameters
#define SIG_BITS       14
#define SIG_SIZE       (1 << SIG_BITS)
#define SIG_MASK       (SIG_SIZE - 1)
#define CNTR_BITS      3
#define CNTR_MAX       ((1 << CNTR_BITS) - 1)
#define THRESHOLD      ((CNTR_MAX >> 1) + 1)  // 4 when CNTR_MAX=7

// Replacement state
static uint8_t  repl_rrpv    [LLC_SETS][LLC_WAYS];
static uint8_t  sig_table    [SIG_SIZE];                // 3-bit saturating counters
static uint16_t block_sig    [LLC_SETS][LLC_WAYS];      // signature of the block
static bool     block_refd   [LLC_SETS][LLC_WAYS];      // referenced since insertion
static bool     block_valid  [LLC_SETS][LLC_WAYS];      // valid bit for SHiP feedback

// Stats
static uint64_t stat_hits, stat_misses;

void InitReplacementState() {
    stat_hits    = 0;
    stat_misses  = 0;
    // Initialize RRPVs and block metadata
    for (int s = 0; s < LLC_SETS; s++) {
        for (int w = 0; w < LLC_WAYS; w++) {
            repl_rrpv[s][w]   = RRPV_INIT;
            block_sig[s][w]   = 0;
            block_refd[s][w]  = false;
            block_valid[s][w] = false;
        }
    }
    // Initialize signature counters to weakly taken (threshold)
    for (int i = 0; i < SIG_SIZE; i++) {
        sig_table[i] = THRESHOLD;
    }
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
    // Compute signature from PC
    uint32_t sig = (PC >> 2) & SIG_MASK;

    if (hit) {
        // On hit: promote to MRU, mark referenced
        stat_hits++;
        repl_rrpv[set][way]    = 0;
        block_refd[set][way]   = true;
        return;
    }

    // On miss: installing at [set][way]
    stat_misses++;

    // 1) If the way held a valid block, update its signature counter
    if (block_valid[set][way]) {
        uint32_t old_sig = block_sig[set][way];
        if (block_refd[set][way]) {
            // useful block => increment counter
            if (sig_table[old_sig] < CNTR_MAX)
                sig_table[old_sig]++;
        } else {
            // dead block => decrement
            if (sig_table[old_sig] > 0)
                sig_table[old_sig]--;
        }
    }
    // 2) Decide insertion RRPV based on current signature entry
    if (sig_table[sig] >= THRESHOLD) {
        // predicted high reuse
        repl_rrpv[set][way] = 0;
    } else {
        // predicted low reuse
        repl_rrpv[set][way] = RRPV_MAX;
    }
    // 3) Install metadata for new block
    block_sig[set][way]   = sig;
    block_refd[set][way]  = false;
    block_valid[set][way] = true;
}

void PrintStats() {
    uint64_t total = stat_hits + stat_misses;
    double   hr    = total ? (100.0 * stat_hits / total) : 0.0;
    std::cout << "==== SHiP-RRIP Statistics ====\n";
    std::cout << "Total refs      : " << total       << "\n";
    std::cout << "Hits            : " << stat_hits   << "\n";
    std::cout << "Misses          : " << stat_misses << "\n";
    std::cout << "Hit Rate (%)    : " << hr          << "\n";
}

void PrintStats_Heartbeat() {
    PrintStats();
}