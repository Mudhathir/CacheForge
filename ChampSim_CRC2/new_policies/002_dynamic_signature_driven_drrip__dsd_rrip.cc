#include <vector>
#include <cstdint>
#include <iostream>
#include "../inc/champsim_crc2.h"

#define NUM_CORE            1
#define LLC_SETS            (NUM_CORE * 2048)
#define LLC_WAYS            16

// RRPV parameters
#define RRPV_BITS           2
#define RRPV_MAX            ((1 << RRPV_BITS) - 1)   // 3

// SHCT parameters
#define SHCT_SIZE           4096
#define SHCT_INIT           4
#define SHCT_MAX            7
#define HOT_THRESHOLD       5

// DRRIP parameters
#define SAMP_MOD            64                        // number of sample sets
#define SAMP_SRRIP_MAX      (SAMP_MOD/2)              // first half => SRRIP
#define PSEL_BITS           10
#define PSEL_MAX            ((1 << PSEL_BITS) - 1)
#define PSEL_INIT           (PSEL_MAX / 2)
#define BIP_PROB_BITS       5                         // 1/32 prob to insert at RRPV=0
#define BIP_PROB_MASK       ((1 << BIP_PROB_BITS) - 1)

// Per‐line state
static uint8_t  repl_rrpv     [LLC_SETS][LLC_WAYS];
static uint16_t repl_sig      [LLC_SETS][LLC_WAYS];
static uint8_t  block_hit_flag[LLC_SETS][LLC_WAYS];

// Signature Counter Table
static uint8_t  SHCT[SHCT_SIZE];

// Global policy selector
static int      PSEL;

// Statistics
static uint64_t stat_hits      = 0;
static uint64_t stat_misses    = 0;
static uint64_t stat_evictions = 0;

void InitReplacementState() {
    // Initialize RRPVs, signatures, and hit flags
    for (uint32_t s = 0; s < LLC_SETS; s++) {
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            repl_rrpv[s][w]       = RRPV_MAX;
            repl_sig[s][w]        = 0;
            block_hit_flag[s][w]  = 0;
        }
    }
    // Init SHCT
    for (uint32_t i = 0; i < SHCT_SIZE; i++) {
        SHCT[i] = SHCT_INIT;
    }
    // Init PSEL
    PSEL = PSEL_INIT;
    // Stats
    stat_hits = stat_misses = stat_evictions = 0;
}

// Standard SRRIP victim search
uint32_t GetVictimInSet(
    uint32_t cpu,
    uint32_t set,
    const BLOCK *current_set,
    uint64_t PC,
    uint64_t paddr,
    uint32_t type
) {
    while (true) {
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            if (repl_rrpv[set][w] == RRPV_MAX) {
                stat_evictions++;
                return w;
            }
        }
        // Age all
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            if (repl_rrpv[set][w] < RRPV_MAX) {
                repl_rrpv[set][w]++;
            }
        }
    }
    return 0;
}

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
    // Determine if this is a sample set
    uint32_t M = set % SAMP_MOD;
    bool is_srrip_sample = (M < SAMP_SRRIP_MAX);
    bool is_bip_sample   = (M >= SAMP_SRRIP_MAX && M < SAMP_MOD);

    if (hit) {
        // On hit: update RRPV, SHCT, PSEL if sample
        repl_rrpv[set][way] = 0;
        uint16_t sig = repl_sig[set][way];
        if (SHCT[sig] < SHCT_MAX) SHCT[sig]++;
        block_hit_flag[set][way] = 1;
        stat_hits++;
        // Adjust PSEL based on sample hits
        if (is_srrip_sample && PSEL < PSEL_MAX)      PSEL++;
        else if (is_bip_sample && PSEL > 0)           PSEL--;
    }
    else {
        // On miss/fill: eviction feedback
        uint16_t old_sig = repl_sig[set][way];
        if (!block_hit_flag[set][way] && SHCT[old_sig] > 0) {
            SHCT[old_sig]--;
        }
        // Compute new signature
        uint16_t sig = (uint16_t)((PC ^ (paddr >> 12)) & (SHCT_SIZE - 1));
        repl_sig[set][way]       = sig;
        block_hit_flag[set][way] = 0;

        // Decide insertion policy
        bool useSRRIP;
        if      (is_srrip_sample) useSRRIP = true;
        else if (is_bip_sample)   useSRRIP = false;
        else                       useSRRIP = (PSEL >= PSEL_INIT);

        // Tri‐level: hot override
        if (SHCT[sig] >= HOT_THRESHOLD) {
            // Always keep hot blocks
            repl_rrpv[set][way] = 0;
        }
        else {
            if (useSRRIP) {
                // SRRIP insertion
                repl_rrpv[set][way] = RRPV_MAX - 1;
            } else {
                // Bimodal insertion: 1/32 chance to RRPV=0, else RRPV=MAX
                uint64_t rnd = (PC ^ (paddr >> 12)) & BIP_PROB_MASK;
                if (rnd == 0) repl_rrpv[set][way] = 0;
                else          repl_rrpv[set][way] = RRPV_MAX;
            }
        }
        stat_misses++;
    }
}

void PrintStats() {
    uint64_t total = stat_hits + stat_misses;
    double hr = total ? (double)stat_hits / total * 100.0 : 0.0;
    std::cout << "==== DSD-RRIP Replacement Stats ====\n";
    std::cout << "Total refs   : " << total << "\n";
    std::cout << "Hits         : " << stat_hits << "\n";
    std::cout << "Misses       : " << stat_misses << "\n";
    std::cout << "Evictions    : " << stat_evictions << "\n";
    std::cout << "Hit rate (%) : " << hr << "\n";
    std::cout << "PSEL         : " << PSEL << " / " << PSEL_MAX << "\n";
}

void PrintStats_Heartbeat() {
    PrintStats();
}