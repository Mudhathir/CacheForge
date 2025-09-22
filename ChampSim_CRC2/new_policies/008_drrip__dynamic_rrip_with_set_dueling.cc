#include <vector>
#include <cstdint>
#include <iostream>
#include "../inc/champsim_crc2.h"

#define NUM_CORE       1
#define LLC_SETS       (NUM_CORE * 2048)
#define LLC_WAYS       16

// RRIP parameters
#define RRPV_BITS      2
#define RRPV_MAX       ((1 << RRPV_BITS) - 1)   // 3 for 2-bit
#define SRRIP_INIT     (RRPV_MAX - 1)           // 2
#define BIP_RATE       32                        // 1/32 chance promotion

// PSEL (policy selector) parameters
#define PSEL_BITS      10
#define PSEL_MAX       ((1 << PSEL_BITS) - 1)
#define PSEL_INIT      (PSEL_MAX >> 1)           // midpoint
#define PSEL_THRESHOLD (PSEL_MAX >> 1)

// Set‐Dueling classification
// sample_period = 64 sets: [0..15]=SRRIP sample, [16..31]=BRRIP sample, rest=follower
enum SetType : uint8_t { FOLLOWER = 0, SRRIP_SAMPLE = 1, BRRIP_SAMPLE = 2 };

// Replacement state
static uint8_t repl_rrpv   [LLC_SETS][LLC_WAYS];
static uint8_t set_type    [LLC_SETS];
static uint32_t psel;                    // saturating policy selector
static uint32_t bip_counter;             // for BRRIP randomization

// Statistics
static uint64_t stat_hits      = 0;
static uint64_t stat_misses    = 0;
static uint64_t stat_evictions = 0;

// Initialize replacement state
void InitReplacementState() {
    // Initialize RRPVs and classify sets
    for (uint32_t s = 0; s < LLC_SETS; s++) {
        // classify set for sampling
        uint32_t m = s & 63;  // mod 64
        if      (m < 16)  set_type[s] = SRRIP_SAMPLE;
        else if (m < 32)  set_type[s] = BRRIP_SAMPLE;
        else              set_type[s] = FOLLOWER;
        // initialize lines
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            repl_rrpv[s][w] = RRPV_MAX;
        }
    }
    // Initialize PSEL and counters
    psel         = PSEL_INIT;
    bip_counter  = 0;
    stat_hits      = stat_misses = stat_evictions = 0;
}

// Victim selection using RRIP
uint32_t GetVictimInSet(
    uint32_t cpu,
    uint32_t set,
    const BLOCK *current_set,
    uint64_t PC,
    uint64_t paddr,
    uint32_t type
) {
    // find a line with RRPV == max, aging otherwise
    while (true) {
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            if (repl_rrpv[set][w] == RRPV_MAX) {
                return w;
            }
        }
        // age all lines
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            if (repl_rrpv[set][w] < RRPV_MAX) {
                repl_rrpv[set][w]++;
            }
        }
    }
    return 0; // should never reach
}

// Update replacement state on each access
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
        // Hit: promote to MRU
        stat_hits++;
        repl_rrpv[set][way] = 0;
        // Update PSEL on sample-set hits
        if (set_type[set] == SRRIP_SAMPLE) {
            if (psel < PSEL_MAX) psel++;
        } else if (set_type[set] == BRRIP_SAMPLE) {
            if (psel > 0)       psel--;
        }
    } else {
        // Miss & eviction: install new block
        stat_misses++;
        stat_evictions++;
        uint8_t policy = set_type[set];
        // Decide insertion policy
        bool use_srrip;
        if (policy == SRRIP_SAMPLE) {
            use_srrip = true;
        } else if (policy == BRRIP_SAMPLE) {
            use_srrip = false;
        } else {
            // follower: choose based on PSEL
            use_srrip = (psel >= PSEL_THRESHOLD);
        }
        // Perform insertion
        if (use_srrip) {
            // SRRIP insertion: moderate RRPV
            repl_rrpv[set][way] = SRRIP_INIT;
        } else {
            // BRRIP insertion: mostly worst RRPV, rare moderate
            bip_counter++;
            if ((bip_counter & (BIP_RATE - 1)) == 0) {
                repl_rrpv[set][way] = SRRIP_INIT;
            } else {
                repl_rrpv[set][way] = RRPV_MAX;
            }
        }
    }
}

// Print end-of-simulation statistics
void PrintStats() {
    uint64_t total = stat_hits + stat_misses;
    double   hr    = total ? double(stat_hits) / total * 100.0 : 0.0;
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