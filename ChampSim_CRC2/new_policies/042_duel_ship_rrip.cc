#include <vector>
#include <cstdint>
#include <iostream>
#include "../inc/champsim_crc2.h"

#define NUM_CORE           1
#define LLC_SETS           (NUM_CORE * 2048)
#define LLC_WAYS           16

// RRIP parameters
#define RRPV_BITS          2
#define RRPV_MAX           ((1 << RRPV_BITS) - 1)

// Set‐dueling parameters
#define DUEL_INTERVAL      64      // must be power‐of‐two
#define BRRIP_SAMPLE_TH    16      // [0..15] => BRRIP sample
#define SHIP_SAMPLE_TH     32      // [16..31] => SHiP sample; [32..63] => follower

// BRRIP (bimodal RRIP) insertion rate
#define BRRIP_RATE         32      // 1/32 insert near‐MRU (RRPV_MAX-1), else far (RRPV_MAX)

// SHiP predictor parameters
#define SHCT_BITS          3
#define SHCT_SIZE          32768
#define SHCT_MASK          (SHCT_SIZE - 1)
#define SHCT_MAX           ((1 << SHCT_BITS) - 1)
#define SHCT_INIT          (SHCT_MAX >> 1)
#define SHCT_THRESHOLD     (SHCT_MAX >> 1)  // >= threshold ⇒ predict reuse

// PSEL counter for set dueling
#define PSEL_BITS          10
#define PSEL_MAX           ((1 << PSEL_BITS) - 1)
#define PSEL_INIT          (PSEL_MAX >> 1)

static uint8_t   repl_rrpv[LLC_SETS][LLC_WAYS];
static bool      reused[LLC_SETS][LLC_WAYS];
static uint16_t  sigtbl[LLC_SETS][LLC_WAYS];
static uint8_t   SHCT[SHCT_SIZE];
static uint16_t  PSEL;
static uint64_t  fill_counter;
static uint64_t  stat_hits;
static uint64_t  stat_misses;

// Standard RRIP victim selection
static uint32_t FindVictimWay(uint32_t set) {
    while (true) {
        // look for RRPV == max
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            if (repl_rrpv[set][w] == RRPV_MAX)
                return w;
        }
        // age all entries
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            if (repl_rrpv[set][w] < RRPV_MAX)
                repl_rrpv[set][w]++;
        }
    }
}

void InitReplacementState() {
    stat_hits     = 0;
    stat_misses   = 0;
    PSEL          = PSEL_INIT;
    fill_counter  = 0;
    // initialize per‐line state
    for (uint32_t s = 0; s < LLC_SETS; s++) {
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            repl_rrpv[s][w] = RRPV_MAX;
            reused[s][w]    = false;
            sigtbl[s][w]    = 0;
        }
    }
    // initialize SHCT to neutral
    for (uint32_t i = 0; i < SHCT_SIZE; i++) {
        SHCT[i] = SHCT_INIT;
    }
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
    // Update SHCT on eviction
    if (sig < SHCT_SIZE) {
        if (reused[set][victim]) {
            if (SHCT[sig] < SHCT_MAX) SHCT[sig]++;
        } else {
            if (SHCT[sig] > 0) SHCT[sig]--;
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
        // On hit: mark MRU and record reuse
        stat_hits++;
        repl_rrpv[set][way] = 0;
        reused[set][way]    = true;
    } else {
        // On miss: choose insertion by set‐dueling
        stat_misses++;
        fill_counter++;
        // classify set
        uint32_t m = set & (DUEL_INTERVAL - 1);
        bool     is_brrip_sample = (m < BRRIP_SAMPLE_TH);
        bool     is_ship_sample  = (m >= BRRIP_SAMPLE_TH && m < SHIP_SAMPLE_TH);
        bool     use_brrip;
        if (is_brrip_sample) {
            use_brrip = true;
            // BRRIP-sample miss ⇒ punish BRRIP
            if (PSEL > 0) PSEL--;
        } else if (is_ship_sample) {
            use_brrip = false;
            // SHiP-sample miss ⇒ punish SHiP (i.e., favor BRRIP)
            if (PSEL < PSEL_MAX) PSEL++;
        } else {
            // follower sets follow PSEL
            use_brrip = (PSEL > (PSEL_MAX >> 1));
        }
        if (!use_brrip) {
            // SHiP insertion
            uint16_t sig = static_cast<uint16_t>(PC) & SHCT_MASK;
            sigtbl[set][way]  = sig;
            reused[set][way]  = false;
            // predicted reuse?
            if (SHCT[sig] >= SHCT_THRESHOLD) {
                repl_rrpv[set][way] = RRPV_MAX - 1;  // near‐MRU
            } else {
                repl_rrpv[set][way] = RRPV_MAX;      // far‐MRU
            }
        } else {
            // BRRIP insertion
            sigtbl[set][way] = 0;
            reused[set][way] = false;
            // 1/BRRIP_RATE get near‐MRU, else far‐MRU
            if ((fill_counter & (BRRIP_RATE - 1)) == 0) {
                repl_rrpv[set][way] = RRPV_MAX - 1;
            } else {
                repl_rrpv[set][way] = RRPV_MAX;
            }
        }
    }
}

void PrintStats() {
    uint64_t total = stat_hits + stat_misses;
    double   hr    = total ? (100.0 * stat_hits / total) : 0.0;
    std::cout << "==== Duel-SHiP-RRIP Policy Statistics ====\n";
    std::cout << "Total refs   : " << total     << "\n";
    std::cout << "Hits         : " << stat_hits << "\n";
    std::cout << "Misses       : " << stat_misses << "\n";
    std::cout << "Hit Rate (%) : " << hr        << "\n";
}

void PrintStats_Heartbeat() {
    PrintStats();
}