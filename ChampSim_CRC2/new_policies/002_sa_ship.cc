#include <vector>
#include <cstdint>
#include <iostream>
#include "../inc/champsim_crc2.h"

#define NUM_CORE        1
#define LLC_SETS        (NUM_CORE * 2048)
#define LLC_WAYS        16

// RRIP parameters
#define RRPV_MAX        3       // 2-bit RRPV: 0..3
// SHiP signature table parameters
#define SHCT_SIZE       4096    // must be power of two
#define SHCT_THRESHOLD  2       // counters >= 2 → hot

// Per-block metadata
static uint8_t   RRPV      [LLC_SETS][LLC_WAYS];
static bool      reused    [LLC_SETS][LLC_WAYS];
static uint16_t  block_sig [LLC_SETS][LLC_WAYS];
// Signature Counter Table (SHiP)
static uint8_t   SHCT      [SHCT_SIZE];

// Make a small signature from the PC
static inline uint32_t MakeSignature(uint64_t PC) {
    // combine bits of PC to form an index
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
        SHCT[i] = 0;
    }
}

// Standard SRRIP victim selection
uint32_t GetVictimInSet(
    uint32_t                cpu,
    uint32_t                set,
    const BLOCK           * current_set,
    uint64_t                PC,
    uint64_t                paddr,
    uint32_t                type
) {
    // find a line with RRPV == RRPV_MAX, or age all and retry
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
    // unreachable
    return 0;
}

// Update on hit/miss
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
        // On a hit: promote to MRU, mark reused, boost the SHCT counter
        RRPV[set][way]   = 0;
        reused[set][way] = true;
        uint16_t sig = block_sig[set][way];
        if (SHCT[sig] < 3) SHCT[sig]++;
        return;
    }

    // On a miss: first update the SHCT entry of the evicted block
    uint16_t old_sig = block_sig[set][way];
    if (reused[set][way]) {
        // block was reused → reinforce the signature
        if (SHCT[old_sig] < 3) SHCT[old_sig]++;
    } else {
        // block never reused → weaken the signature
        if (SHCT[old_sig] > 0) SHCT[old_sig]--;
    }

    // Prepare metadata for the newly filled block
    uint16_t newsig        = MakeSignature(PC);
    block_sig[set][way]    = newsig;
    reused[set][way]       = false;

    // Insert based on signature counter
    if (SHCT[newsig] >= SHCT_THRESHOLD) {
        // predicted hot → MRU insert
        RRPV[set][way] = 0;
    } else {
        // predicted cold → near-MRU insert (small reuse window)
        RRPV[set][way] = RRPV_MAX - 1;
    }
}

// End-of-simulation statistics
void PrintStats() {
    std::cout << "### SA-SHiP Replacement Statistics ###\n";
    // (Optional) Could dump SHCT hot vs cold counts here
}

// Periodic (heartbeat) statistics
void PrintStats_Heartbeat() {
    // e.g., sample a few SHCT counters, print phase changes, etc.
}