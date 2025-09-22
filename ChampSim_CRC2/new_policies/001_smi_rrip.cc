#include <vector>
#include <cstdint>
#include <iostream>
#include "../inc/champsim_crc2.h"

#define NUM_CORE    1
#define LLC_SETS    (NUM_CORE * 2048)
#define LLC_WAYS    16

// RRIP parameters
#define RRPV_BITS   2
#define MAX_RRPV    ((1 << RRPV_BITS) - 1)

// Signature-predictor parameters
#define SIG_COUNT   1024   // number of distinct PC signatures
#define CNT_MAX     3      // saturating counter maximum
#define CNT_INIT    2      // initial counter value (mid‐level)

// Replacement state
static uint8_t  rrpv      [NUM_CORE][LLC_SETS][LLC_WAYS];
static bool     valid     [NUM_CORE][LLC_SETS][LLC_WAYS];
static uint16_t sig_arr   [NUM_CORE][LLC_SETS][LLC_WAYS];
static bool     hit_flag  [NUM_CORE][LLC_SETS][LLC_WAYS];

// Signature counter table
static uint8_t pred_table[SIG_COUNT];

// Statistics
static uint64_t total_accesses = 0, hit_accesses = 0, miss_accesses = 0;

// Simple PC → signature hash
static inline int GetSignature(uint64_t PC) {
    return int(((PC >> 2) ^ (PC >> 12)) & (SIG_COUNT - 1));
}

// Initialize replacement state and predictor
void InitReplacementState() {
    // init predictor counters
    for (int i = 0; i < SIG_COUNT; i++) {
        pred_table[i] = CNT_INIT;
    }
    // init cache meta
    for (int c = 0; c < NUM_CORE; c++) {
        for (int s = 0; s < LLC_SETS; s++) {
            for (int w = 0; w < LLC_WAYS; w++) {
                valid[c][s][w]    = false;
                rrpv[c][s][w]     = MAX_RRPV;
                hit_flag[c][s][w] = false;
                sig_arr[c][s][w]  = 0;
            }
        }
    }
}

// Victim selection using SRRIP
uint32_t GetVictimInSet(
    uint32_t cpu,
    uint32_t set,
    const BLOCK *current_set,
    uint64_t PC,
    uint64_t paddr,
    uint32_t type
) {
    // 1) empty way?
    for (uint32_t w = 0; w < LLC_WAYS; w++) {
        if (!valid[cpu][set][w]) {
            return w;
        }
    }
    // 2) find RRPV == MAX_RRPV
    while (true) {
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            if (rrpv[cpu][set][w] == MAX_RRPV) {
                return w;
            }
        }
        // age all
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            if (rrpv[cpu][set][w] < MAX_RRPV) {
                rrpv[cpu][set][w]++;
            }
        }
    }
}

// Update on access
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
    total_accesses++;
    if (hit) {
        // on hit, promote to MRU
        hit_accesses++;
        rrpv[cpu][set][way]     = 0;
        hit_flag[cpu][set][way] = true;
        return;
    }
    // miss path: will insert at [set][way]
    miss_accesses++;
    // 1) update predictor for the evicted block
    if (valid[cpu][set][way]) {
        int old_sig = sig_arr[cpu][set][way];
        if (hit_flag[cpu][set][way]) {
            // block was reused → strengthen predictor
            if (pred_table[old_sig] < CNT_MAX) pred_table[old_sig]++;
        } else {
            // block not reused → weaken predictor
            if (pred_table[old_sig] > 0) pred_table[old_sig]--;
        }
    }
    // 2) install new block metadata
    int new_sig = GetSignature(PC);
    sig_arr   [cpu][set][way] = new_sig;
    valid     [cpu][set][way] = true;
    hit_flag  [cpu][set][way] = false;
    // 3) choose insertion RRPV based on predictor counter
    uint8_t cnt = pred_table[new_sig];
    if (cnt == CNT_MAX) {
        // highly reused → MRU
        rrpv[cpu][set][way] = 0;
    }
    else if (cnt == CNT_INIT) {
        // moderate reuse → mid-age
        rrpv[cpu][set][way] = MAX_RRPV - 1;
    }
    else {
        // likely streaming → distant
        rrpv[cpu][set][way] = MAX_RRPV;
    }
}

// Final statistics
void PrintStats() {
    double hr = total_accesses ? (double)hit_accesses / total_accesses * 100.0 : 0.0;
    std::cout << "==== SMI-RRIP Final Stats ====\n";
    std::cout << "Total Accesses : " << total_accesses << "\n";
    std::cout << " Hits          : " << hit_accesses  << "\n";
    std::cout << " Misses        : " << miss_accesses << "\n";
    std::cout << "Hit Rate (%)   : " << hr            << "\n";
}

// Periodic heartbeat
void PrintStats_Heartbeat() {
    double hr = total_accesses ? (double)hit_accesses / total_accesses * 100.0 : 0.0;
    std::cout << "[SMI-RRIP HB] Acc=" << total_accesses
              << " Hit=" << hit_accesses
              << " Miss=" << miss_accesses
              << " HR=" << hr << "%\n";
}