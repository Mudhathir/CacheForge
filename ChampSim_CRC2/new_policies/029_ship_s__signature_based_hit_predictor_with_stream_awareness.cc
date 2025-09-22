#include <vector>
#include <cstdint>
#include <iostream>
#include <cstring>
#include "../inc/champsim_crc2.h"

#define NUM_CORE         1
#define LLC_SETS         (NUM_CORE * 2048)
#define LLC_WAYS         16

// RRIP parameters
#define RRPV_BITS        2
#define RRPV_MAX         ((1 << RRPV_BITS) - 1)   // 3
#define RRPV_INIT        (RRPV_MAX - 1)           // 2

// SHiP signature table
#define SHCT_SIZE        2048
#define SHCT_MAX         3   // 2-bit counter max

// Stream detection per-PC
#define STREAM_PC_SIZE   1024
#define STREAM_PC_MASK   (STREAM_PC_SIZE - 1)

static uint8_t  repl_rrpv   [LLC_SETS][LLC_WAYS];
static bool     repl_has_hit[LLC_SETS][LLC_WAYS];
static uint16_t repl_sig    [LLC_SETS][LLC_WAYS];

// SHiP Signature Hit Counter Table
static uint8_t shct[SHCT_SIZE];

// Simple per-PC stride/stream tracker
struct StreamEntry {
    int64_t last_blk;
    int64_t last_stride;
    uint8_t count;
};
static StreamEntry stream_table[STREAM_PC_SIZE];

// Statistics
static uint64_t stat_hits, stat_misses;

// Initialize replacement state
void InitReplacementState() {
    // Initialize RRIP arrays
    for (int s = 0; s < LLC_SETS; s++) {
        for (int w = 0; w < LLC_WAYS; w++) {
            repl_rrpv[s][w]    = RRPV_INIT;
            repl_has_hit[s][w] = false;
            repl_sig[s][w]     = 0;
        }
    }
    // Initialize SHCT to weakly reusable
    for (int i = 0; i < SHCT_SIZE; i++) {
        shct[i] = SHCT_MAX >> 1;  // =1
    }
    // Initialize stream tracker
    for (int i = 0; i < STREAM_PC_SIZE; i++) {
        stream_table[i].last_blk    = -1;
        stream_table[i].last_stride = 0;
        stream_table[i].count       = 0;
    }
    // Zero stats
    stat_hits   = 0;
    stat_misses = 0;
}

// Victim selection (SRRIP)
uint32_t GetVictimInSet(
    uint32_t /*cpu*/,
    uint32_t set,
    const BLOCK */*current_set*/,
    uint64_t /*PC*/,
    uint64_t /*paddr*/,
    uint32_t /*type*/
) {
    while (true) {
        // 1) Find any way with max RRPV
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            if (repl_rrpv[set][w] == RRPV_MAX) {
                return w;
            }
        }
        // 2) Age all entries
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            if (repl_rrpv[set][w] < RRPV_MAX)
                repl_rrpv[set][w]++;
        }
    }
}

// Update on each access
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
    // Derive PC‐signature for SHiP
    uint16_t sig = (PC >> 2) & (SHCT_SIZE - 1);
    // Derive stream‐tracker index
    uint16_t st_idx = (PC >> 2) & STREAM_PC_MASK;
    // Compute current block address for stride
    int64_t curr_blk = paddr >> 6;
    // Update stream detection
    StreamEntry &st = stream_table[st_idx];
    bool is_stream = false;
    if (st.last_blk != -1) {
        int64_t stride = curr_blk - st.last_blk;
        if (stride == st.last_stride) {
            st.count++;
            if (st.count >= 2) {
                is_stream = true;
            }
        } else {
            st.last_stride = stride;
            st.count = 1;
        }
    }
    st.last_blk = curr_blk;

    if (hit) {
        // ---- HIT ----
        stat_hits++;
        // Promote to MRU
        repl_rrpv[set][way]    = 0;
        repl_has_hit[set][way] = true;
    } else {
        // ---- MISS & INSERT ----
        stat_misses++;
        // Before we overwrite this way, adjust SHCT based on reuse
        uint16_t old_sig = repl_sig[set][way];
        if (repl_has_hit[set][way]) {
            // block was reused -> strengthen prediction
            if (shct[old_sig] < SHCT_MAX) shct[old_sig]++;
        } else {
            // never reused -> weaken prediction
            if (shct[old_sig] > 0) shct[old_sig]--;
        }
        // Choose insertion RRPV
        if (shct[sig] == SHCT_MAX) {
            // strongly predicted reusable
            repl_rrpv[set][way] = 0;
        }
        else if (is_stream) {
            // streaming spatial reuse: default SRRIP insert
            repl_rrpv[set][way] = RRPV_INIT;
        }
        else if (shct[sig] == 0) {
            // strongly predicted non‐reusable
            repl_rrpv[set][way] = RRPV_MAX;
        }
        else {
            // weak/neutral predictions
            repl_rrpv[set][way] = RRPV_INIT;
        }
        // Reset metadata for the new block
        repl_has_hit[set][way] = false;
        repl_sig[set][way]     = sig;
    }
}

// Print end‐of‐simulation statistics
void PrintStats() {
    uint64_t total = stat_hits + stat_misses;
    double   hr    = total ? (100.0 * stat_hits / total) : 0.0;
    std::cout << "==== SHiP-S Statistics ====\n";
    std::cout << "Total refs   : " << total      << "\n";
    std::cout << "Hits         : " << stat_hits  << "\n";
    std::cout << "Misses       : " << stat_misses<< "\n";
    std::cout << "Hit Rate (%) : " << hr         << "\n";
}

// Print periodic (heartbeat) statistics
void PrintStats_Heartbeat() {
    PrintStats();
}