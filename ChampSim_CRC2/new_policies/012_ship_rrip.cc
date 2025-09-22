#include <vector>
#include <cstdint>
#include <iostream>
#include "../inc/champsim_crc2.h"

#define NUM_CORE     1
#define LLC_SETS     (NUM_CORE * 2048)
#define LLC_WAYS     16

// SRRIP parameters
#define RRPV_BITS    2
#define MAX_RRPV     ((1 << RRPV_BITS) - 1)

// SHiP parameters
#define SHCT_BITS    3
#define SHCT_SIZE    (1 << 10)   // 1024 entries
#define SHCT_MAX     ((1 << SHCT_BITS) - 1)
#define SHCT_INIT    (SHCT_MAX / 2)
#define SIG_MASK     (SHCT_SIZE - 1)

// Per‐block metadata
static uint8_t  rrpv        [NUM_CORE][LLC_SETS][LLC_WAYS];
static bool     valid       [NUM_CORE][LLC_SETS][LLC_WAYS];
static bool     M_flag      [NUM_CORE][LLC_SETS][LLC_WAYS];
static uint16_t signature_arr[NUM_CORE][LLC_SETS][LLC_WAYS];

// Signature History Counter Table
static uint8_t SHCT[SHCT_SIZE];

// Statistics
static uint64_t total_accesses, hit_accesses, miss_accesses;

// Initialize replacement state
void InitReplacementState() {
    total_accesses = hit_accesses = miss_accesses = 0;
    for (int c = 0; c < NUM_CORE; c++) {
        for (int s = 0; s < LLC_SETS; s++) {
            for (int w = 0; w < LLC_WAYS; w++) {
                valid[c][s][w]         = false;
                rrpv[c][s][w]          = MAX_RRPV;
                M_flag[c][s][w]        = false;
                signature_arr[c][s][w] = 0;
            }
        }
    }
    for (int i = 0; i < SHCT_SIZE; i++) {
        SHCT[i] = SHCT_INIT;
    }
}

// Find victim in the set
uint32_t GetVictimInSet(
    uint32_t cpu,
    uint32_t set,
    const BLOCK *current_set,
    uint64_t PC,
    uint64_t paddr,
    uint32_t type
) {
    // 1) Cold‐miss preference
    for (uint32_t w = 0; w < LLC_WAYS; w++) {
        if (!valid[cpu][set][w]) {
            return w;
        }
    }
    // 2) SRRIP‐style eviction
    while (true) {
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            if (rrpv[cpu][set][w] == MAX_RRPV) {
                return w;
            }
        }
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            if (rrpv[cpu][set][w] < MAX_RRPV) {
                rrpv[cpu][set][w]++;
            }
        }
    }
    return 0; // unreachable
}

// Update replacement state
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
        // On hit: promote to MRU and mark reuse
        hit_accesses++;
        M_flag[cpu][set][way] = true;
        rrpv[cpu][set][way]    = 0;
        return;
    }
    // On miss
    miss_accesses++;

    // 1) On eviction, update SHCT for the evicted block
    if (valid[cpu][set][way]) {
        uint16_t old_sig = signature_arr[cpu][set][way];
        if (M_flag[cpu][set][way]) {
            if (SHCT[old_sig] < SHCT_MAX) SHCT[old_sig]++;
        } else {
            if (SHCT[old_sig] > 0)      SHCT[old_sig]--;
        }
    }

    // 2) Install new block
    valid[cpu][set][way]   = true;
    M_flag[cpu][set][way]  = false;
    // Compute a PC+address‐based signature
    uint16_t sig = (uint16_t)((PC ^ (paddr >> 6)) & SIG_MASK);
    signature_arr[cpu][set][way] = sig;
    // 3) Insertion decision based on SHCT
    if (SHCT[sig] > 0) {
        // Reuse‐likely: MRU insertion
        rrpv[cpu][set][way] = 0;
    } else {
        // Streaming or no‐reuse: near eviction
        rrpv[cpu][set][way] = MAX_RRPV;
    }
}

// Print end‐of‐simulation statistics
void PrintStats() {
    double hr = total_accesses
                ? (double)hit_accesses / total_accesses * 100.0
                : 0.0;
    std::cout << "==== SHiP-RRIP Final Stats ====\n";
    std::cout << "Total Accesses : " << total_accesses << "\n";
    std::cout << " Hits          : " << hit_accesses   << "\n";
    std::cout << " Misses        : " << miss_accesses  << "\n";
    std::cout << "Hit Rate (%)   : " << hr             << "\n";
}

// Print periodic (heartbeat) statistics
void PrintStats_Heartbeat() {
    double hr = total_accesses
                ? (double)hit_accesses / total_accesses * 100.0
                : 0.0;
    std::cout << "[SHiP-RRIP HB] Acc="
              << total_accesses
              << " Hit="   << hit_accesses
              << " Miss="  << miss_accesses
              << " HR="    << hr << "%\n";
}