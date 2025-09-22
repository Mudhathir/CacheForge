#include <vector>
#include <cstdint>
#include <iostream>
#include "../inc/champsim_crc2.h"

#define NUM_CORE    1
#define LLC_SETS    (NUM_CORE * 2048)
#define LLC_WAYS    16

// RRIP parameters
#define RRPV_BITS            2
#define MAX_RRPV             ((1 << RRPV_BITS) - 1)

// DRRIP dueling parameters
#define PSEL_BITS            10
#define PSEL_MAX             ((1 << PSEL_BITS) - 1)
#define PSEL_INIT            (PSEL_MAX >> 1)
#define NUM_LEADERS          32
#define SAMPLE_PERIOD        (LLC_SETS / NUM_LEADERS)
#define SRRIP_SAMPLE_VALUE   0
#define BRRIP_SAMPLE_VALUE   (SAMPLE_PERIOD / 2)

// Global replacement state
static uint8_t  rrpv      [NUM_CORE][LLC_SETS][LLC_WAYS];
static bool     valid     [NUM_CORE][LLC_SETS][LLC_WAYS];
static uint16_t psel;           // policy selection counter
static uint64_t total_accesses = 0, hit_accesses = 0, miss_accesses = 0;

// Initialize replacement state
void InitReplacementState() {
    psel = PSEL_INIT;
    total_accesses = hit_accesses = miss_accesses = 0;
    for (int c = 0; c < NUM_CORE; c++) {
        for (int s = 0; s < LLC_SETS; s++) {
            for (int w = 0; w < LLC_WAYS; w++) {
                valid[c][s][w] = false;
                rrpv[c][s][w]  = MAX_RRPV;
            }
        }
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
    // 1) empty way?
    for (uint32_t w = 0; w < LLC_WAYS; w++) {
        if (!valid[cpu][set][w]) {
            return w;
        }
    }
    // 2) find RRPV == MAX_RRPV, aging on the fly
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
        hit_accesses++;
        // Promote on hit
        rrpv[cpu][set][way] = 0;
        valid[cpu][set][way] = true;
        return;
    }
    // Miss path: we will install at [set][way]
    miss_accesses++;
    // Determine if this is a leader set for SRRIP or BRRIP
    int m = set % SAMPLE_PERIOD;
    bool is_sr_leader = (m == SRRIP_SAMPLE_VALUE);
    bool is_br_leader = (m == BRRIP_SAMPLE_VALUE);
    // Update PSEL on misses in leader sets
    if (is_sr_leader) {
        if (psel > 0) psel--;
    } else if (is_br_leader) {
        if (psel < PSEL_MAX) psel++;
    }
    // Install new block
    valid[cpu][set][way] = true;
    bool use_srrip;
    if (is_sr_leader) {
        use_srrip = true;
    } else if (is_br_leader) {
        use_srrip = false;
    } else {
        // follower sets follow PSEL
        use_srrip = (psel >= PSEL_INIT);
    }
    if (use_srrip) {
        // SRRIP insertion: near-reuse
        rrpv[cpu][set][way] = MAX_RRPV - 1;
    } else {
        // BRRIP insertion: mostly distant, rare near
        if ((PC & 0x1F) == 0) {
            rrpv[cpu][set][way] = MAX_RRPV - 1;
        } else {
            rrpv[cpu][set][way] = MAX_RRPV;
        }
    }
}

// Print end-of-simulation statistics
void PrintStats() {
    double hr = total_accesses ? (double)hit_accesses / total_accesses * 100.0 : 0.0;
    std::cout << "==== DRRIP Final Stats ====\n";
    std::cout << "Total Accesses : " << total_accesses << "\n";
    std::cout << " Hits          : " << hit_accesses  << "\n";
    std::cout << " Misses        : " << miss_accesses << "\n";
    std::cout << "Hit Rate (%)   : " << hr            << "\n";
}

// Print periodic (heartbeat) statistics
void PrintStats_Heartbeat() {
    double hr = total_accesses ? (double)hit_accesses / total_accesses * 100.0 : 0.0;
    std::cout << "[DRRIP HB] Acc=" << total_accesses
              << " Hit=" << hit_accesses
              << " Miss=" << miss_accesses
              << " HR=" << hr << "%\n";
}