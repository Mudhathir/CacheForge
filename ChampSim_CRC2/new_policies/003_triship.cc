#include <vector>
#include <cstdint>
#include <iostream>
#include "../inc/champsim_crc2.h"

#define NUM_CORE        1
#define LLC_SETS        (NUM_CORE * 2048)
#define LLC_WAYS        16

// RRPV and SHCT parameters
#define RRPV_MAX        3          // 2-bit: 0..3
#define SHCT_SIZE       4096       // must be power of two
#define SHCT_THRESHOLD  2          // counters >=2 → heavy reuse
#define SHCT_INIT       SHCT_THRESHOLD  // start at heavy so new PCs insert MRU

// Per-block metadata
static uint8_t   RRPV      [LLC_SETS][LLC_WAYS];
static bool      reused    [LLC_SETS][LLC_WAYS];
static uint16_t  block_sig [LLC_SETS][LLC_WAYS];
// Signature counter
static uint8_t   SHCT      [SHCT_SIZE];

// Build a small signature from the load PC
static inline uint32_t MakeSignature(uint64_t PC) {
    // XOR-fold bits to index into SHCT
    return (uint32_t)((PC ^ (PC >> 12) ^ (PC >> 20)) & (SHCT_SIZE - 1));
}

// Initialize all state
void InitReplacementState() {
    for (uint32_t s = 0; s < LLC_SETS; s++) {
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            RRPV[s][w]      = RRPV_MAX;
            reused[s][w]    = false;
            block_sig[s][w] = 0;
        }
    }
    for (uint32_t i = 0; i < SHCT_SIZE; i++) {
        SHCT[i] = SHCT_INIT;
    }
}

// Standard SRRIP victim selection: find RRPV==MAX or age all and retry
uint32_t GetVictimInSet(
    uint32_t                cpu,
    uint32_t                set,
    const BLOCK           * current_set,
    uint64_t                PC,
    uint64_t                paddr,
    uint32_t                type
) {
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

// Update on hit/miss and adapt the three-state insertion
void UpdateReplacementState(
    uint32_t                cpu,
    uint32_t                set,
    uint32_t                way,
    uint64_t                paddr,
    uint64_t                PC,
    uint64_t                victim_addr,
    uint32_t                type,
    uint8_t                 hit
) {
    if (hit) {
        // On hit: promote to MRU, mark reused, reinforce signature
        RRPV[set][way]   = 0;
        reused[set][way] = true;
        uint16_t sig = block_sig[set][way];
        if (SHCT[sig] < 3) SHCT[sig]++;
        return;
    }

    // On miss: update SHCT for the evicted block
    uint16_t old_sig = block_sig[set][way];
    if (reused[set][way]) {
        // block saw at least one hit → reinforce
        if (SHCT[old_sig] < 3) SHCT[old_sig]++;
    } else {
        // never reused → weaken
        if (SHCT[old_sig] > 0) SHCT[old_sig]--;
    }

    // Prepare new block's signature
    uint16_t newsig     = MakeSignature(PC);
    block_sig[set][way] = newsig;
    reused[set][way]    = false;

    // Tri-state insertion based on signature counter
    uint8_t ctr = SHCT[newsig];
    if (ctr == 0) {
        // streaming → bypass (insert at LRU so evicted immediately)
        RRPV[set][way] = RRPV_MAX;
    } else if (ctr < SHCT_THRESHOLD) {
        // moderate reuse → near-MRU
        RRPV[set][way] = RRPV_MAX - 1;
    } else {
        // heavy reuse → MRU
        RRPV[set][way] = 0;
    }
}

// End-of-simulation statistics
void PrintStats() {
    std::cout << "### TriSHiP Replacement Statistics ###\n";
    // (Optional) could dump SHCT histogram here
}

// Periodic (heartbeat) statistics
void PrintStats_Heartbeat() {
    // not used
}