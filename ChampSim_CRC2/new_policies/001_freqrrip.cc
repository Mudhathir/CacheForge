#include <vector>
#include <cstdint>
#include <iostream>
#include "../inc/champsim_crc2.h"

#define NUM_CORE          1
#define LLC_SETS          (NUM_CORE * 2048)
#define LLC_WAYS          16

// Frequency table parameters
#define FREQ_TABLE_SIZE   4096    // power of two
#define FREQ_THRESHOLD    1       // PC_FREQ >= 1 → MRU insert
#define RRPV_MAX          3       // SRRIP: 2-bit RRPV (0..3)

// Per-block metadata
static uint8_t   RRPV      [LLC_SETS][LLC_WAYS];
static bool      reused    [LLC_SETS][LLC_WAYS];
static uint16_t  block_sig [LLC_SETS][LLC_WAYS];
// Global PC frequency counters
static uint8_t   PC_FREQ   [FREQ_TABLE_SIZE];

// Hash PC → small signature
static inline uint32_t MakeSignature(uint64_t PC) {
    return (uint32_t)((PC ^ (PC >> 12)) & (FREQ_TABLE_SIZE - 1));
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
    for (uint32_t i = 0; i < FREQ_TABLE_SIZE; i++) {
        PC_FREQ[i] = 0;
    }
}

// Standard SRRIP victim selection
uint32_t GetVictimInSet(
    uint32_t cpu,
    uint32_t set,
    const BLOCK *current_set,
    uint64_t PC,
    uint64_t paddr,
    uint32_t type
) {
    // Find a line with RRPV == RRPV_MAX, or age all and retry
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
    uint32_t cpu,
    uint32_t set,
    uint32_t way,
    uint64_t paddr,
    uint64_t PC,
    uint64_t victim_addr,
    uint32_t type,
    uint8_t hit
) {
    if (hit) {
        // On a hit: promote to MRU, mark reused, boost PC counter
        RRPV[set][way]   = 0;
        reused[set][way] = true;
        uint16_t sig = block_sig[set][way];
        if (PC_FREQ[sig] < 255) PC_FREQ[sig]++;
        return;
    }
    // On miss: we are filling way 'way', so first train on the evicted block
    uint16_t old_sig = block_sig[set][way];
    if (reused[set][way]) {
        if (PC_FREQ[old_sig] < 255) PC_FREQ[old_sig]++;
    } else {
        if (PC_FREQ[old_sig] > 0) PC_FREQ[old_sig]--;
    }
    // Prepare metadata for the new block
    uint32_t newsig = MakeSignature(PC);
    block_sig[set][way] = newsig;
    reused[set][way]    = false;
    // Insertion decision based on the PC frequency
    if (PC_FREQ[newsig] >= FREQ_THRESHOLD) {
        // Hot PC → MRU insert
        RRPV[set][way] = 0;
    } else {
        // Cold/streaming PC → bypass/far insert
        RRPV[set][way] = RRPV_MAX;
    }
}

// End‐of‐sim statistics
void PrintStats() {
    std::cout << "### FreqRRIP Statistics ###\n";
}

// Optional periodic reporting
void PrintStats_Heartbeat() {
    // e.g., print a sample of the hottest PC_FREQ entries
}