#include <vector>
#include <cstdint>
#include <iostream>
#include "../inc/champsim_crc2.h"

#define NUM_CORE            1
#define LLC_SETS            (NUM_CORE * 2048)
#define LLC_WAYS            16

// SRRIP parameters
#define RRPV_BITS           2
#define MAX_RRPV            ((1 << RRPV_BITS) - 1)

// DIP parameters
#define PSEL_BITS           10
#define PSEL_MAX            ((1 << PSEL_BITS) - 1)
#define PSEL_INIT           (PSEL_MAX / 2)
#define LEADER_DISTANCE     64
#define LRU_LEADER_ID       0
#define BIP_LEADER_ID       1
#define BIP_EPSILON         32  // 1/32 chance to insert at MRU under BIP

// Per‐block state
static uint8_t   rrpv[NUM_CORE][LLC_SETS][LLC_WAYS];
static bool      valid[NUM_CORE][LLC_SETS][LLC_WAYS];

// Global DIP state
static uint32_t  PSEL;
static uint32_t  insertion_counter;

// Statistics
static uint64_t total_accesses = 0, hit_accesses = 0, miss_accesses = 0;

// Initialize replacement state
void InitReplacementState() {
    total_accesses = hit_accesses = miss_accesses = 0;
    insertion_counter = 0;
    PSEL = PSEL_INIT;
    for (int c = 0; c < NUM_CORE; c++) {
        for (int s = 0; s < LLC_SETS; s++) {
            for (int w = 0; w < LLC_WAYS; w++) {
                valid[c][s][w] = false;
                rrpv[c][s][w]  = MAX_RRPV;
            }
        }
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
        // On hit: always promote to MRU
        hit_accesses++;
        rrpv[cpu][set][way] = 0;
        return;
    }
    // On miss
    miss_accesses++;
    bool is_leader_lru = (set % LEADER_DISTANCE == LRU_LEADER_ID);
    bool is_leader_bip = (set % LEADER_DISTANCE == BIP_LEADER_ID);

    // 1) Update PSEL based on leader misses
    if (is_leader_lru) {
        if (PSEL > 0) PSEL--;
    } else if (is_leader_bip) {
        if (PSEL < PSEL_MAX) PSEL++;
    }

    // 2) Choose insertion policy
    bool use_lru;
    if (is_leader_lru)        use_lru = true;
    else if (is_leader_bip)   use_lru = false;
    else                      use_lru = (PSEL > PSEL_INIT);

    // 3) Install new block
    valid[cpu][set][way] = true;
    if (use_lru) {
        // LRU insertion: MRU
        rrpv[cpu][set][way] = 0;
    } else {
        // BIP insertion: usually far eviction, occasionally MRU
        if ((insertion_counter++ & (BIP_EPSILON - 1)) == 0) {
            rrpv[cpu][set][way] = 0;
        } else {
            rrpv[cpu][set][way] = MAX_RRPV;
        }
    }
}

// Print end‐of‐simulation statistics
void PrintStats() {
    double hr = total_accesses
                ? (double)hit_accesses / total_accesses * 100.0
                : 0.0;
    std::cout << "==== SetDuelDIP Final Stats ====\n";
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
    std::cout << "[SetDuelDIP HB] Acc="
              << total_accesses
              << " Hit="   << hit_accesses
              << " Miss="  << miss_accesses
              << " HR="    << hr << "%\n";
}