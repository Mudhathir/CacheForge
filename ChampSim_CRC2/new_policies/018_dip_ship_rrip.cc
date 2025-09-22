#include <vector>
#include <cstdint>
#include <iostream>
#include "../inc/champsim_crc2.h"

#define NUM_CORE 1
#define LLC_SETS (NUM_CORE * 2048)
#define LLC_WAYS 16

// RRIP parameters
#define RRPV_BITS    3
#define RRPV_MAX     ((1 << RRPV_BITS) - 1)  // 7
#define SRRIP_INIT   (RRPV_MAX - 1)          // 6

// SHiP parameters
#define SHCT_CTR_BITS    3
#define SHCT_CTR_MAX     ((1 << SHCT_CTR_BITS) - 1)  // 7
#define SHCT_INIT        (SHCT_CTR_MAX >> 1)         // 3
#define SHCT_THRESHOLD   SHCT_INIT
#define SHCT_SIZE        4096

// Set-dueling DIP parameters
#define SETDUEL_DIVISOR 32      // 1 sample SRRIP set, 1 sample SHiP set per 32 sets
#define PSEL_BITS       10
#define PSEL_MAX        ((1 << PSEL_BITS) - 1)       // 1023
#define PSEL_INIT       (PSEL_MAX >> 1)              // 511
#define PSEL_THRESHOLD  PSEL_INIT

// Replacement metadata
static uint8_t   repl_rrpv[LLC_SETS][LLC_WAYS];
static bool      repl_has_hit[LLC_SETS][LLC_WAYS];
static uint16_t  repl_sig[LLC_SETS][LLC_WAYS];

// SHiP signature history counter table
static uint8_t SHCT[SHCT_SIZE];

// Set-dueling global policy selector
static uint16_t psel;

// Stats
static uint64_t stat_hits, stat_misses, stat_evictions;

// Helpers to identify sample/cohort sets
static inline bool is_sample_srrip(uint32_t set) {
    return (set % SETDUEL_DIVISOR) == 0;
}
static inline bool is_sample_ship(uint32_t set) {
    return (set % SETDUEL_DIVISOR) == 1;
}
// Decide which policy to use on a non-sample set
static inline bool use_ship_policy(uint32_t set) {
    if (is_sample_srrip(set)) return false;
    if (is_sample_ship(set))  return true;
    return (psel >= PSEL_THRESHOLD);
}

void InitReplacementState() {
    // Initialize RRIP state and SHiP metadata
    for (uint32_t s = 0; s < LLC_SETS; s++) {
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            repl_rrpv[s][w]     = RRPV_MAX;
            repl_has_hit[s][w]  = false;
            repl_sig[s][w]      = 0;
        }
    }
    // Initialize SHCT to neutral
    for (uint32_t i = 0; i < SHCT_SIZE; i++) {
        SHCT[i] = SHCT_INIT;
    }
    // Initialize PSEL and stats
    psel          = PSEL_INIT;
    stat_hits     = 0;
    stat_misses   = 0;
    stat_evictions= 0;
}

uint32_t GetVictimInSet(
    uint32_t cpu,
    uint32_t set,
    const BLOCK */* current_set */,
    uint64_t /* PC */,
    uint64_t /* paddr */,
    uint32_t /* type */
) {
    // Common SRRIP victim search and aging
    while (true) {
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            if (repl_rrpv[set][w] == RRPV_MAX) {
                return w;
            }
        }
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            if (repl_rrpv[set][w] < RRPV_MAX) {
                repl_rrpv[set][w]++;
            }
        }
    }
    return 0; // unreachable
}

void UpdateReplacementState(
    uint32_t cpu,
    uint32_t set,
    uint32_t way,
    uint64_t /* paddr */,
    uint64_t PC,
    uint64_t /* victim_addr */,
    uint32_t /* type */,
    uint8_t hit
) {
    bool sample_sr   = is_sample_srrip(set);
    bool sample_sh   = is_sample_ship(set);
    bool use_ship    = use_ship_policy(set);

    if (hit) {
        // Global hit counting
        stat_hits++;
        // Always promote in RRIP
        repl_rrpv[set][way] = 0;
        // SHiP-specific hit tracking
        if (use_ship) {
            repl_has_hit[set][way] = true;
            uint16_t sig = repl_sig[set][way];
            if (SHCT[sig] < SHCT_CTR_MAX) {
                SHCT[sig]++;
            }
        }
        // Update PSEL based on sample hits
        if (sample_sr) {
            if (psel > 0) psel--;
        } else if (sample_sh) {
            if (psel < PSEL_MAX) psel++;
        }
    } else {
        // Miss & eviction
        stat_misses++;
        stat_evictions++;
        // SHiP updates on victim
        if (use_ship) {
            uint16_t old_sig = repl_sig[set][way];
            if (!repl_has_hit[set][way] && SHCT[old_sig] > 0) {
                SHCT[old_sig]--;
            }
        }
        // Compute new block signature
        uint16_t new_sig = (uint32_t)((PC >> 2) & (SHCT_SIZE - 1));
        repl_sig[set][way]     = new_sig;
        repl_has_hit[set][way] = false;
        // Insertion decision
        if (use_ship) {
            uint8_t ctr = SHCT[new_sig];
            if (ctr > SHCT_THRESHOLD) {
                repl_rrpv[set][way] = 0;            // MRU insertion
            } else if (ctr == 0) {
                repl_rrpv[set][way] = RRPV_MAX;     // Bypass
            } else {
                repl_rrpv[set][way] = SRRIP_INIT;   // Default SRRIP
            }
        } else {
            // Plain SRRIP insertion
            repl_rrpv[set][way] = SRRIP_INIT;
        }
        // No PSEL update on misses (DIP tracks hits only)
    }
}

void PrintStats() {
    uint64_t total = stat_hits + stat_misses;
    double   hr    = total ? double(stat_hits) / double(total) * 100.0 : 0.0;
    std::cout << "==== DIP-SHiP-RRIP Statistics ====\n";
    std::cout << "Total refs    : " << total         << "\n";
    std::cout << "Hits          : " << stat_hits     << "\n";
    std::cout << "Misses        : " << stat_misses   << "\n";
    std::cout << "Evictions     : " << stat_evictions<< "\n";
    std::cout << "Hit rate (%)  : " << hr            << "\n";
    std::cout << "PSEL          : " << psel          << "\n";
}

void PrintStats_Heartbeat() {
    PrintStats();
}