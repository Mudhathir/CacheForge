#include <vector>
#include <cstdint>
#include <iostream>
#include "../inc/champsim_crc2.h"

#define NUM_CORE 1
#define LLC_SETS (NUM_CORE * 2048)
#define LLC_WAYS 16

// RRPV parameters
#define RRPV_BITS 3
#define RRPV_MAX   ((1 << RRPV_BITS) - 1) // 7
#define RRPV_INIT  (RRPV_MAX - 1)         // 6

// SHiP signature table
#define SIG_TABLE_SIZE      4096
#define SIG_CTR_BITS        2
#define SIG_CTR_MAX         ((1 << SIG_CTR_BITS) - 1) // 3
#define SIG_CTR_INIT        (SIG_CTR_MAX / 2)         // 1
#define CTR_THRESHOLD       (SIG_CTR_MAX / 2 + 1)      // 2

// Stream detection
#define STREAM_DETECT_COUNT 2
#define MAX_SMALL_STRIDE    8

// ----------------------------------------------------------------------------
// Replacement metadata
static uint8_t  repl_rrpv   [LLC_SETS][LLC_WAYS];
static uint16_t repl_sig    [LLC_SETS][LLC_WAYS];
static bool     repl_has_hit[LLC_SETS][LLC_WAYS];

// SHiP tables & stream detection
static uint8_t  ship_ctr     [SIG_TABLE_SIZE];
static int64_t  last_block   [SIG_TABLE_SIZE];
static int64_t  last_stride  [SIG_TABLE_SIZE];
static uint8_t  stride_run   [SIG_TABLE_SIZE];

// Statistics
static uint64_t stat_hits, stat_misses, stat_evictions;

// Initialize replacement state
void InitReplacementState() {
    // Initialize SRRIP state and SHiP tables
    for (uint32_t s = 0; s < LLC_SETS; s++) {
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            repl_rrpv[s][w]    = RRPV_MAX;
            repl_sig[s][w]     = 0;
            repl_has_hit[s][w] = false;
        }
    }
    for (uint32_t i = 0; i < SIG_TABLE_SIZE; i++) {
        ship_ctr[i]    = SIG_CTR_INIT;
        last_block[i]  = -1;
        last_stride[i] = 0;
        stride_run[i]  = 0;
    }
    stat_hits = stat_misses = stat_evictions = 0;
}

// Victim selection using SRRIP
uint32_t GetVictimInSet(
    uint32_t /*cpu*/,
    uint32_t set,
    const BLOCK */*current_set*/,
    uint64_t /*PC*/,
    uint64_t /*paddr*/,
    uint32_t /*type*/
) {
    while (true) {
        // Find RRPV == max
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            if (repl_rrpv[set][w] == RRPV_MAX)
                return w;
        }
        // Otherwise, age all
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            if (repl_rrpv[set][w] < RRPV_MAX)
                repl_rrpv[set][w]++;
        }
    }
    return 0; // unreachable
}

// Update state on every access
void UpdateReplacementState(
    uint32_t /*cpu*/,
    uint32_t set,
    uint32_t way,
    uint64_t paddr,
    uint64_t PC,
    uint64_t /*victim_addr*/,
    uint32_t /*type*/,
    uint8_t hit
) {
    if (hit) {
        // ---- HIT ----
        stat_hits++;
        // If first hit since fill, update SHiP counter
        if (!repl_has_hit[set][way]) {
            uint16_t psig = repl_sig[set][way];
            if (ship_ctr[psig] < SIG_CTR_MAX)
                ship_ctr[psig]++;
            repl_has_hit[set][way] = true;
        }
        // Promote to MRU
        repl_rrpv[set][way] = 0;
    } else {
        // ---- MISS & EVICTION ----
        stat_misses++;
        stat_evictions++;
        // Update SHiP on the evicted block
        uint16_t old_sig = repl_sig[set][way];
        bool old_hit    = repl_has_hit[set][way];
        if (!old_hit && ship_ctr[old_sig] > 0)
            ship_ctr[old_sig]--;

        // Compute new signature
        uint16_t sig = ((PC >> 2) ^ (PC >> 12)) & (SIG_TABLE_SIZE - 1);

        // Small‐stride stream detection
        int64_t curr_block = paddr >> 6;
        bool is_stream = false;
        if (last_block[sig] >= 0) {
            int64_t stride = curr_block - last_block[sig];
            if (stride == last_stride[sig]) {
                stride_run[sig]++;
            } else {
                last_stride[sig] = stride;
                stride_run[sig]  = 1;
            }
            if (stride_run[sig] >= STREAM_DETECT_COUNT &&
                last_stride[sig] <= MAX_SMALL_STRIDE &&
                last_stride[sig] >= -MAX_SMALL_STRIDE) {
                is_stream = true;
            }
        } else {
            stride_run[sig]  = 1;
            last_stride[sig] = 0;
        }
        last_block[sig] = curr_block;

        // Insertion decisions
        if (ship_ctr[sig] >= CTR_THRESHOLD) {
            // Hot → MRU
            repl_rrpv[set][way] = 0;
        }
        else if (is_stream) {
            // Spatial stream → near-MRU
            repl_rrpv[set][way] = (RRPV_INIT > 0 ? RRPV_INIT - 1 : 0);
        }
        else if (ship_ctr[sig] > 0) {
            // Potential reuse → default SRRIP
            repl_rrpv[set][way] = RRPV_INIT;
        }
        else {
            // Cold → bypass
            repl_rrpv[set][way] = RRPV_MAX;
        }

        // Install metadata for the new block
        repl_sig[set][way]     = sig;
        repl_has_hit[set][way] = false;
    }
}

// Print final statistics
void PrintStats() {
    uint64_t total = stat_hits + stat_misses;
    double hr = total ? (double(stat_hits) * 100.0 / double(total)) : 0.0;
    std::cout << "==== SHiPPlus Statistics ====\n";
    std::cout << "Total refs   : " << total          << "\n";
    std::cout << "Hits         : " << stat_hits      << "\n";
    std::cout << "Misses       : " << stat_misses    << "\n";
    std::cout << "Evictions    : " << stat_evictions << "\n";
    std::cout << "Hit rate (%) : " << hr             << "\n";
}

// Heartbeat prints the same
void PrintStats_Heartbeat() {
    PrintStats();
}