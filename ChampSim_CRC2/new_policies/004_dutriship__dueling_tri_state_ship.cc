#include <vector>
#include <cstdint>
#include <iostream>
#include "../inc/champsim_crc2.h"

#define NUM_CORE       1
#define LLC_SETS       (NUM_CORE * 2048)
#define LLC_WAYS       16

// RRPV and SHCT parameters
#define RRPV_MAX       3       // 2-bit RRPV: 0..3
#define SHCT_SIZE      4096    // must be power of two
#define SHCT_THRESHOLD 2       // moderate vs. heavy reuse
#define SHCT_INIT      SHCT_THRESHOLD
#define SHCT_MAX       3       // saturating counter max

// PSEL (set dueling) parameters
#define PSEL_MAX       1023
#define PSEL_HALF      (PSEL_MAX/2)

// Duelling sample policy: use low 6 bits of set index
inline bool is_leader_tric(uint32_t set)  { return ((set & 0x3F) == 0); }
inline bool is_leader_srrip(uint32_t set){ return ((set & 0x3F) == 1); }

// Per-block metadata
static uint8_t  RRPV      [LLC_SETS][LLC_WAYS];
static bool     reused    [LLC_SETS][LLC_WAYS];
static uint16_t block_sig [LLC_SETS][LLC_WAYS];
// Signature counter
static uint8_t  SHCT      [SHCT_SIZE];
// Global policy selector
static uint16_t PSEL;

// Build a small PC signature (same as TriSHiP)
static inline uint32_t MakeSignature(uint64_t PC) {
    return (uint32_t)((PC ^ (PC >> 12) ^ (PC >> 20)) & (SHCT_SIZE - 1));
}

void InitReplacementState() {
    // Initialize RRPVs & per-block state
    for (uint32_t s = 0; s < LLC_SETS; s++) {
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            RRPV[s][w]      = RRPV_MAX;
            reused[s][w]    = false;
            block_sig[s][w] = 0;
        }
    }
    // Initialize signature counters
    for (uint32_t i = 0; i < SHCT_SIZE; i++) {
        SHCT[i] = SHCT_INIT;
    }
    // Initialize dueling counter
    PSEL = PSEL_HALF;
}

// SRRIP victim selection (same for both policies)
uint32_t GetVictimInSet(
    uint32_t                cpu,
    uint32_t                set,
    const BLOCK           * current_set,
    uint64_t                PC,
    uint64_t                paddr,
    uint32_t                type
) {
    // Standard SRRIP: find RRPV==MAX, else age all and retry
    while (true) {
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            if (RRPV[set][w] == RRPV_MAX)
                return w;
        }
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            if (RRPV[set][w] < RRPV_MAX)
                RRPV[set][w]++;
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
    // On a miss, update the dueling counter
    if (!hit) {
        if (is_leader_tric(set)) {
            // TriSHiP sample: fewer misses → decrement
            if (PSEL > 0) PSEL--;
        } else if (is_leader_srrip(set)) {
            // SRRIP sample: fewer misses → increment
            if (PSEL < PSEL_MAX) PSEL++;
        }
    }

    // Decide which policy to apply in this set
    bool use_tri;
    if (is_leader_tric(set)) {
        use_tri = true;
    } else if (is_leader_srrip(set)) {
        use_tri = false;
    } else {
        // follower sets follow winner: PSEL<half → Tri wins; else SRRIP wins
        use_tri = (PSEL < PSEL_HALF);
    }

    // On hit: promote to MRU in all policies
    if (hit) {
        RRPV[set][way] = 0;
        if (use_tri) {
            // TriSHiP: mark reused and reinforce signature
            reused[set][way] = true;
            uint16_t sig = block_sig[set][way];
            if (SHCT[sig] < SHCT_MAX) SHCT[sig]++;
        }
        return;
    }

    // On miss: if TriSHiP, update SHCT for the evicted block
    if (use_tri) {
        uint16_t old_sig = block_sig[set][way];
        if (reused[set][way]) {
            // block saw a hit → strengthen
            if (SHCT[old_sig] < SHCT_MAX) SHCT[old_sig]++;
        } else {
            // never reused → weaken
            if (SHCT[old_sig] > 0) SHCT[old_sig]--;
        }
    }

    // Compute new signature (always store it)
    uint16_t newsig      = MakeSignature(PC);
    block_sig[set][way]  = newsig;
    reused[set][way]     = false;

    // Insert according to the chosen policy
    if (use_tri) {
        // Tri-state insertion
        uint8_t ctr = SHCT[newsig];
        if (ctr == 0) {
            // streaming → insert at LRU
            RRPV[set][way] = RRPV_MAX;
        } else if (ctr < SHCT_THRESHOLD) {
            // moderate reuse → near-MRU
            RRPV[set][way] = RRPV_MAX - 1;
        } else {
            // heavy reuse → MRU
            RRPV[set][way] = 0;
        }
    } else {
        // Plain SRRIP insertion: always near-LRU
        RRPV[set][way] = RRPV_MAX - 1;
    }
}

void PrintStats() {
    std::cout << "### DuTriSHiP Replacement Statistics ###\n";
    std::cout << "PSEL = " << PSEL << "\n";
    // (Optional) Dump SHCT histogram or other counters if desired
}

void PrintStats_Heartbeat() {
    // Not used
}