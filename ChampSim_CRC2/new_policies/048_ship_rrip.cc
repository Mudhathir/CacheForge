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

// Signature predictor parameters
#define SIG_CTR_BITS     3
#define SIG_CTR_MAX      ((1 << SIG_CTR_BITS) - 1)
#define SIG_TABLE_SIZE   1024
#define SIG_INIT         (SIG_CTR_MAX / 2)
#define GET_SIG(PC)      (uint8_t)(((PC) >> 6) & (SIG_TABLE_SIZE - 1))

// Replacement metadata
static uint8_t repl_rrpv [LLC_SETS][LLC_WAYS];
static uint8_t block_sig [LLC_SETS][LLC_WAYS];
static uint8_t block_hit [LLC_SETS][LLC_WAYS];
// Signature counters
static uint8_t sig_ctr   [SIG_TABLE_SIZE];
// Statistics
static uint64_t stat_hits;
static uint64_t stat_misses;

// SRRIP victim selection
static uint32_t FindVictimWay(uint32_t set) {
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

void InitReplacementState() {
    stat_hits   = 0;
    stat_misses = 0;
    // Init signature counters
    for (uint32_t i = 0; i < SIG_TABLE_SIZE; i++)
        sig_ctr[i] = SIG_INIT;
    // Init per-block metadata
    for (uint32_t s = 0; s < LLC_SETS; s++) {
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            repl_rrpv[s][w] = RRPV_MAX;
            block_sig[s][w] = 0;
            block_hit[s][w] = 0;
        }
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
    uint64_t /*paddr*/,
    uint64_t PC,
    uint64_t /*victim_addr*/,
    uint32_t /*type*/,
    uint8_t hit
) {
    if (hit) {
        // On hit: promote to MRU and mark reused
        stat_hits++;
        repl_rrpv[set][way] = 0;
        block_hit[set][way] = 1;
    } else {
        // On miss: update signature of evicted block
        stat_misses++;
        uint8_t old_sig = block_sig[set][way];
        if (block_hit[set][way]) {
            if (sig_ctr[old_sig] < SIG_CTR_MAX)
                sig_ctr[old_sig]++;
        } else {
            if (sig_ctr[old_sig] > 0)
                sig_ctr[old_sig]--;
        }
        // Install new block
        uint8_t sig = GET_SIG(PC);
        block_sig[set][way] = sig;
        block_hit[set][way] = 0;
        // Insertion: MRU if signature predicts reuse, else near-LRU
        if (sig_ctr[sig] > (SIG_CTR_MAX >> 1))
            repl_rrpv[set][way] = 0;
        else
            repl_rrpv[set][way] = RRPV_MAX - 1;
    }
}

void PrintStats() {
    uint64_t total = stat_hits + stat_misses;
    double hr = total ? (100.0 * stat_hits / total) : 0.0;
    std::cout << "==== SHIP-RRIP Policy Statistics ====\n";
    std::cout << "Total refs   : " << total       << "\n";
    std::cout << "Hits         : " << stat_hits   << "\n";
    std::cout << "Misses       : " << stat_misses << "\n";
    std::cout << "Hit Rate (%) : " << hr          << "\n";
}

void PrintStats_Heartbeat() {
    PrintStats();
}