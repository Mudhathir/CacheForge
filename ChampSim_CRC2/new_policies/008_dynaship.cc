#include <vector>
#include <cstdint>
#include <iostream>
#include "../inc/champsim_crc2.h"

#define NUM_CORE      1
#define LLC_SETS      (NUM_CORE * 2048)
#define LLC_WAYS      16

// RRIP parameters
#define RRPV_BITS     2
#define MAX_RRPV      ((1 << RRPV_BITS) - 1)

// Tri-level SHiP predictor parameters
#define SHIP_TABLE_BITS 14
#define SHIP_TABLE_SIZE (1 << SHIP_TABLE_BITS)
#define SHIP_CTR_BITS   2
#define SHIP_CTR_MAX    ((1 << SHIP_CTR_BITS) - 1)
#define SHIP_CTR_INIT   (SHIP_CTR_MAX / 2)

// Dueling parameters
#define PSEL_BITS      10
#define PSEL_MAX       ((1 << PSEL_BITS) - 1)
#define PSEL_INIT      (PSEL_MAX >> 1)
#define DUEL_WINDOW    64  // one LRU-duel set at set%DUEL_WINDOW==0, one SRRIP-duel at ==1

static uint8_t   rrpv    [NUM_CORE][LLC_SETS][LLC_WAYS];
static bool      valid   [NUM_CORE][LLC_SETS][LLC_WAYS];
static uint16_t  sig     [NUM_CORE][LLC_SETS][LLC_WAYS];
static bool      reused  [NUM_CORE][LLC_SETS][LLC_WAYS];
static uint8_t   ship_table[SHIP_TABLE_SIZE];
static uint16_t  psel;

// Statistics
static uint64_t total_accesses = 0, hit_accesses = 0, miss_accesses = 0;

void InitReplacementState() {
    total_accesses = hit_accesses = miss_accesses = 0;
    psel = PSEL_INIT;
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
    for (int i = 0; i < SHIP_TABLE_SIZE; i++) {
        ship_table[i] = SHIP_CTR_INIT;
    }
}

// Find a victim by SRRIP-style eviction
uint32_t GetVictimInSet(
    uint32_t cpu,
    uint32_t set,
    const BLOCK *current_set,
    uint64_t PC,
    uint64_t paddr,
    uint32_t type
) {
    // 1) Invalid line
    for (uint32_t w = 0; w < LLC_WAYS; w++) {
        if (!valid[cpu][set][w]) {
            return w;
        }
    }
    // 2) Evict RRPV==MAX_RRPV, aging otherwise
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
        // Promote to MRU
        rrpv[cpu][set][way]  = 0;
        reused[cpu][set][way] = true;
        return;
    }
    miss_accesses++;
    // 1) Dueling update on miss in dueling sets
    if ((set % DUEL_WINDOW) == 0) {
        // LRU dueling set: if it misses, we penalize LRU => favor SRRIP
        if (psel > 0) psel--;
    }
    else if ((set % DUEL_WINDOW) == 1) {
        // SRRIP dueling set: if it misses, we penalize SRRIP => favor LRU
        if (psel < PSEL_MAX) psel++;
    }
    // 2) Update SHiP predictor based on evicted line
    if (valid[cpu][set][way]) {
        uint16_t old_sig = sig[cpu][set][way];
        if (reused[cpu][set][way]) {
            if (ship_table[old_sig] < SHIP_CTR_MAX)
                ship_table[old_sig]++;
        } else {
            if (ship_table[old_sig] > 0)
                ship_table[old_sig]--;
        }
    }
    // 3) Prepare new block
    valid[cpu][set][way]   = true;
    uint16_t new_sig       = (PC >> 2) & (SHIP_TABLE_SIZE - 1);
    sig[cpu][set][way]     = new_sig;
    reused[cpu][set][way]  = false;
    uint8_t ctr            = ship_table[new_sig];
    // 4) Tri-level with dynamic moderate mapping
    if (ctr == SHIP_CTR_MAX) {
        // Heavy reuse → MRU
        rrpv[cpu][set][way] = 0;
    }
    else if (ctr == SHIP_CTR_MAX - 1) {
        // Moderate reuse → dynamic or dueling override
        if ((set % DUEL_WINDOW) == 0) {
            // LRU-duel: always MRU
            rrpv[cpu][set][way] = 0;
        }
        else if ((set % DUEL_WINDOW) == 1) {
            // SRRIP-duel: always near-evict
            rrpv[cpu][set][way] = MAX_RRPV - 1;
        }
        else {
            // All other sets: choose based on PSEL
            if (psel > PSEL_INIT)
                rrpv[cpu][set][way] = 0;            // favor LRU
            else
                rrpv[cpu][set][way] = MAX_RRPV - 1; // favor SRRIP
        }
    }
    else {
        // Low reuse/streaming → evict quickly
        rrpv[cpu][set][way] = MAX_RRPV;
    }
}

void PrintStats() {
    double hr = total_accesses ? (double)hit_accesses / total_accesses * 100.0 : 0.0;
    std::cout << "==== DynaSHiP Final Stats ====\n";
    std::cout << "Total Accesses : " << total_accesses << "\n";
    std::cout << " Hits          : " << hit_accesses  << "\n";
    std::cout << " Misses        : " << miss_accesses << "\n";
    std::cout << "Hit Rate (%)   : " << hr            << "\n";
    std::cout << "Final PSEL     : " << psel          << "\n";
}

void PrintStats_Heartbeat() {
    double hr = total_accesses ? (double)hit_accesses / total_accesses * 100.0 : 0.0;
    std::cout << "[DynaSHiP HB] Acc=" << total_accesses
              << " Hit="  << hit_accesses
              << " Miss=" << miss_accesses
              << " HR="   << hr
              << "% PSEL=" << psel
              << "\n";
}