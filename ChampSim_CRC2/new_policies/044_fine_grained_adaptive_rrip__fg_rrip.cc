#include <vector>
#include <cstdint>
#include <iostream>
#include "../inc/champsim_crc2.h"

#define NUM_CORE            1
#define LLC_SETS            (NUM_CORE * 2048)
#define LLC_WAYS            16

// RRIP parameters
#define RRPV_BITS           2
#define RRPV_MAX            ((1 << RRPV_BITS) - 1)

// Signature-based predictor parameters
#define PRED_TABLE_BITS     15
#define PRED_TABLE_SIZE     (1 << PRED_TABLE_BITS)
#define PRED_MASK           (PRED_TABLE_SIZE - 1)
#define PRED_MAX            3
#define PRED_INIT           2

// Epoch parameters
#define EPOCH_INTERVAL      (1 << 15)    // 32768 updates
#define ADAPT_BIAS_MAX      1
#define ADAPT_BIAS_MIN     -1

// Replacement state
static uint8_t   repl_rrpv[LLC_SETS][LLC_WAYS];
static uint16_t  sigtbl[LLC_SETS][LLC_WAYS];
static bool      reused[LLC_SETS][LLC_WAYS];

// Predictor & adaptation
static uint8_t   Predict[PRED_TABLE_SIZE];
static int8_t    adapt_bias;
static uint64_t  epoch_counter;
static uint64_t  epoch_hits;
static uint64_t  epoch_misses;
static double    prev_epoch_hr;

// Statistics
static uint64_t  stat_hits;
static uint64_t  stat_misses;

// Find a victim using standard RRIP mechanism
static uint32_t FindVictimWay(uint32_t set) {
    while (true) {
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            if (repl_rrpv[set][w] == RRPV_MAX)
                return w;
        }
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            if (repl_rrpv[set][w] < RRPV_MAX)
                repl_rrpv[set][w]++;
        }
    }
}

void InitReplacementState() {
    stat_hits        = 0;
    stat_misses      = 0;
    // Initialize cache lines
    for (uint32_t s = 0; s < LLC_SETS; s++) {
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            repl_rrpv[s][w] = RRPV_MAX;
            reused[s][w]    = false;
            sigtbl[s][w]    = 0;
        }
    }
    // Initialize predictor
    for (uint32_t i = 0; i < PRED_TABLE_SIZE; i++)
        Predict[i] = PRED_INIT;
    adapt_bias     = 0;
    epoch_counter  = 0;
    epoch_hits     = 0;
    epoch_misses   = 0;
    prev_epoch_hr  = 0.0;
}

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
    // Update predictor on eviction
    if (sig < PRED_TABLE_SIZE) {
        if (reused[set][victim]) {
            if (Predict[sig] < PRED_MAX) Predict[sig]++;
        } else {
            if (Predict[sig] > 0)         Predict[sig]--;
        }
    }
    // Clear metadata
    reused[set][victim] = false;
    sigtbl[set][victim] = 0;
    return victim;
}

void UpdateReplacementState(
    uint32_t /*cpu*/,
    uint32_t set,
    uint32_t way,
    uint64_t /*paddr*/,
    uint64_t PC,
    uint64_t /*victim_addr*/,
    uint32_t /*type*/,
    uint8_t hit
) {
    if (hit) {
        // Hit path
        stat_hits++;
        epoch_hits++;
        repl_rrpv[set][way] = 0;       // promote to MRU
        reused[set][way]    = true;
    } else {
        // Miss path
        stat_misses++;
        epoch_misses++;
        uint16_t sig = static_cast<uint16_t>(PC) & PRED_MASK;
        sigtbl[set][way] = sig;
        reused[set][way] = false;
        // Fine-grained insertion: map predictor [0..3] -> base RRPV [3..0]
        int base_rrpv = RRPV_MAX - Predict[sig];
        // Apply epoch-based bias
        int new_rrpv = base_rrpv + adapt_bias;
        if (new_rrpv < 0)           new_rrpv = 0;
        if (new_rrpv > RRPV_MAX)    new_rrpv = RRPV_MAX;
        repl_rrpv[set][way] = static_cast<uint8_t>(new_rrpv);
    }
    // Epoch adaptation
    epoch_counter++;
    if (epoch_counter >= EPOCH_INTERVAL) {
        uint64_t total = epoch_hits + epoch_misses;
        double hr = total ? double(epoch_hits)/double(total) : 0.0;
        // If hit-rate fell, become more conservative
        if (hr < prev_epoch_hr) {
            if (adapt_bias < ADAPT_BIAS_MAX) adapt_bias++;
        }
        // If hit-rate rose or stayed, become more aggressive
        else {
            if (adapt_bias > ADAPT_BIAS_MIN) adapt_bias--;
        }
        prev_epoch_hr = hr;
        epoch_counter = 0;
        epoch_hits    = 0;
        epoch_misses  = 0;
    }
}

void PrintStats() {
    uint64_t total = stat_hits + stat_misses;
    double hr = total ? (100.0 * stat_hits / total) : 0.0;
    std::cout << "==== FG-RRIP Policy Statistics ====\n";
    std::cout << "Total refs   : " << total      << "\n";
    std::cout << "Hits         : " << stat_hits  << "\n";
    std::cout << "Misses       : " << stat_misses<< "\n";
    std::cout << "Hit Rate (%) : " << hr         << "\n";
}

void PrintStats_Heartbeat() {
    PrintStats();
}