#include <vector>
#include <cstdint>
#include <iostream>
#include "../inc/champsim_crc2.h"

#define NUM_CORE 1
#define LLC_SETS (NUM_CORE * 2048)
#define LLC_WAYS 16

// RRIP parameters
#define RRPV_BITS 2
#define MAX_RRPV ((1 << RRPV_BITS) - 1)

// DIP parameters
#define PSEL_BITS   10
#define PSEL_MAX    ((1 << PSEL_BITS) - 1)
#define BIP_PROB    32    // one near insertion per 32

enum SetType { LEADER_BIP = 0, LEADER_SRRIP = 1, FOLLOWER = 2 };

// Per‐line state
static uint8_t  rrpv     [NUM_CORE][LLC_SETS][LLC_WAYS];
static bool     valid    [NUM_CORE][LLC_SETS][LLC_WAYS];

// Set dueling and policy counters
static uint8_t  set_type [LLC_SETS];
static uint16_t PSEL;            // saturating counter
static uint32_t bip_counter;     // for BIP randomness

// Statistics
static uint64_t total_accesses = 0, hit_accesses = 0, miss_accesses = 0;

// Initialize replacement state
void InitReplacementState() {
    total_accesses = hit_accesses = miss_accesses = 0;
    PSEL          = PSEL_MAX / 2;
    bip_counter   = 0;
    for (int s = 0; s < LLC_SETS; s++) {
        // Assign leader sets: one every 32 for BIP, one every 32 offset by 16 for SRRIP
        if ((s & 0x1F) == 0)            set_type[s] = LEADER_BIP;
        else if ((s & 0x1F) == 16)      set_type[s] = LEADER_SRRIP;
        else                            set_type[s] = FOLLOWER;
        for (int c = 0; c < NUM_CORE; c++) {
            for (int w = 0; w < LLC_WAYS; w++) {
                valid[c][s][w] = false;
                rrpv[c][s][w] = MAX_RRPV;
            }
        }
    }
}

// Select a victim via SRRIP scanning
uint32_t GetVictimInSet(
    uint32_t cpu,
    uint32_t set,
    const BLOCK *current_set,
    uint64_t PC,
    uint64_t paddr,
    uint32_t type
) {
    // 1) Prefer any invalid way
    for (uint32_t w = 0; w < LLC_WAYS; w++) {
        if (!valid[cpu][set][w]) {
            return w;
        }
    }
    // 2) Otherwise, find RRPV == MAX_RRPV, aging others
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
        // On hit: promote to MRU (RRPV=0)
        hit_accesses++;
        rrpv[cpu][set][way] = 0;
        return;
    }
    // Miss path
    miss_accesses++;
    // Decide which insertion policy to use here
    bool use_bip;
    if (set_type[set] == LEADER_BIP) {
        use_bip = true;
        // BIP leader miss → BIP underperformed → move PSEL toward SRRIP
        if (PSEL > 0) PSEL--;
    }
    else if (set_type[set] == LEADER_SRRIP) {
        use_bip = false;
        // SRRIP leader miss → SRRIP underperformed → move PSEL toward BIP
        if (PSEL < PSEL_MAX) PSEL++;
    }
    else {
        // Follower sets follow global PSEL decision
        use_bip = (PSEL >= (PSEL_MAX + 1) / 2);
    }
    // Install new block
    valid[cpu][set][way] = true;
    if (use_bip) {
        // BIP insertion: mostly far, occasional near
        bip_counter++;
        if ((bip_counter & (BIP_PROB - 1)) == 0) {
            rrpv[cpu][set][way] = 0;
        } else {
            rrpv[cpu][set][way] = MAX_RRPV;
        }
    } else {
        // SRRIP insertion: mid‐priority for temporal reuse
        rrpv[cpu][set][way] = MAX_RRPV - 1;
    }
}

// Print end‐of‐simulation statistics
void PrintStats() {
    double hr = total_accesses
              ? (double)hit_accesses / total_accesses * 100.0
              : 0.0;
    std::cout << "==== DIP Final Stats ====\n";
    std::cout << "Total Accesses : " << total_accesses << "\n";
    std::cout << " Hits          : " << hit_accesses  << "\n";
    std::cout << " Misses        : " << miss_accesses << "\n";
    std::cout << "Hit Rate (%)   : " << hr            << "\n";
    std::cout << "PSEL Value     : " << PSEL          << "\n";
}

// Print periodic (heartbeat) statistics
void PrintStats_Heartbeat() {
    double hr = total_accesses
              ? (double)hit_accesses / total_accesses * 100.0
              : 0.0;
    std::cout << "[DIP HB] Acc=" << total_accesses
              << " Hit="  << hit_accesses
              << " Miss=" << miss_accesses
              << " HR="   << hr
              << "% PSEL=" << PSEL << "\n";
}