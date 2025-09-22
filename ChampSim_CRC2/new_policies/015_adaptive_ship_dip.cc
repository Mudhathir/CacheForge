#include <vector>
#include <cstdint>
#include <iostream>
#include <cstdlib>
#include "../inc/champsim_crc2.h"

#define NUM_CORE           1
#define LLC_SETS           (NUM_CORE * 2048)
#define LLC_WAYS           16

// RRIP parameters
#define RRPV_BITS          3
#define RRPV_MAX           ((1 << RRPV_BITS) - 1)   // 7
#define SRRIP_INIT         (RRPV_MAX - 1)           // 6

// Signature‐based Hit Counter Table (SHCT) parameters
#define SHCT_BITS          2
#define SHCT_SIZE          (1 << SHCT_BITS)         // 4 entries
#define SHCT_MAX           (SHCT_SIZE - 1)          // 3
#define SHCT_INIT          1                        // weakly‐taken

// Per‐PC signature table
#define PHT_ENTRIES        16384
#define PHT_MASK           (PHT_ENTRIES - 1)
#define PHT_INDEX(pc)      (((pc) >> 4) & PHT_MASK)

// Dynamic Insertion Policy (DIP) parameters
#define PSEL_BITS          10
#define PSEL_MAX           ((1 << PSEL_BITS) - 1)
#define PSEL_THRESHOLD     (PSEL_MAX / 2)
#define DIP_PROB           32     // 1/32 for BRRIP high‐priority
#define NUM_LEADER_SETS    64     // per policy

// Epoch for dynamic threshold tuning
static const uint64_t EPOCH_LEN     = 1000000ULL;
static const double   HIGH_MISS_RATE= 0.50;
static const double   LOW_MISS_RATE = 0.20;

// Replacement state
static uint8_t   repl_rrpv   [LLC_SETS][LLC_WAYS];
static uint16_t  repl_sig    [LLC_SETS][LLC_WAYS];
static bool      repl_has_hit[LLC_SETS][LLC_WAYS];
static uint8_t   SHCT        [PHT_ENTRIES];
static uint16_t  PSEL;               // global DIP selector

// Dynamic threshold & epoch stats
static uint8_t   SHCT_THRESHOLD;
static uint64_t  epoch_refs;
static uint64_t  epoch_misses;

// Hit/miss/eviction stats
static uint64_t  stat_hits;
static uint64_t  stat_misses;
static uint64_t  stat_evictions;

// Initialize replacement state
void InitReplacementState() {
    for (uint32_t s = 0; s < LLC_SETS; s++) {
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            repl_rrpv[s][w]    = RRPV_MAX;
            repl_sig[s][w]     = 0;
            repl_has_hit[s][w] = false;
        }
    }
    for (uint32_t i = 0; i < PHT_ENTRIES; i++) {
        SHCT[i] = SHCT_INIT;
    }
    PSEL           = PSEL_THRESHOLD;
    SHCT_THRESHOLD = 2;        // start moderate
    epoch_refs     = 0;
    epoch_misses   = 0;
    stat_hits      = stat_misses = stat_evictions = 0;
    std::srand(0xDEADBEEF);
}

// Find victim in the set using RRIP aging
uint32_t GetVictimInSet(
    uint32_t cpu,
    uint32_t set,
    const BLOCK *current_set,
    uint64_t PC,
    uint64_t paddr,
    uint32_t type
) {
    // Look for an RRPV == max; if none, age and retry
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
    // Epoch counting
    epoch_refs++;
    if (!hit) epoch_misses++;

    if (hit) {
        // Hit path: promote and update SHCT
        stat_hits++;
        repl_rrpv[set][way]    = 0;
        if (!repl_has_hit[set][way]) {
            uint16_t sig = repl_sig[set][way];
            if (SHCT[sig] < SHCT_MAX) SHCT[sig]++;
        }
        repl_has_hit[set][way] = true;
        return;
    }

    // Miss path: eviction feedback + DIP/PSEL
    stat_misses++;
    stat_evictions++;
    uint16_t old_sig = repl_sig[set][way];
    if (repl_has_hit[set][way]) {
        if (SHCT[old_sig] < SHCT_MAX) SHCT[old_sig]++;
    } else {
        if (SHCT[old_sig] > 0)        SHCT[old_sig]--;
    }

    // DIP leader updates
    if (set < NUM_LEADER_SETS) {
        if (PSEL > 0) PSEL--;
    } else if (set < 2 * NUM_LEADER_SETS) {
        if (PSEL < PSEL_MAX) PSEL++;
    }

    // Dynamic threshold adjustment at epoch boundaries
    if (epoch_refs >= EPOCH_LEN) {
        double miss_rate = double(epoch_misses) / double(epoch_refs);
        if (miss_rate > HIGH_MISS_RATE && SHCT_THRESHOLD < SHCT_MAX) {
            SHCT_THRESHOLD++;
        } else if (miss_rate < LOW_MISS_RATE && SHCT_THRESHOLD > 1) {
            SHCT_THRESHOLD--;
        }
        epoch_refs   = 0;
        epoch_misses = 0;
    }

    // Insert new block: compute signature
    uint16_t sig = PHT_INDEX(PC);
    repl_sig[set][way]     = sig;
    repl_has_hit[set][way] = false;
    uint8_t pred = SHCT[sig];

    if (pred >= SHCT_THRESHOLD) {
        // heavy‐reuse: MRU
        repl_rrpv[set][way] = 0;
    }
    else if (pred == 0) {
        // streaming / no reuse: lowest priority
        repl_rrpv[set][way] = RRPV_MAX;
    }
    else {
        // uncertain: choose between SRRIP/BRRIP
        bool use_srrip;
        if (set < NUM_LEADER_SETS) {
            use_srrip = true;
        } else if (set < 2 * NUM_LEADER_SETS) {
            use_srrip = false;
        } else {
            use_srrip = (PSEL > PSEL_THRESHOLD);
        }
        if (use_srrip) {
            repl_rrpv[set][way] = SRRIP_INIT;
        } else {
            // BRRIP insertion
            if ((std::rand() % DIP_PROB) == 0) {
                repl_rrpv[set][way] = SRRIP_INIT;
            } else {
                repl_rrpv[set][way] = RRPV_MAX;
            }
        }
    }
}

// Print end‐of‐simulation statistics
void PrintStats() {
    uint64_t total = stat_hits + stat_misses;
    double   hr    = total ? double(stat_hits) / double(total) * 100.0 : 0.0;
    std::cout << "==== Adaptive SHiP-DIP Statistics ====\n";
    std::cout << "Total refs      : " << total          << "\n";
    std::cout << "Hits            : " << stat_hits      << "\n";
    std::cout << "Misses          : " << stat_misses    << "\n";
    std::cout << "Evictions       : " << stat_evictions << "\n";
    std::cout << "Hit rate (%)    : " << hr             << "\n";
    std::cout << "SHCT threshold  : " << int(SHCT_THRESHOLD) << "\n";
}

// Print periodic (heartbeat) statistics
void PrintStats_Heartbeat() {
    PrintStats();
}