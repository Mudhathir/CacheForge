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

// PC Counter Table parameters
#define PCCTR_BITS   5
#define PCCT_SIZE    (1 << 12)           // 4096 entries
#define PCCTR_MAX    ((1 << PCCTR_BITS) - 1)
#define PCCTR_INIT   (PCCTR_MAX / 2)
#define PC_MASK      (PCCT_SIZE - 1)
#define LOW_THLD     (PCCTR_MAX / 3)              // ~10
#define HIGH_THLD    ((2 * PCCTR_MAX) / 3)        // ~20

// Per-block metadata
static uint8_t  rrpv        [NUM_CORE][LLC_SETS][LLC_WAYS];
static bool     valid       [NUM_CORE][LLC_SETS][LLC_WAYS];
static bool     M_flag      [NUM_CORE][LLC_SETS][LLC_WAYS];
static uint16_t pc_index_arr[NUM_CORE][LLC_SETS][LLC_WAYS];

// PC Counter Table
static uint8_t PCCT[PCCT_SIZE];

// Statistics
static uint64_t total_accesses, hit_accesses, miss_accesses;

// Initialize replacement state
void InitReplacementState() {
    total_accesses = hit_accesses = miss_accesses = 0;
    // Clear cache metadata
    for (int c = 0; c < NUM_CORE; c++) {
        for (int s = 0; s < LLC_SETS; s++) {
            for (int w = 0; w < LLC_WAYS; w++) {
                valid[c][s][w]         = false;
                rrpv[c][s][w]          = MAX_RRPV;
                M_flag[c][s][w]        = false;
                pc_index_arr[c][s][w]  = 0;
            }
        }
    }
    // Initialize PC counters
    for (int i = 0; i < PCCT_SIZE; i++) {
        PCCT[i] = PCCTR_INIT;
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
    // 1) Cold‐miss: empty way
    for (uint32_t w = 0; w < LLC_WAYS; w++) {
        if (!valid[cpu][set][w]) {
            return w;
        }
    }
    // 2) SRRIP‐style victim search
    while (true) {
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            if (rrpv[cpu][set][w] == MAX_RRPV) {
                return w;
            }
        }
        // Increment all RRPV < MAX
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            if (rrpv[cpu][set][w] < MAX_RRPV) {
                rrpv[cpu][set][w]++;
            }
        }
    }
    // unreachable
    return 0;
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
        rrpv[cpu][set][way]   = 0;
        return;
    }
    // On miss
    miss_accesses++;

    // 1) Eviction: update PCCT based on reuse flag of evicted block
    if (valid[cpu][set][way]) {
        uint16_t old_idx = pc_index_arr[cpu][set][way];
        if (M_flag[cpu][set][way]) {
            if (PCCT[old_idx] < PCCTR_MAX)
                PCCT[old_idx]++;
        } else {
            if (PCCT[old_idx] > 0)
                PCCT[old_idx]--;
        }
    }

    // 2) Install new block
    valid[cpu][set][way]      = true;
    M_flag[cpu][set][way]     = false;
    // Compute PC‐based index
    uint16_t idx = (uint16_t)(PC & PC_MASK);
    pc_index_arr[cpu][set][way] = idx;

    // 3) Three‐level insertion based on PCCT counter
    uint8_t ctr = PCCT[idx];
    if (ctr > HIGH_THLD) {
        // heavy reuse → MRU
        rrpv[cpu][set][way] = 0;
    } else if (ctr > LOW_THLD) {
        // moderate reuse → mid‐priority
        rrpv[cpu][set][way] = 1;
    } else {
        // streaming/no‐reuse → near eviction
        rrpv[cpu][set][way] = MAX_RRPV;
    }
}

// Print end‐of‐simulation statistics
void PrintStats() {
    double hr = total_accesses
                ? (double)hit_accesses / total_accesses * 100.0
                : 0.0;
    std::cout << "==== Tri-Insert-RRIP Final Stats ====\n";
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
    std::cout << "[Tri-Insert-RRIP HB] Acc="
              << total_accesses
              << " Hit="   << hit_accesses
              << " Miss="  << miss_accesses
              << " HR="    << hr << "%\n";
}