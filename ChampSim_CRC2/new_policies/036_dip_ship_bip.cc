#include <vector>
#include <cstdint>
#include <iostream>
#include <cstdlib>
#include "../inc/champsim_crc2.h"

#define NUM_CORE               1
#define LLC_SETS               (NUM_CORE * 2048)
#define LLC_WAYS               16

// --- RRIP parameters ---
#define RRPV_BITS              2
#define RRPV_MAX               ((1 << RRPV_BITS) - 1)   // 3

// --- SHiP signature table parameters ---
#define SIG_BITS               14
#define SIG_SIZE               (1 << SIG_BITS)
#define SIG_MASK               (SIG_SIZE - 1)
#define CNTR_BITS              4
#define CNTR_MAX               ((1 << CNTR_BITS) - 1)   // 15
// 4 bands: [0..TH1-1], [TH1..TH2-1], [TH2..TH3-1], [TH3..CNTR_MAX]
#define TH1                    ((CNTR_MAX >> 2) + 1)    // ≈4
#define TH2                    ((CNTR_MAX >> 1) + 1)    // ≈8
#define TH3                    (((CNTR_MAX * 3) >> 2) + 1) // ≈12

// --- DIP parameters ---
#define DUELING_SETS           64
#define LEADER_SHIP_SETS       32
#define LEADER_BIP_SETS        32
#define DUELING_PERIOD         (LLC_SETS / DUELING_SETS) // 2048/64=32
#define PSEL_BITS              10
#define PSEL_MAX               ((1 << PSEL_BITS) - 1)   // 1023
#define PSEL_THRESHOLD         (PSEL_MAX >> 1)          // 511

// --- BIP (BRRIP) probability ---
#define BIP_PROB               32    // 1/32 insert near-MRU, else long

// Replacement state
static uint8_t  repl_rrpv   [LLC_SETS][LLC_WAYS];
static uint8_t  sig_table   [SIG_SIZE];               // 4-bit saturating counters
static uint16_t block_sig   [LLC_SETS][LLC_WAYS];     // stored signature index
static bool     block_refd  [LLC_SETS][LLC_WAYS];     // referenced since insertion
static bool     block_valid [LLC_SETS][LLC_WAYS];     // valid for SHiP feedback
static uint32_t stat_hits, stat_misses;
static uint16_t psel;                                  // DIP preference counter

void InitReplacementState() {
    stat_hits   = 0;
    stat_misses = 0;
    // Initialize RRIP state and SHiP metadata
    for (int s = 0; s < LLC_SETS; s++) {
        for (int w = 0; w < LLC_WAYS; w++) {
            repl_rrpv[s][w]   = RRPV_MAX;
            block_sig[s][w]   = 0;
            block_refd[s][w]  = false;
            block_valid[s][w] = false;
        }
    }
    // Initialize all SHiP signatures to "medium reuse"
    for (int i = 0; i < SIG_SIZE; i++) {
        sig_table[i] = TH2;
    }
    // Initialize DIP selector to neutral
    psel = PSEL_THRESHOLD;
    // Seed RNG for BIP
    std::srand(0xC0FFEE);
}

static inline bool IsLeaderShipSet(uint32_t set) {
    // sample every DUELING_PERIOD-th set
    if ((set % DUELING_PERIOD) != 0) return false;
    uint32_t idx = set / DUELING_PERIOD; // [0..DUELING_SETS-1]
    return (idx < LEADER_SHIP_SETS);
}
static inline bool IsLeaderBipSet(uint32_t set) {
    if ((set % DUELING_PERIOD) != 0) return false;
    uint32_t idx = set / DUELING_PERIOD;
    return (idx >= LEADER_SHIP_SETS && idx < (LEADER_SHIP_SETS + LEADER_BIP_SETS));
}

// Standard RRIP victim selection
uint32_t GetVictimInSet(
    uint32_t /*cpu*/,
    uint32_t set,
    const BLOCK */*current_set*/,
    uint64_t /*PC*/,
    uint64_t /*paddr*/,
    uint32_t /*type*/
) {
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

// Update on hit or miss+install
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
    // On hit: MRU both for SHiP/BIP
    if (hit) {
        stat_hits++;
        repl_rrpv[set][way]  = 0;
        block_refd[set][way] = true;
        return;
    }

    // On miss/install
    stat_misses++;

    bool is_leader_ship = IsLeaderShipSet(set);
    bool is_leader_bip  = IsLeaderBipSet(set);
    bool use_ship;

    // 1) If we're in a leader set, update PSEL based on the losing policy
    if (is_leader_ship) {
        use_ship = true;
    } else if (is_leader_bip) {
        use_ship = false;
    } else {
        // follower: pick based on PSEL
        use_ship = (psel >= PSEL_THRESHOLD);
    }
    // Update PSEL on misses in the sample sets
    if (is_leader_ship) {
        // ship missed → it might be worse
        if (psel > 0) psel--;
    } else if (is_leader_bip) {
        // bip missed → ship may be better
        if (psel < PSEL_MAX) psel++;
    }

    // 2) Feedback to SHiP signatures for the evicted block (if any)
    if (block_valid[set][way]) {
        uint16_t old_sig = block_sig[set][way];
        if (block_refd[set][way]) {
            // useful → increment
            if (sig_table[old_sig] < CNTR_MAX)
                sig_table[old_sig]++;
        } else {
            // dead → decrement
            if (sig_table[old_sig] > 0)
                sig_table[old_sig]--;
        }
    }

    // 3) Insert new block under chosen policy
    if (use_ship) {
        // SHiP insertion
        uint16_t sig = (PC >> 2) & SIG_MASK;
        uint8_t ctr = sig_table[sig];
        if (ctr < TH1)             repl_rrpv[set][way] = RRPV_MAX;
        else if (ctr < TH2)        repl_rrpv[set][way] = RRPV_MAX - 1;
        else if (ctr < TH3)        repl_rrpv[set][way] = RRPV_MAX - 2;
        else                        repl_rrpv[set][way] = 0;
        block_sig[set][way]   = sig;
        block_refd[set][way]  = false;
        block_valid[set][way] = true;
    } else {
        // Bimodal insertion (BRRIP)
        // Most likely long (RRPV_MAX), rarely near-MRU (RRPV_MAX-1)
        if ((std::rand() & (BIP_PROB - 1)) == 0)
            repl_rrpv[set][way] = RRPV_MAX - 1;
        else
            repl_rrpv[set][way] = RRPV_MAX;
        // Disable SHiP feedback
        block_valid[set][way] = false;
    }
}

void PrintStats() {
    uint64_t total = stat_hits + stat_misses;
    double   hr    = total ? (100.0 * stat_hits / total) : 0.0;
    std::cout << "==== DIP-SHiP-BIP Policy Statistics ====\n";
    std::cout << "PSEL            : " << psel        << "\n";
    std::cout << "Total refs      : " << total       << "\n";
    std::cout << "Hits            : " << stat_hits   << "\n";
    std::cout << "Misses          : " << stat_misses << "\n";
    std::cout << "Hit Rate (%)    : " << hr          << "\n";
}

void PrintStats_Heartbeat() {
    PrintStats();
}