#include <vector>
#include <cstdint>
#include <iostream>
#include <algorithm>
#include "../inc/champsim_crc2.h"

#define NUM_CORE 1
#define LLC_SETS (NUM_CORE * 2048)
#define LLC_WAYS 16

// Tunable parameters
static const uint32_t SAMPLE_PERIOD      = 32;    // Every 32nd set is sampled
static const uint32_t EPOCH_MISS_THRES   = 10000; // Epoch ends after this many sample misses
static const uint32_t PC_CTR_SIZE        = 1024;  // PC‐counter table entries
static const uint8_t  PC_CTR_MAX         = 7;     // 3‐bit saturating
static const uint8_t  PC_USE_THRESH      = 4;     // >= threshold ⇒ high‐priority insertion

// Replacement policies
enum PolicyType { POL_LRU = 0, POL_SRRIP = 1, POL_PFSRRIP = 2 };
static const int NUM_POLICY = 3;

// Per‐way metadata
struct LineRepl {
    uint8_t  rrpv;     // 0 (hot) .. 3 (cold)
    uint64_t last_ts;  // for LRU
    bool     valid;
};

// Global state
static LineRepl repl[NUM_CORE][LLC_SETS][LLC_WAYS];
static bool      is_sample[LLC_SETS];
static PolicyType sample_pol[LLC_SETS];
static uint64_t  sample_misses[NUM_POLICY];
static uint64_t  total_sample_misses;
static PolicyType current_winner;
static uint64_t  global_ts;
static uint64_t  epoch_count;
// PC‐based counters for PFSRRIP
static uint8_t   pc_ctr[PC_CTR_SIZE];

// Helpers
inline uint32_t PCIndex(uint64_t PC) {
    return (PC >> 4) & (PC_CTR_SIZE - 1);
}
inline void SaturatingInc(uint8_t &c) {
    if (c < PC_CTR_MAX) c++;
}
inline void SaturatingDec(uint8_t &c) {
    if (c > 0) c--;
}

// Initialize replacement state
void InitReplacementState() {
    global_ts = 0;
    epoch_count = 0;
    total_sample_misses = 0;
    current_winner = POL_SRRIP; // warm‐start with SRRIP

    // Initialize per‐way metadata
    for (uint32_t core = 0; core < NUM_CORE; core++) {
        for (uint32_t s = 0; s < LLC_SETS; s++) {
            for (uint32_t w = 0; w < LLC_WAYS; w++) {
                repl[core][s][w].rrpv     = 3; // coldest
                repl[core][s][w].last_ts  = 0;
                repl[core][s][w].valid    = false;
            }
        }
    }
    // Setup sampling
    for (uint32_t s = 0; s < LLC_SETS; s++) {
        uint32_t m = s % SAMPLE_PERIOD;
        if (m < NUM_POLICY) {
            is_sample[s]     = true;
            sample_pol[s]    = static_cast<PolicyType>(m);
        } else {
            is_sample[s]     = false;
        }
    }
    // Zero counters
    for (int i = 0; i < NUM_POLICY; i++) {
        sample_misses[i] = 0;
    }
    for (uint32_t i = 0; i < PC_CTR_SIZE; i++) {
        pc_ctr[i] = PC_CTR_MAX / 2;
    }
}

// Choose a victim based on the active policy
uint32_t GetVictimInSet(
    uint32_t cpu,
    uint32_t set,
    const BLOCK *current_set,
    uint64_t PC,
    uint64_t paddr,
    uint32_t type
) {
    PolicyType policy = is_sample[set] ? sample_pol[set] : current_winner;
    // 1) LRU victim
    if (policy == POL_LRU) {
        uint64_t oldest_ts = UINT64_MAX;
        uint32_t  victim = 0;
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            if (!current_set[w].valid) return w;
            if (repl[cpu][set][w].last_ts < oldest_ts) {
                oldest_ts = repl[cpu][set][w].last_ts;
                victim    = w;
            }
        }
        return victim;
    }
    // 2) SRRIP‐based (incl. PFSRRIP)
    // Find a line with rrpv == MAX (3). If none, age all lines.
    while (true) {
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            if (!current_set[w].valid) return w;
            if (repl[cpu][set][w].rrpv == 3) return w;
        }
        // Age
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            repl[cpu][set][w].rrpv = std::min<uint8_t>(3, repl[cpu][set][w].rrpv + 1);
        }
    }
}

// Update replacement state on access/hit/miss
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
    global_ts++;

    PolicyType policy = is_sample[set] ? sample_pol[set] : current_winner;

    // On hit: refresh metadata
    if (hit) {
        // LRU
        repl[cpu][set][way].last_ts = global_ts;
        // SRRIP/PFSRRIP: promote to hot
        repl[cpu][set][way].rrpv = 0;
        // PC‐filter learning
        if (policy == POL_PFSRRIP) {
            uint32_t idx = PCIndex(PC);
            SaturatingInc(pc_ctr[idx]);
        }
        return;
    }

    // On miss: count sample misses
    if (is_sample[set]) {
        sample_misses[policy]++;
        total_sample_misses++;
    }

    // Epoch end?
    if (total_sample_misses >= EPOCH_MISS_THRES) {
        // Choose winner = min misses
        uint32_t best = 0;
        uint64_t best_count = sample_misses[0];
        for (uint32_t i = 1; i < NUM_POLICY; i++) {
            if (sample_misses[i] < best_count) {
                best = i;
                best_count = sample_misses[i];
            }
        }
        current_winner = static_cast<PolicyType>(best);
        // Reset epoch
        total_sample_misses = 0;
        for (int i = 0; i < NUM_POLICY; i++) sample_misses[i] = 0;
        epoch_count++;
    }

    // Insertion / victim slot metadata
    // LRU
    repl[cpu][set][way].last_ts = global_ts;
    repl[cpu][set][way].valid   = true;

    // SRRIP vs. PFSRRIP decide RRPV
    if (policy == POL_SRRIP) {
        repl[cpu][set][way].rrpv = 2; // long re‐reference interval
    }
    else if (policy == POL_PFSRRIP) {
        uint32_t idx = PCIndex(PC);
        if (pc_ctr[idx] >= PC_USE_THRESH) {
            repl[cpu][set][way].rrpv = 0; // hot insertion
        } else {
            repl[cpu][set][way].rrpv = 2; // cold insertion
        }
        // Decay PC counter on miss
        SaturatingDec(pc_ctr[idx]);
    }
    else { // POL_LRU
        // LRU does not use RRPV; but we still zero it
        repl[cpu][set][way].rrpv = 0;
    }
}

// Print end-of-simulation statistics
void PrintStats() {
    std::cout << "AMPT Epochs: "       << epoch_count
              << " | Final Winner: "   << current_winner
              << std::endl;
}

// Print periodic (heartbeat) statistics
void PrintStats_Heartbeat() {
    std::cout << "[Heartbeat] Epochs=" << epoch_count
              << " Winner="            << current_winner
              << std::endl;
}