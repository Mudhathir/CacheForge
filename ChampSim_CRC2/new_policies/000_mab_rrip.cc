#include <vector>
#include <cstdint>
#include <cmath>
#include <iostream>
#include "../inc/champsim_crc2.h"

#define NUM_CORE      1
#define LLC_SETS      (NUM_CORE * 2048)
#define LLC_WAYS      16

// RRIP parameters
#define RRPV_BITS     2
#define RRPV_MAX      ((1 << RRPV_BITS) - 1)
#define LRU_RRPV      0
#define SRRIP_RRPV    1
#define BIP_RRPV      RRPV_MAX

// Multi-armed bandit parameters
#define NUM_POLICIES  3   // 0=LRU,1=SRRIP,2=BIP
#define UCB_C         1.0 // exploration constant

// Replacement state per block
static uint8_t RRPV_table  [LLC_SETS][LLC_WAYS];
static uint8_t policy_table[LLC_SETS][LLC_WAYS];

// Bandit statistics per set
static uint16_t select_count[LLC_SETS][NUM_POLICIES];
static uint16_t hit_count_arr[LLC_SETS][NUM_POLICIES];

// Initialize replacement state
void InitReplacementState() {
  for (uint32_t set = 0; set < LLC_SETS; ++set) {
    for (uint32_t way = 0; way < LLC_WAYS; ++way) {
      RRPV_table[set][way]   = RRPV_MAX;
      policy_table[set][way] = 1; // default to SRRIP arm
    }
    for (int p = 0; p < NUM_POLICIES; ++p) {
      select_count[set][p] = 0;
      hit_count_arr[set][p] = 0;
    }
  }
}

// UCB1-based policy selector for a given set
static uint32_t select_policy(uint32_t set) {
  uint32_t total = select_count[set][0]
                 + select_count[set][1]
                 + select_count[set][2];
  // Ensure each arm is tried once
  for (uint32_t p = 0; p < NUM_POLICIES; ++p) {
    if (select_count[set][p] == 0) return p;
  }
  double best_ucb = -1e9;
  uint32_t best_p = 0;
  for (uint32_t p = 0; p < NUM_POLICIES; ++p) {
    double mean = (double)hit_count_arr[set][p] / select_count[set][p];
    double ucb  = mean + UCB_C * std::sqrt(std::log((double)total) / select_count[set][p]);
    if (ucb > best_ucb) {
      best_ucb = ucb;
      best_p   = p;
    }
  }
  return best_p;
}

// Find victim via standard SRRIP aging
uint32_t GetVictimInSet(
    uint32_t cpu,
    uint32_t set,
    const BLOCK *current_set,
    uint64_t PC,
    uint64_t paddr,
    uint32_t type
) {
  while (true) {
    // Look for an RRPV == MAX block
    for (uint32_t way = 0; way < LLC_WAYS; ++way) {
      if (RRPV_table[set][way] == RRPV_MAX) {
        return way;
      }
    }
    // Age all RRPVs
    for (uint32_t way = 0; way < LLC_WAYS; ++way) {
      if (RRPV_table[set][way] < RRPV_MAX)
        ++RRPV_table[set][way];
    }
  }
}

// Update on hit or fill
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
    // On a hit: always promote to MRU
    RRPV_table[set][way] = LRU_RRPV;
    // Credit the policy that inserted this block
    uint32_t p = policy_table[set][way];
    hit_count_arr[set][p]++;
  } else {
    // On a miss+fill: select an arm and insert accordingly
    uint32_t p = select_policy(set);
    select_count[set][p]++;
    policy_table[set][way] = p;
    switch (p) {
      case 0: RRPV_table[set][way] = LRU_RRPV;   break; // LRU insertion
      case 1: RRPV_table[set][way] = SRRIP_RRPV; break; // SRRIP insertion
      case 2: RRPV_table[set][way] = BIP_RRPV;   break; // BIP insertion
    }
  }
}

// Print end-of-simulation statistics
void PrintStats() {
  uint64_t total_sel[NUM_POLICIES] = {0}, total_hits[NUM_POLICIES] = {0};
  for (uint32_t s = 0; s < LLC_SETS; ++s) {
    for (int p = 0; p < NUM_POLICIES; ++p) {
      total_sel[p]  += select_count[s][p];
      total_hits[p] += hit_count_arr[s][p];
    }
  }
  std::cout << "==== MAB-RRIP Stats ====\n";
  const char *names[NUM_POLICIES] = {"LRU", "SRRIP", "BIP"};
  for (int p = 0; p < NUM_POLICIES; ++p) {
    double hr = total_sel[p] ? (double)total_hits[p] / total_sel[p] : 0.0;
    std::cout << names[p]
              << " selected=" << total_sel[p]
              << " hits="    << total_hits[p]
              << " hr="      << hr << "\n";
  }
}

// Heartbeat stats (optional)
void PrintStats_Heartbeat() {
  // Could print intermediate bandit convergence info
}