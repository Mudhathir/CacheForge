#include <vector>
#include <cstdint>
#include <iostream>
#include "../inc/champsim_crc2.h"

#define NUM_CORE      1
#define LLC_SETS      (NUM_CORE * 2048)
#define LLC_WAYS      16

// --- Tunable parameters ---
#define RRPV_BITS        2
#define RRPV_MAX         ((1 << RRPV_BITS) - 1)  // 3 for 2‐bit RRPV
#define SHCT_SIZE        1024
#define SHCT_MAX         7
#define SHCT_INIT        4
#define SHCT_THRESHOLD   4
// ---------------------------

// Per‐line replacement state
static uint8_t  repl_rrpv     [LLC_SETS][LLC_WAYS];
static uint16_t repl_sig      [LLC_SETS][LLC_WAYS];
static uint8_t  block_hit_flag[LLC_SETS][LLC_WAYS];
// Signature counter table
static uint8_t  SHCT[SHCT_SIZE];

// Statistics
static uint64_t stat_hits      = 0;
static uint64_t stat_misses    = 0;
static uint64_t stat_bypassed  = 0;

void InitReplacementState() {
  // Initialize per‐line RRPVs, signatures, hit flags
  for (uint32_t s = 0; s < LLC_SETS; s++) {
    for (uint32_t w = 0; w < LLC_WAYS; w++) {
      repl_rrpv[s][w]      = RRPV_MAX;
      repl_sig[s][w]       = 0;
      block_hit_flag[s][w] = 0;
    }
  }
  // Initialize SHCT
  for (uint32_t i = 0; i < SHCT_SIZE; i++) {
    SHCT[i] = SHCT_INIT;
  }
}

uint32_t GetVictimInSet(
    uint32_t cpu,
    uint32_t set,
    const BLOCK *current_set,
    uint64_t PC,
    uint64_t paddr,
    uint32_t type
) {
  // SRRIP victim search: look for RRPV==MAX, else age all and retry
  while (true) {
    for (uint32_t w = 0; w < LLC_WAYS; w++) {
      if (repl_rrpv[set][w] == RRPV_MAX) {
        return w;
      }
    }
    // No candidate: age all entries
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
  if (hit) {
    // On hit: reset RRPV, reinforce signature
    repl_rrpv[set][way] = 0;
    uint16_t sig = repl_sig[set][way];
    if (SHCT[sig] < SHCT_MAX) {
      SHCT[sig]++;
    }
    block_hit_flag[set][way] = 1;
    stat_hits++;
  }
  else {
    // On miss/fill: compute new signature
    uint16_t sig = (uint16_t)((PC ^ (paddr >> 12)) & (SHCT_SIZE - 1));
    repl_sig[set][way] = sig;
    block_hit_flag[set][way] = 0;
    // Predict reuse?
    if (SHCT[sig] >= SHCT_THRESHOLD) {
      // likely reusable → keep
      repl_rrpv[set][way] = 0;
    } else {
      // likely one‐off → bypass
      repl_rrpv[set][way] = RRPV_MAX;
      stat_bypassed++;
    }
    stat_misses++;
  }
}

void PrintStats() {
  uint64_t total = stat_hits + stat_misses;
  double hr = total ? (double)stat_hits / total * 100.0 : 0.0;
  std::cout << "===== SFSRIP Replacement Stats =====\n";
  std::cout << "Total refs     : " << total << "\n";
  std::cout << "Hits           : " << stat_hits << "\n";
  std::cout << "Misses         : " << stat_misses << "\n";
  std::cout << "Bypassed inserts: " << stat_bypassed << "\n";
  std::cout << "Hit rate (%)   : " << hr << "\n";
}

void PrintStats_Heartbeat() {
  PrintStats();
}