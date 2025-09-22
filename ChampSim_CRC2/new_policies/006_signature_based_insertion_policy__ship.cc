#include <vector>
#include <cstdint>
#include <iostream>
#include "../inc/champsim_crc2.h"

#define NUM_CORE 1
#define LLC_SETS (NUM_CORE * 2048)
#define LLC_WAYS 16

// RRIP parameters
#define RRPV_BITS 2
#define MAX_RRPV    ((1 << RRPV_BITS) - 1)

// SHiP predictor parameters
#define SHIP_TABLE_BITS 14
#define SHIP_TABLE_SIZE (1 << SHIP_TABLE_BITS)
#define SHIP_CTR_BITS   2
#define SHIP_CTR_MAX    ((1 << SHIP_CTR_BITS) - 1)
#define SHIP_CTR_INIT   (SHIP_CTR_MAX / 2)

// Per‐line RRIP state
static uint8_t  rrpv     [NUM_CORE][LLC_SETS][LLC_WAYS];
static bool     valid    [NUM_CORE][LLC_SETS][LLC_WAYS];
// Per‐line signature and reuse flag
static uint16_t sig      [NUM_CORE][LLC_SETS][LLC_WAYS];
static bool     reused   [NUM_CORE][LLC_SETS][LLC_WAYS];
// Global SHiP table
static uint8_t  ship_table[SHIP_TABLE_SIZE];

// Statistics
static uint64_t total_accesses = 0, hit_accesses = 0, miss_accesses = 0;

// Initialize replacement state
void InitReplacementState() {
    total_accesses = hit_accesses = miss_accesses = 0;
    // Init per‐line state
    for (int c = 0; c < NUM_CORE; c++) {
        for (int s = 0; s < LLC_SETS; s++) {
            for (int w = 0; w < LLC_WAYS; w++) {
                valid[c][s][w]   = false;
                rrpv[c][s][w]    = MAX_RRPV;
                reused[c][s][w]  = false;
                sig[c][s][w]     = 0;
            }
        }
    }
    // Init SHiP predictor to weakly neutral
    for (int i = 0; i < SHIP_TABLE_SIZE; i++) {
        ship_table[i] = SHIP_CTR_INIT;
    }
}

// Find victim in the set (SRRIP victim selection)
uint32_t GetVictimInSet(
    uint32_t cpu,
    uint32_t set,
    const BLOCK *current_set,
    uint64_t PC,
    uint64_t paddr,
    uint32_t type
) {
    // 1) If any invalid, choose it
    for (uint32_t w = 0; w < LLC_WAYS; w++) {
        if (!valid[cpu][set][w]) {
            return w;
        }
    }
    // 2) Otherwise find RRPV == MAX_RRPV, aging others until found
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
    total_accesses++;
    if (hit) {
        // On hit: promote to MRU and mark reused
        hit_accesses++;
        rrpv[cpu][set][way]  = 0;
        reused[cpu][set][way] = true;
        return;
    }
    // Miss path
    miss_accesses++;
    // 1) Update SHiP predictor using the evicted block's signature and reuse flag
    if (valid[cpu][set][way]) {
        uint16_t old_sig = sig[cpu][set][way];
        if (reused[cpu][set][way]) {
            // rewarded: strongly reuse → increment
            if (ship_table[old_sig] < SHIP_CTR_MAX)
                ship_table[old_sig]++;
        } else {
            // never reused → decrement
            if (ship_table[old_sig] > 0)
                ship_table[old_sig]--;
        }
    }
    // 2) Insert the new block
    valid[cpu][set][way]    = true;
    // Compute signature from PC (simple hash)
    uint16_t new_sig = (uint16_t)((PC >> 2) & (SHIP_TABLE_SIZE - 1));
    sig[cpu][set][way]      = new_sig;
    reused[cpu][set][way]   = false;
    // 3) Choose insertion priority based on predictor
    if (ship_table[new_sig] >= (SHIP_CTR_MAX + 1) / 2) {
        // predicted reusable: SRRIP insert (mid‐priority)
        rrpv[cpu][set][way] = MAX_RRPV - 1;
    } else {
        // predicted dead/streaming: BIP insert (far)
        rrpv[cpu][set][way] = MAX_RRPV;
    }
}

// Print end-of-simulation statistics
void PrintStats() {
    double hr = total_accesses
              ? (double)hit_accesses / total_accesses * 100.0
              : 0.0;
    std::cout << "==== SHiP Final Stats ====\n";
    std::cout << "Total Accesses : " << total_accesses << "\n";
    std::cout << " Hits          : " << hit_accesses  << "\n";
    std::cout << " Misses        : " << miss_accesses << "\n";
    std::cout << "Hit Rate (%)   : " << hr            << "\n";
}

// Print periodic (heartbeat) statistics
void PrintStats_Heartbeat() {
    double hr = total_accesses
              ? (double)hit_accesses / total_accesses * 100.0
              : 0.0;
    std::cout << "[SHiP HB] Acc=" << total_accesses
              << " Hit="  << hit_accesses
              << " Miss=" << miss_accesses
              << " HR="   << hr
              << "%\n";
}