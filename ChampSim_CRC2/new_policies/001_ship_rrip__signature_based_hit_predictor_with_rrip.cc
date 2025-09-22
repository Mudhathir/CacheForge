#include <vector>
#include <cstdint>
#include <iostream>
#include "../inc/champsim_crc2.h"

#define NUM_CORE   1
#define LLC_SETS   (NUM_CORE * 2048)
#define LLC_WAYS   16

// SHCT parameters
static const uint32_t SHCT_SIZE       = 1024;  // must be power of two
static const uint8_t  SHCT_MAX        = 7;     
static const uint8_t  SHCT_INIT       = 3;     // initial counter value
static const uint8_t  SIG_USE_THRESH  = 3;     // ≥ ⇒ hot insertion

// Per‐way metadata
struct LineRepl {
    uint8_t  rrpv;        // 0 (hot) .. 3 (cold)
    bool     valid;
    uint32_t signature;   // PC signature
    bool     last_hit;    // true if we've seen a hit since insertion
};

// Global state
static LineRepl repl[NUM_CORE][LLC_SETS][LLC_WAYS];
static uint8_t  SHCT[SHCT_SIZE];
static uint64_t total_hits, total_misses;

// Helpers
inline uint32_t GetSignature(uint64_t PC) {
    // simple PC hash: bits [4..(log2(SHCT_SIZE)+3)]
    return (PC >> 4) & (SHCT_SIZE - 1);
}
inline void SaturatingInc(uint8_t &c) {
    if (c < SHCT_MAX) c++;
}
inline void SaturatingDec(uint8_t &c) {
    if (c > 0) c--;
}

// Initialize replacement state
void InitReplacementState() {
    total_hits   = 0;
    total_misses = 0;
    // Initialize SHCT
    for (uint32_t i = 0; i < SHCT_SIZE; i++) {
        SHCT[i] = SHCT_INIT;
    }
    // Initialize per‐way metadata
    for (uint32_t cpu = 0; cpu < NUM_CORE; cpu++) {
        for (uint32_t s = 0; s < LLC_SETS; s++) {
            for (uint32_t w = 0; w < LLC_WAYS; w++) {
                repl[cpu][s][w].rrpv      = 3; 
                repl[cpu][s][w].valid     = false;
                repl[cpu][s][w].signature = 0;
                repl[cpu][s][w].last_hit  = false;
            }
        }
    }
}

// Find victim in the set (SRRIP)
uint32_t GetVictimInSet(
    uint32_t cpu,
    uint32_t set,
    const BLOCK *current_set,
    uint64_t PC,
    uint64_t paddr,
    uint32_t type
) {
    // Find a line with rrpv == 3, else age all lines and repeat
    while (true) {
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            if (!current_set[w].valid) {
                // empty slot: immediate use
                return w;
            }
            if (repl[cpu][set][w].rrpv == 3) {
                return w;
            }
        }
        // Age all lines
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            repl[cpu][set][w].rrpv = std::min<uint8_t>(3, repl[cpu][set][w].rrpv + 1);
        }
    }
}

// Update replacement state on access/hit/miss
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
        // Hit path: promote and train SHCT
        total_hits++;
        LineRepl &line = repl[cpu][set][way];
        line.last_hit  = true;
        line.rrpv      = 0;  // mark hot
        uint32_t sig   = line.signature;
        SaturatingInc(SHCT[sig]);
        return;
    }

    // Miss path
    total_misses++;
    LineRepl &victim = repl[cpu][set][way];
    // 1) Train SHCT on evicted block if it never saw a hit
    if (victim.valid && !victim.last_hit) {
        SaturatingDec(SHCT[victim.signature]);
    }

    // 2) Insert new block
    uint32_t sig_new        = GetSignature(PC);
    victim.signature        = sig_new;
    victim.last_hit         = false;
    victim.valid            = true;
    // Decide insertion RRPV by SHCT prediction
    if (SHCT[sig_new] >= SIG_USE_THRESH) {
        victim.rrpv = 0;    // likely reused → hot
    } else {
        victim.rrpv = 2;    // likely one‐time → cold
    }
}

// Print end-of-simulation statistics
void PrintStats() {
    double hr = (total_hits + total_misses) 
                ? (double)total_hits / (total_hits + total_misses) * 100.0 
                : 0.0;
    std::cout << "SHiP-RRIP Hits=" << total_hits
              << " Misses=" << total_misses
              << " HitRate=" << hr << "%\n";
}

// Print periodic (heartbeat) statistics
void PrintStats_Heartbeat() {
    double hr = (total_hits + total_misses) 
                ? (double)total_hits / (total_hits + total_misses) * 100.0 
                : 0.0;
    std::cout << "[Heartbeat] SHiP-RRIP HitRate=" << hr << "%\n";
}