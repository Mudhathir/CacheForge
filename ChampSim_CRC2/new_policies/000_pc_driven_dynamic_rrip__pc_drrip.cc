#include <vector>
#include <cstdint>
#include <algorithm>
#include <iostream>
#include "../inc/champsim_crc2.h"

#define NUM_CORE 1
#define LLC_SETS (NUM_CORE * 2048)
#define LLC_WAYS 16

// RRIP parameters
static const uint8_t RRPV_BITS = 2;
static const uint8_t RRPV_MAX = (1 << RRPV_BITS) - 1;       // 3
static const uint8_t RRPV_SHIFT = 1;                        // SRRIP inset = RRPV_MAX-1

// PCCT parameters
static const uint32_t PCCT_SIZE = 1024;                     // entries
static const uint8_t PCCT_CTR_BITS = 2;
static const uint8_t PCCT_CTR_MAX = (1 << PCCT_CTR_BITS) - 1; // 3
static const uint8_t PCCT_INIT = PCCT_CTR_MAX / 2;           // start at 1
static const uint8_t PCCT_COLD_THRESHOLD = 1;               // <=1 => cold
static const uint8_t PCCT_HOT_THRESHOLD  = PCCT_CTR_MAX;    // ==3 => hot

// DRRIP set‐dueling
static const uint32_t DRRIP_LEADER_MOD = 32;                // 1/32 of sets
static const int PSEL_BITS = 10;
static const int PSEL_MAX = (1 << PSEL_BITS) - 1;            // 1023
static const int PSEL_INIT = PSEL_MAX / 2;                  // 512
static const int PSEL_THRESHOLD = PSEL_INIT;                // >= => favor SRRIP

// Replacement state per block
struct BlockInfo {
  uint8_t rrpv;
  uint8_t reused;
  uint16_t signature;
};

static BlockInfo repl[LLC_SETS][LLC_WAYS];
static std::vector<uint8_t> PCCT;    // PC‐signature counters
static int PSEL;                     // global policy selector

// Statistics
static uint64_t stat_hits=0, stat_misses=0, stat_psel_inc=0, stat_psel_dec=0;

// Helper: find a victim by SRRIP eviction (RRPV==MAX), incrementing RRPVs if needed
static uint32_t find_victim_way(uint32_t set) {
  // search for RRPV_MAX
  for (;;) {
    for (uint32_t w = 0; w < LLC_WAYS; w++) {
      if (repl[set][w].rrpv == RRPV_MAX) return w;
    }
    // no candidate, age all blocks
    for (uint32_t w = 0; w < LLC_WAYS; w++) {
      if (repl[set][w].rrpv < RRPV_MAX)
        repl[set][w].rrpv++;
    }
  }
}

void InitReplacementState() {
  // initialize per‐block state
  for (uint32_t s = 0; s < LLC_SETS; s++) {
    for (uint32_t w = 0; w < LLC_WAYS; w++) {
      repl[s][w].rrpv = RRPV_MAX;
      repl[s][w].reused = 0;
      repl[s][w].signature = 0;
    }
  }
  // init PCCT
  PCCT.assign(PCCT_SIZE, PCCT_INIT);
  // init PSEL
  PSEL = PSEL_INIT;
}

uint32_t GetVictimInSet(
  uint32_t cpu,
  uint32_t set,
  const BLOCK *current_set,
  uint64_t PC,
  uint64_t paddr,
  uint32_t type
) {
  // Always use SRRIP‐style victim search
  return find_victim_way(set);
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
    // HIT: promote to MRU
    repl[set][way].rrpv = 0;
    repl[set][way].reused = 1;
    stat_hits++;
    return;
  }
  // MISS: we are replacing repl[set][way]
  stat_misses++;
  // 1) Update PSEL if this is a dedicated leader set
  uint32_t m = set % DRRIP_LEADER_MOD;
  if (m == 0) {          // SRRIP leader
    if (PSEL > 0) { PSEL--; stat_psel_dec++; }
  } else if (m == 1) {   // BRRIP leader
    if (PSEL < PSEL_MAX) { PSEL++; stat_psel_inc++; }
  }
  // 2) Update PCCT for the evicted block, using its signature and reused flag
  uint16_t old_sig = repl[set][way].signature;
  if (repl[set][way].reused) {
    // increment toward hot
    if (PCCT[old_sig] < PCCT_CTR_MAX) PCCT[old_sig]++;
  } else {
    // decrement toward cold
    if (PCCT[old_sig] > 0) PCCT[old_sig]--;
  }
  // 3) Classify new block via PC signature
  uint16_t sig = (uint16_t)(PC & (PCCT_SIZE - 1));
  uint8_t ctr = PCCT[sig];
  uint8_t new_rrpv;
  if (ctr >= PCCT_HOT_THRESHOLD) {
    // hot => MRU
    new_rrpv = 0;
  } else if (ctr <= PCCT_COLD_THRESHOLD) {
    // cold => far eviction
    new_rrpv = RRPV_MAX;
  } else {
    // moderate => use DRRIP decision
    if (m == 0) {
      // SRRIP leader
      new_rrpv = RRPV_MAX - 1;
    } else if (m == 1) {
      // BRRIP leader
      new_rrpv = RRPV_MAX;
    } else {
      // follower: consult PSEL
      new_rrpv = (PSEL >= PSEL_THRESHOLD
                  ? (RRPV_MAX - 1)   // favor SRRIP
                  : RRPV_MAX);       // favor BRRIP
    }
  }
  // 4) Install the new block
  repl[set][way].rrpv = new_rrpv;
  repl[set][way].reused = 0;
  repl[set][way].signature = sig;
}

void PrintStats() {
  std::cout << "PC-DRRIP Stats:\n";
  std::cout << "  Hits        = " << stat_hits  << "\n";
  std::cout << "  Misses      = " << stat_misses<< "\n";
  std::cout << "  PSEL++      = " << stat_psel_inc << "\n";
  std::cout << "  PSEL--      = " << stat_psel_dec << "\n";
}

void PrintStats_Heartbeat() {
  // same as full stats for simplicity
  PrintStats();
}