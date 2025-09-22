#include <vector>
#include <cstdint>
#include <iostream>
#include <algorithm>
#include "../inc/champsim_crc2.h"

#define NUM_CORE     1
#define LLC_SETS     (NUM_CORE * 2048)
#define LLC_WAYS     16

// RRIP parameters
#define RRPV_BITS            2
#define MAX_RRPV             ((1 << RRPV_BITS) - 1)

// SHiP parameters
#define SHT_SIZE             4096
#define SHT_COUNTER_BITS     3
#define SHT_MAX              ((1 << SHT_COUNTER_BITS) - 1)
#define SHT_THRESHOLD        (SHT_MAX >> 1)

// Global replacement state
static uint8_t  rrpv        [NUM_CORE][LLC_SETS][LLC_WAYS];
static bool     valid       [NUM_CORE][LLC_SETS][LLC_WAYS];
static bool     re_ref      [NUM_CORE][LLC_SETS][LLC_WAYS];
static uint16_t block_sig   [NUM_CORE][LLC_SETS][LLC_WAYS];
static uint8_t  sht         [SHT_SIZE];
static uint64_t total_accesses = 0, hit_accesses = 0, miss_accesses = 0;

// Hash function to map PC to SHT index
static inline uint32_t
SignatureOf(uint64_t PC)
{
    // Mix bits of PC and fold into SHT_SIZE
    return (uint32_t)((PC ^ (PC >> 3)) & (SHT_SIZE - 1));
}

// Initialize replacement state
void InitReplacementState() {
    total_accesses = hit_accesses = miss_accesses = 0;
    // Initialize cache metadata
    for (int c = 0; c < NUM_CORE; c++) {
        for (int s = 0; s < LLC_SETS; s++) {
            for (int w = 0; w < LLC_WAYS; w++) {
                valid[c][s][w]     = false;
                re_ref[c][s][w]    = false;
                rrpv[c][s][w]      = MAX_RRPV;
                block_sig[c][s][w] = 0;
            }
        }
    }
    // Initialize SHiP counters to weakly‐reuse
    for (int i = 0; i < SHT_SIZE; i++) {
        sht[i] = SHT_THRESHOLD;
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
    // 1) Look for an invalid way
    for (uint32_t w = 0; w < LLC_WAYS; w++) {
        if (!valid[cpu][set][w]) {
            return w;
        }
    }
    // 2) Find a line with RRPV == MAX_RRPV, aging on the fly if needed
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
    // unreachable
    return 0;
}

// Update replacement state on access
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
        // On hit: promote and mark reuse
        hit_accesses++;
        rrpv[cpu][set][way]   = 0;
        valid[cpu][set][way]  = true;
        re_ref[cpu][set][way] = true;
        return;
    }
    // Miss path
    miss_accesses++;
    // 1) Update SHiP on eviction of the block in 'way'
    if (valid[cpu][set][way]) {
        uint16_t old_sig = block_sig[cpu][set][way];
        bool     reused  = re_ref[cpu][set][way];
        if (reused) {
            // Increment toward strongly‐reuse
            sht[old_sig] = std::min<uint8_t>(sht[old_sig] + 1, SHT_MAX);
        } else {
            // Decrement toward strongly‐non‐reuse
            sht[old_sig] = std::max<int>(sht[old_sig] - 1, 0);
        }
    }
    // 2) Install new block
    valid[cpu][set][way]  = true;
    re_ref[cpu][set][way] = false;
    uint32_t sig = SignatureOf(PC);
    block_sig[cpu][set][way] = sig;
    // 3) Choose insertion RRPV by SHiP prediction
    if (sht[sig] >= SHT_THRESHOLD) {
        // Predict reuse: near insertion
        rrpv[cpu][set][way] = MAX_RRPV - 1;
    } else {
        // Predict non‐reuse: far insertion
        rrpv[cpu][set][way] = MAX_RRPV;
    }
}

// Print end‐of‐simulation statistics
void PrintStats() {
    double hr = total_accesses ? (double)hit_accesses / total_accesses * 100.0 : 0.0;
    std::cout << "==== SHiP-RRIP Final Stats ====\n";
    std::cout << "Total Accesses : " << total_accesses << "\n";
    std::cout << " Hits          : " << hit_accesses  << "\n";
    std::cout << " Misses        : " << miss_accesses << "\n";
    std::cout << "Hit Rate (%)   : " << hr            << "\n";
}

// Print periodic (heartbeat) statistics
void PrintStats_Heartbeat() {
    double hr = total_accesses ? (double)hit_accesses / total_accesses * 100.0 : 0.0;
    std::cout << "[SHiP-RRIP HB] Acc=" << total_accesses
              << " Hit=" << hit_accesses
              << " Miss=" << miss_accesses
              << " HR=" << hr << "%\n";
}