#include <cstdint>
#include <iostream>
#include "../inc/champsim_crc2.h"

#define NUM_CORE             1
#define LLC_SETS             (NUM_CORE * 2048)
#define LLC_WAYS             16

// RRIP parameters
#define RRPV_BITS            3
#define RRPV_MAX             ((1 << RRPV_BITS) - 1)  // 7
#define SRRIP_INIT           (RRPV_MAX - 1)          // 6

// Epoch‐based adaptation
#define EPOCH_LEN            500000ULL
#define HIGH_HR_THRESHOLD    0.50    // if hit rate > 50% → MRU
#define HIGH_THRASH_THRESHOLD 0.60   // if thrash rate > 60% → BYPASS

// Replacement state arrays
static uint8_t  repl_rrpv    [LLC_SETS][LLC_WAYS];
static bool     repl_has_hit [LLC_SETS][LLC_WAYS];

// Insertion policy states
enum InsertionState { INSERT_MRU, INSERT_BYPASS, INSERT_SRRIP };
static InsertionState insertion_state;

// Statistics
static uint64_t stat_hits, stat_misses, stat_evictions;
static uint64_t epoch_events, epoch_hits, epoch_misses, epoch_evictions, epoch_thrashes;

void InitReplacementState() {
  // Initialize RRIP values and flags
  for (uint32_t s = 0; s < LLC_SETS; s++) {
    for (uint32_t w = 0; w < LLC_WAYS; w++) {
      repl_rrpv[s][w]    = RRPV_MAX;
      repl_has_hit[s][w] = false;
    }
  }
  // Start in SRRIP mode
  insertion_state = INSERT_SRRIP;
  // Reset global stats
  stat_hits      = stat_misses = stat_evictions = 0;
  epoch_events   = epoch_hits   = epoch_misses     = 0;
  epoch_evictions= epoch_thrashes = 0;
}

uint32_t GetVictimInSet(
    uint32_t cpu,
    uint32_t set,
    const BLOCK *current_set,
    uint64_t PC,
    uint64_t paddr,
    uint32_t type
) {
  // Standard RRIP victim selection: look for RRPV==MAX, otherwise age
  while (true) {
    for (uint32_t w = 0; w < LLC_WAYS; w++) {
      if (repl_rrpv[set][w] == RRPV_MAX) {
        return w;
      }
    }
    for (uint32_t w = 0; w < LLC_WAYS; w++) {
      if (repl_rrpv[set][w] < RRPV_MAX) {
        repl_rrpv[set][w]++;
      }
    }
  }
  // unreachable
  return 0;
}

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
  // Count every access in the epoch
  epoch_events++;

  if (hit) {
    // HIT: promote to MRU
    stat_hits++;
    epoch_hits++;
    repl_rrpv[set][way]    = 0;
    repl_has_hit[set][way] = true;
  } else {
    // MISS: eviction + insertion
    stat_misses++;
    stat_evictions++;
    epoch_misses++;
    epoch_evictions++;
    // Track thrash if the victim block never got a hit
    if (!repl_has_hit[set][way]) {
      epoch_thrashes++;
    }
    // Reset hit flag for the new block
    repl_has_hit[set][way] = false;
    // Choose insertion RRPV based on the current state
    switch (insertion_state) {
      case INSERT_MRU:
        repl_rrpv[set][way] = 0;
        break;
      case INSERT_BYPASS:
        repl_rrpv[set][way] = RRPV_MAX;
        break;
      case INSERT_SRRIP:
      default:
        repl_rrpv[set][way] = SRRIP_INIT;
        break;
    }
  }

  // At the end of an epoch, re‐evaluate insertion policy
  if (epoch_events >= EPOCH_LEN) {
    double hr          = double(epoch_hits)      / double(epoch_events);
    double thrash_rate = (epoch_evictions > 0)
                        ? double(epoch_thrashes) / double(epoch_evictions)
                        : 0.0;
    if (thrash_rate > HIGH_THRASH_THRESHOLD) {
      insertion_state = INSERT_BYPASS;
    }
    else if (hr > HIGH_HR_THRESHOLD) {
      insertion_state = INSERT_MRU;
    }
    else {
      insertion_state = INSERT_SRRIP;
    }
    // Reset epoch counters
    epoch_events    = epoch_hits    = epoch_misses    =
    epoch_evictions = epoch_thrashes= 0;
  }
}

void PrintStats() {
  uint64_t total = stat_hits + stat_misses;
  double   hr    = total ? double(stat_hits) / double(total) * 100.0 : 0.0;
  const char* state_str =
    (insertion_state == INSERT_MRU)    ? "MRU" :
    (insertion_state == INSERT_BYPASS) ? "BYPASS" : "SRRIP";
  std::cout << "==== AEI Statistics ====\n";
  std::cout << "Total refs      : " << total        << "\n";
  std::cout << "Hits            : " << stat_hits    << "\n";
  std::cout << "Misses          : " << stat_misses  << "\n";
  std::cout << "Evictions       : " << stat_evictions << "\n";
  std::cout << "Hit rate (%)    : " << hr           << "\n";
  std::cout << "Final state     : " << state_str    << "\n";
}

void PrintStats_Heartbeat() {
  PrintStats();
}