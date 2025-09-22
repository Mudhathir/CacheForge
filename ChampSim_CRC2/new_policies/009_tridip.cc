#include <vector>
#include <cstdint>
#include <iostream>
#include <cstdlib>
#include "../inc/champsim_crc2.h"

#define NUM_CORE      1
#define LLC_SETS      (NUM_CORE * 2048)
#define LLC_WAYS      16

// RRIP parameters
#define RRPV_BITS     2
#define MAX_RRPV      ((1 << RRPV_BITS) - 1)

// SHiP predictor parameters
#define SHIP_TABLE_BITS 14
#define SHIP_TABLE_SIZE (1 << SHIP_TABLE_BITS)
#define SHIP_CTR_BITS   2
#define SHIP_CTR_MAX    ((1 << SHIP_CTR_BITS) - 1)
#define SHIP_CTR_INIT   (SHIP_CTR_MAX / 2)

// Dueling parameters
#define PSEL_BITS      10
#define PSEL_MAX       ((1 << PSEL_BITS) - 1)
#define PSEL_INIT      (PSEL_MAX >> 1)
#define DUEL_WINDOW    64   // use sets mod 0/1 for LRU vs SRRIP, 2/3 for SRRIP vs BIP

// BIP probability: 1 in BIP_PROB inserts as MRU
#define BIP_PROB       32

static uint8_t   rrpv      [NUM_CORE][LLC_SETS][LLC_WAYS];
static bool      valid     [NUM_CORE][LLC_SETS][LLC_WAYS];
static uint16_t  sig       [NUM_CORE][LLC_SETS][LLC_WAYS];
static bool      reused    [NUM_CORE][LLC_SETS][LLC_WAYS];
static uint8_t   ship_table[SHIP_TABLE_SIZE];
static uint16_t  psel1, psel2;

// Statistics
static uint64_t total_accesses = 0, hit_accesses = 0, miss_accesses = 0;

void InitReplacementState() {
    total_accesses = hit_accesses = miss_accesses = 0;
    psel1 = psel2 = PSEL_INIT;
    std::srand(0);
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
        // Promote to MRU on hit
        rrpv[cpu][set][way] = 0;
        reused[cpu][set][way] = true;
        return;
    }
    miss_accesses++;
    // 1) Dueling updates on every miss
    uint32_t d = set % DUEL_WINDOW;
    if (d == 0) {
        // Duel1 LRU vs SRRIP: this set uses LRU
        if (psel1 > 0) psel1--;
    }
    else if (d == 1) {
        // Duel1 second half uses SRRIP
        if (psel1 < PSEL_MAX) psel1++;
    }
    else if (d == 2) {
        // Duel2 SRRIP vs BIP: this set uses SRRIP
        if (psel2 > 0) psel2--;
    }
    else if (d == 3) {
        // Duel2 second half uses BIP
        if (psel2 < PSEL_MAX) psel2++;
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

    // 4) Tri-level insertion with ternary moderate policy
    if (ctr == SHIP_CTR_MAX) {
        // Heavy reuse → MRU
        rrpv[cpu][set][way] = 0;
    }
    else if (ctr == (SHIP_CTR_MAX - 1)) {
        // Moderate reuse → choose among LRU, SRRIP, BIP
        switch (d) {
            case 0:
                // Duel1-LRU sets
                rrpv[cpu][set][way] = 0;
                break;
            case 1:
                // Duel1-SRRIP sets
                rrpv[cpu][set][way] = MAX_RRPV - 1;
                break;
            case 2:
                // Duel2-SRRIP sets
                rrpv[cpu][set][way] = MAX_RRPV - 1;
                break;
            case 3:
                // Duel2-BIP sets
                if ((std::rand() % BIP_PROB) == 0)
                    rrpv[cpu][set][way] = 0;
                else
                    rrpv[cpu][set][way] = MAX_RRPV - 1;
                break;
            default:
                // All other sets: use global duel results
                if (psel1 > PSEL_INIT) {
                    // Favor LRU
                    rrpv[cpu][set][way] = 0;
                }
                else if (psel2 > PSEL_INIT) {
                    // Favor SRRIP
                    rrpv[cpu][set][way] = MAX_RRPV - 1;
                }
                else {
                    // Favor BIP
                    if ((std::rand() % BIP_PROB) == 0)
                        rrpv[cpu][set][way] = 0;
                    else
                        rrpv[cpu][set][way] = MAX_RRPV - 1;
                }
                break;
        }
    }
    else {
        // Low reuse / streaming → near-evict
        rrpv[cpu][set][way] = MAX_RRPV;
    }
}

void PrintStats() {
    double hr = total_accesses ? (double)hit_accesses / total_accesses * 100.0 : 0.0;
    std::cout << "==== TriDIP Final Stats ====\n";
    std::cout << "Total Accesses : " << total_accesses << "\n";
    std::cout << " Hits          : " << hit_accesses  << "\n";
    std::cout << " Misses        : " << miss_accesses << "\n";
    std::cout << "Hit Rate (%)   : " << hr            << "\n";
    std::cout << "Final PSEL1    : " << psel1         << "\n";
    std::cout << "Final PSEL2    : " << psel2         << "\n";
}

void PrintStats_Heartbeat() {
    double hr = total_accesses ? (double)hit_accesses / total_accesses * 100.0 : 0.0;
    std::cout << "[TriDIP HB] Acc=" << total_accesses
              << " Hit="  << hit_accesses
              << " Miss=" << miss_accesses
              << " HR="   << hr
              << "% PSEL1=" << psel1
              << " PSEL2=" << psel2
              << "\n";
}