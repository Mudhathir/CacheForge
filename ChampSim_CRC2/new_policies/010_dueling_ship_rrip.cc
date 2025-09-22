#include <vector>
#include <cstdint>
#include <iostream>
#include "../inc/champsim_crc2.h"

#define NUM_CORE             1
#define LLC_SETS             (NUM_CORE * 2048)
#define LLC_WAYS             16

// RRIP parameters
#define RRPV_BITS            2
#define RRPV_MAX             ((1 << RRPV_BITS) - 1)   // 3
#define SRRIP_INIT           (RRPV_MAX - 1)           // 2

// SHiP Signature History Counter Table (SHCT)
#define SHCT_BITS            15
#define SHCT_SIZE            (1 << SHCT_BITS)         // 32K
#define SHCT_CTR_MAX         7
#define SHCT_CTR_INIT        (SHCT_CTR_MAX >> 1)      // 3
#define SHCT_THRESHOLD       (SHCT_CTR_MAX >> 1)      // 3

// Set‐dueling parameters
#define DUEL_SETS            64                       // power of two
#define PSEL_BITS            10
#define PSEL_MAX             ((1 << PSEL_BITS) - 1)   // 1023
#define PSEL_INIT            (PSEL_MAX >> 1)          // 511
#define SHIP_LEADER_PATTERN  0
#define SRRIP_LEADER_PATTERN 1

// Replacement state
static uint8_t   repl_rrpv       [LLC_SETS][LLC_WAYS];
static uint16_t  line_signature  [LLC_SETS][LLC_WAYS];
static uint8_t   line_referenced [LLC_SETS][LLC_WAYS];
static uint8_t   SHCT             [SHCT_SIZE];
static uint16_t  PSEL;  // set‐dueling preference counter

// Statistics
static uint64_t stat_hits      = 0;
static uint64_t stat_misses    = 0;
static uint64_t stat_evictions = 0;

// Compute a 15‐bit PC signature
static inline uint16_t ComputeSignature(uint64_t PC) {
    return (uint16_t)((PC ^ (PC >> SHCT_BITS)) & (SHCT_SIZE - 1));
}

void InitReplacementState() {
    // initialize RRIP state
    for (uint32_t s = 0; s < LLC_SETS; s++) {
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            repl_rrpv[s][w]       = RRPV_MAX;
            line_signature[s][w]  = 0;
            line_referenced[s][w] = 0;
        }
    }
    // initialize SHCT
    for (uint32_t i = 0; i < SHCT_SIZE; i++) {
        SHCT[i] = SHCT_CTR_INIT;
    }
    // initialize PSEL for set dueling
    PSEL = PSEL_INIT;

    stat_hits = stat_misses = stat_evictions = 0;
}

uint32_t GetVictimInSet(
    uint32_t cpu,
    uint32_t set,
    const BLOCK *current_set,
    uint64_t PC,
    uint64_t paddr,
    uint32_t type
) {
    // standard RRIP victim search
    while (true) {
        // look for RRPV == max
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            if (repl_rrpv[set][w] == RRPV_MAX) {
                return w;
            }
        }
        // age all entries
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            if (repl_rrpv[set][w] < RRPV_MAX) {
                repl_rrpv[set][w]++;
            }
        }
    }
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
    if (hit) {
        // On hit: promote to MRU, update SHCT
        stat_hits++;
        repl_rrpv[set][way]       = 0;
        uint16_t sig              = ComputeSignature(PC);
        if (SHCT[sig] < SHCT_CTR_MAX) {
            SHCT[sig]++;
        }
        line_referenced[set][way] = 1;
    } else {
        // On miss & eviction
        stat_misses++;
        stat_evictions++;

        // Identify leader sets
        bool is_ship_leader  = ((set & (DUEL_SETS - 1)) == SHIP_LEADER_PATTERN);
        bool is_srrip_leader = ((set & (DUEL_SETS - 1)) == SRRIP_LEADER_PATTERN);

        // Update PSEL based on which leader saw a miss
        if (is_ship_leader) {
            if (PSEL > 0) PSEL--;
        } else if (is_srrip_leader) {
            if (PSEL < PSEL_MAX) PSEL++;
        }

        // Feedback to SHCT for the evicted block
        uint16_t old_sig = line_signature[set][way];
        if (!line_referenced[set][way]) {
            // never re-referenced => decrease confidence
            if (SHCT[old_sig] > 0) {
                SHCT[old_sig]--;
            }
        }

        // Prepare new block metadata
        uint16_t new_sig = ComputeSignature(PC);
        line_signature[set][way]  = new_sig;
        line_referenced[set][way] = 0;

        // Choose insertion policy:
        //   leader sets always use their designated policy;
        //   followers use SHiP if PSEL > midpoint, else SRRIP.
        bool use_ship;
        if (is_ship_leader) {
            use_ship = true;
        } else if (is_srrip_leader) {
            use_ship = false;
        } else {
            use_ship = (PSEL > (PSEL_MAX >> 1));
        }

        if (use_ship) {
            // SHiP‐style insertion
            if (SHCT[new_sig] > SHCT_THRESHOLD) {
                // likely high‐reuse
                repl_rrpv[set][way] = SRRIP_INIT;
            } else {
                // likely low‐reuse
                repl_rrpv[set][way] = RRPV_MAX;
            }
        } else {
            // pure SRRIP insertion
            repl_rrpv[set][way] = SRRIP_INIT;
        }
    }
}

void PrintStats() {
    uint64_t total = stat_hits + stat_misses;
    double   hr    = total ? double(stat_hits) / total * 100.0 : 0.0;
    std::cout << "==== Dueling SHiP-RRIP Stats ====\n";
    std::cout << "Total refs   : " << total          << "\n";
    std::cout << "Hits         : " << stat_hits      << "\n";
    std::cout << "Misses       : " << stat_misses    << "\n";
    std::cout << "Evictions    : " << stat_evictions << "\n";
    std::cout << "Hit rate (%) : " << hr             << "\n";
}

void PrintStats_Heartbeat() {
    PrintStats();
}