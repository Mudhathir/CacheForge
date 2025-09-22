#include <vector>
#include <cstdint>
#include <iostream>
#include <iomanip>
#include "../inc/champsim_crc2.h"

#define NUM_CORE       1
#define LLC_SETS       (NUM_CORE * 2048)
#define LLC_WAYS       16

// ---- Tunable parameters ----
static const int PCTR_SIZE       = 2048;        // PC→counter entries
static const int COUNTER_MAX     = 3;           // 2‐bit saturating
static const int THRESHOLD       = 2;           // reuse threshold
static const int LOW_INSERT_POS  = (LLC_WAYS/2); // halfway insertion

// ---- Replacement state ----
static uint8_t   pctr[PCTR_SIZE];               // PC→reuse counter
static uint8_t   lru_stack[LLC_SETS][LLC_WAYS]; // 0=MRU, 15=LRU

// ---- Stats ----
static uint64_t total_accesses       = 0;
static uint64_t total_hits           = 0;
static uint64_t total_misses         = 0;
static uint64_t high_prio_inserts    = 0;
static uint64_t low_prio_inserts     = 0;
static uint64_t pctr_increments      = 0;
static uint64_t pctr_decrements      = 0;

// Helper: reposition the accessed or filled block to insertion_pos
static void ReplaceLRU(int set, int way, int insertion_pos, bool is_miss)
{
    int old_pos = is_miss ? (LLC_WAYS - 1) : lru_stack[set][way];
    int lower   = insertion_pos;
    int upper   = old_pos - 1;
    for (int w = 0; w < LLC_WAYS; w++) {
        int p = lru_stack[set][w];
        if (p >= lower && p <= upper) {
            lru_stack[set][w] = p + 1;
        }
    }
    lru_stack[set][way] = insertion_pos;
}

// Initialize replacement state
void InitReplacementState()
{
    // Initialize LRU stack: way i → position i
    for (int s = 0; s < LLC_SETS; s++) {
        for (int w = 0; w < LLC_WAYS; w++) {
            lru_stack[s][w] = w;
        }
    }
    // Initialize PC reuse counters to threshold (neutral)
    for (int i = 0; i < PCTR_SIZE; i++) {
        pctr[i] = THRESHOLD;
    }
    // Zero stats
    total_accesses    = total_hits    = total_misses = 0;
    high_prio_inserts = low_prio_inserts = 0;
    pctr_increments   = pctr_decrements = 0;
}

// Find victim in the set (evict LRU)
uint32_t GetVictimInSet(
    uint32_t cpu,
    uint32_t set,
    const BLOCK *current_set,
    uint64_t PC,
    uint64_t paddr,
    uint32_t type
) {
    // 1) If any invalid block, use it immediately
    for (uint32_t w = 0; w < LLC_WAYS; w++) {
        if (! current_set[w].valid()) {
            return w;
        }
    }
    // 2) Otherwise evict the LRU block (stack pos == LLC_WAYS-1)
    for (uint32_t w = 0; w < LLC_WAYS; w++) {
        if (lru_stack[set][w] == (uint8_t)(LLC_WAYS - 1)) {
            return w;
        }
    }
    // Fallback
    return 0;
}

// Update replacement state on hit/miss
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
    int idx = ((uint32_t)PC ^ (uint32_t)(paddr >> 6)) & (PCTR_SIZE - 1);

    if (hit) {
        // Hit path
        total_hits++;
        // Update reuse counter
        if (pctr[idx] < COUNTER_MAX) { pctr[idx]++; pctr_increments++; }
        // Always promote to MRU
        ReplaceLRU(set, way, /*insertion_pos=*/0, /*is_miss=*/false);
    } else {
        // Miss (fill) path
        total_misses++;
        // Penalize counter
        if (pctr[idx] > 0) { pctr[idx]--; pctr_decrements++; }
        // Classify and choose insertion depth
        if (pctr[idx] >= THRESHOLD) {
            high_prio_inserts++;
            ReplaceLRU(set, way, /*insertion_pos=*/0, /*is_miss=*/true);
        } else {
            low_prio_inserts++;
            ReplaceLRU(set, way, /*insertion_pos=*/LOW_INSERT_POS, /*is_miss=*/true);
        }
    }
}

// Print end-of-simulation statistics
void PrintStats() {
    std::cout << "===== APIT Replacement Stats =====\n";
    std::cout << "Accesses           : " << total_accesses    << "\n";
    std::cout << "Hits               : " << total_hits        << "\n";
    std::cout << "Misses             : " << total_misses      << "\n";
    std::cout << "Hit Rate           : "
              << std::fixed << std::setprecision(2)
              << (100.0 * total_hits / total_accesses) << "%\n";
    std::cout << "High-prio inserts  : " << high_prio_inserts << "\n";
    std::cout << "Low-prio inserts   : " << low_prio_inserts  << "\n";
    std::cout << "PCTR increments    : " << pctr_increments   << "\n";
    std::cout << "PCTR decrements    : " << pctr_decrements   << "\n";
    std::cout << "==================================\n";
}

// Print periodic (heartbeat) statistics
void PrintStats_Heartbeat() {
    // Optional: print intermediate hit rate
    static uint64_t last_accesses = 0, last_hits = 0;
    uint64_t da = total_accesses - last_accesses;
    uint64_t dh = total_hits - last_hits;
    if (da) {
        std::cout << "[APIT Heartbeat] Recent Hit Rate: "
                  << std::fixed << std::setprecision(2)
                  << (100.0 * dh / da) << "% over " << da << " accesses\n";
    }
    last_accesses = total_accesses;
    last_hits     = total_hits;
}