#include <vector>
#include <cstdint>
#include <iostream>
#include "../inc/champsim_crc2.h"

#define NUM_CORE         1
#define LLC_SETS         (NUM_CORE * 2048)
#define LLC_WAYS         16

// --- RRIP parameters ---
#define RRPV_BITS        2
#define RRPV_MAX         ((1 << RRPV_BITS) - 1)   // 3

// --- Reuse-Interval Predictor (RIP) parameters ---
#define PRT_SIZE         32768
#define PRT_MASK         (PRT_SIZE - 1)
#define RD_SAMPLE_THRESH 2                       // min samples to trust prediction
#define WINDOW_SIZE      100000                  // tuning constant for thresholds

// Replacement state
static uint8_t   repl_rrpv[LLC_SETS][LLC_WAYS];
static uint8_t   reused[LLC_SETS][LLC_WAYS];
static uint16_t  sigtbl[LLC_SETS][LLC_WAYS];
static uint64_t  fill_time[LLC_SETS][LLC_WAYS];

// Per-signature reuse-interval statistics
static uint64_t  rd_sum[PRT_SIZE];
static uint32_t  rd_cnt[PRT_SIZE];

// Global counters
static uint64_t  stat_hits, stat_misses;
static uint64_t  global_counter;

// Find a victim by standard RRIP aging
static uint32_t FindVictimWay(uint32_t set) {
    while (true) {
        // 1) look for maximal RRPV
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            if (repl_rrpv[set][w] == RRPV_MAX)
                return w;
        }
        // 2) age all entries
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            if (repl_rrpv[set][w] < RRPV_MAX)
                repl_rrpv[set][w]++;
        }
    }
}

// Initialize replacement state
void InitReplacementState() {
    stat_hits      = 0;
    stat_misses    = 0;
    global_counter = 0;
    // Initialize per-line state
    for (int s = 0; s < LLC_SETS; s++) {
        for (int w = 0; w < LLC_WAYS; w++) {
            repl_rrpv[s][w] = RRPV_MAX;  // cold
            reused[s][w]    = 0;
            sigtbl[s][w]    = 0;
            fill_time[s][w] = 0;
        }
    }
    // Initialize predictor tables
    for (int i = 0; i < PRT_SIZE; i++) {
        rd_sum[i] = 0;
        rd_cnt[i] = 0;
    }
}

// Select victim and decay signature confidence if un‐reused
uint32_t GetVictimInSet(
    uint32_t /*cpu*/,
    uint32_t set,
    const BLOCK */*current_set*/,
    uint64_t /*PC*/,
    uint64_t /*paddr*/,
    uint32_t /*type*/
) {
    uint32_t victim = FindVictimWay(set);
    uint16_t sig    = sigtbl[set][victim];
    // If this block was never reused, decay the signature's sample count
    if (!reused[set][victim] && rd_cnt[sig] > 0) {
        rd_cnt[sig]--;
    }
    // Clear metadata (will be reset on fill)
    reused[set][victim]    = 0;
    sigtbl[set][victim]    = 0;
    fill_time[set][victim] = 0;
    return victim;
}

// Update on hit or fill
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
    // Advance global access counter
    global_counter++;

    if (hit) {
        // On hit: promote to MRU and update reuse‐interval stats
        stat_hits++;
        repl_rrpv[set][way] = 0;
        reused[set][way]    = 1;
        uint16_t sig        = sigtbl[set][way];
        // Measure interval since last reference
        uint64_t age        = global_counter - fill_time[set][way];
        // Update predictor
        rd_sum[sig]        += age;
        if (rd_cnt[sig] < UINT32_MAX) rd_cnt[sig]++;
        // Reset fill_time to now to measure next interval
        fill_time[set][way] = global_counter;
    } else {
        // On miss: allocate and insert based on predicted reuse interval
        stat_misses++;
        uint16_t sig              = static_cast<uint16_t>(PC) & PRT_MASK;
        sigtbl[set][way]          = sig;
        reused[set][way]          = 0;
        fill_time[set][way]       = global_counter;
        // Determine insertion RRPV by average reuse interval
        if (rd_cnt[sig] >= RD_SAMPLE_THRESH) {
            uint64_t avg = rd_sum[sig] / rd_cnt[sig];
            if (avg < (WINDOW_SIZE >> 2)) {
                repl_rrpv[set][way] = 0;            // short interval → keep long
            } else if (avg < (WINDOW_SIZE >> 1)) {
                repl_rrpv[set][way] = RRPV_MAX - 1;  // moderate interval
            } else {
                repl_rrpv[set][way] = RRPV_MAX;      // long interval → evict soon
            }
        } else {
            // low confidence signatures treated as one‐shot
            repl_rrpv[set][way] = RRPV_MAX;
        }
    }
}

// Print end‐of‐simulation statistics
void PrintStats() {
    uint64_t total = stat_hits + stat_misses;
    double   hr    = total ? (100.0 * stat_hits / total) : 0.0;
    std::cout << "==== ARD-RRIP Policy Statistics ====\n";
    std::cout << "Total refs   : " << total     << "\n";
    std::cout << "Hits         : " << stat_hits << "\n";
    std::cout << "Misses       : " << stat_misses << "\n";
    std::cout << "Hit Rate (%) : " << hr        << "\n";
}

// Print periodic (heartbeat) statistics
void PrintStats_Heartbeat() {
    PrintStats();
}