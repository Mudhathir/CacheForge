#include <vector>
#include <cstdint>
#include <iostream>
#include "../inc/champsim_crc2.h"

#define NUM_CORE      1
#define LLC_SETS      (NUM_CORE * 2048)
#define LLC_WAYS      16

// SRRIP parameters
#define RRPV_BITS     2
#define RRPV_MAX      ((1 << RRPV_BITS) - 1)   // 3

// TinyLRU filter parameters
#define TINY_SIZE     64

// Per‐line RRPV
static uint8_t repl_rrpv     [LLC_SETS][LLC_WAYS];

// TinyLRU filter state
static uint8_t  tiny_valid   [TINY_SIZE];
static uint8_t  tiny_recency [TINY_SIZE];
static uint64_t tiny_tag     [TINY_SIZE];

// Statistics
static uint64_t stat_hits      = 0;
static uint64_t stat_misses    = 0;
static uint64_t stat_evictions = 0;
static uint64_t tiny_hits      = 0;
static uint64_t tiny_misses    = 0;

// Initialize replacement and filter state
void InitReplacementState() {
    // Initialize RRPVs
    for (uint32_t s = 0; s < LLC_SETS; s++) {
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            repl_rrpv[s][w] = RRPV_MAX;
        }
    }
    // Initialize TinyLRU
    for (int i = 0; i < TINY_SIZE; i++) {
        tiny_valid[i]   = 0;
        tiny_recency[i] = 0;
        tiny_tag[i]     = 0;
    }
    // Clear stats
    stat_hits = stat_misses = stat_evictions = tiny_hits = tiny_misses = 0;
}

// TinyLRU access: returns true if hit, false if miss (and updates filter)
static bool TinyLRU_Access(uint64_t line) {
    int hit_idx = -1;
    // Probe for hit
    for (int i = 0; i < TINY_SIZE; i++) {
        if (tiny_valid[i] && tiny_tag[i] == line) {
            hit_idx = i;
            break;
        }
    }
    if (hit_idx >= 0) {
        // Promote in LRU recency
        uint8_t old = tiny_recency[hit_idx];
        for (int j = 0; j < TINY_SIZE; j++) {
            if (tiny_valid[j] && tiny_recency[j] > old)
                tiny_recency[j]--;
        }
        tiny_recency[hit_idx] = TINY_SIZE - 1;
        tiny_hits++;
        return true;
    } else {
        // Allocate or replace LRU in TinyLRU
        int insert_idx = -1;
        for (int j = 0; j < TINY_SIZE; j++) {
            if (!tiny_valid[j]) {
                insert_idx = j;
                break;
            }
        }
        if (insert_idx < 0) {
            // Find LRU (min recency)
            uint8_t minv = 0xFF;
            int     min_i = 0;
            for (int j = 0; j < TINY_SIZE; j++) {
                if (tiny_recency[j] < minv) {
                    minv = tiny_recency[j];
                    min_i = j;
                }
            }
            insert_idx = min_i;
        }
        // Install new entry
        tiny_valid[insert_idx]   = 1;
        tiny_tag[insert_idx]     = line;
        // Age others
        for (int j = 0; j < TINY_SIZE; j++) {
            if (tiny_valid[j] && j != insert_idx)
                tiny_recency[j]--;
        }
        tiny_recency[insert_idx] = TINY_SIZE - 1;
        tiny_misses++;
        return false;
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
    // Find a way with RRPV == MAX; if none, age all and retry
    while (true) {
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            if (repl_rrpv[set][w] == RRPV_MAX) {
                stat_evictions++;
                return w;
            }
        }
        // Age all
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            if (repl_rrpv[set][w] < RRPV_MAX) {
                repl_rrpv[set][w]++;
            }
        }
    }
    // Unreachable
    return 0;
}

// Update on hit or miss
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
        // LLC hit: rejuvenate
        repl_rrpv[set][way] = 0;
        stat_hits++;
    } else {
        // LLC miss & fill
        stat_misses++;
        // Line identifier (64B blocks)
        uint64_t line = paddr >> 6;
        // Probe TinyLRU to decide hot vs cold
        bool reuse = TinyLRU_Access(line);
        if (reuse) {
            // Proven reuse: insert hot
            repl_rrpv[set][way] = 0;
        } else {
            // Cold or streaming: insert at MAX (bypass‐like)
            repl_rrpv[set][way] = RRPV_MAX;
        }
    }
}

// Print final statistics
void PrintStats() {
    uint64_t total = stat_hits + stat_misses;
    double   hr    = total ? (double)stat_hits / total * 100.0 : 0.0;
    std::cout << "==== TinySRRIP Replacement Stats ====\n";
    std::cout << "Total refs   : " << total       << "\n";
    std::cout << "Hits         : " << stat_hits   << "\n";
    std::cout << "Misses       : " << stat_misses << "\n";
    std::cout << "Evictions    : " << stat_evictions << "\n";
    std::cout << "Hit rate (%) : " << hr << "\n";
    std::cout << "---- TinyLRU Filter ----\n";
    std::cout << "Tiny hits    : " << tiny_hits   << "\n";
    std::cout << "Tiny misses  : " << tiny_misses << "\n";
}

// Periodic (heartbeat) stats
void PrintStats_Heartbeat() {
    PrintStats();
}