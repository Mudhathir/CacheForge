#include <vector>
#include <cstdint>
#include <iostream>
#include "../inc/champsim_crc2.h"

#define NUM_CORE        1
#define LLC_SETS        (NUM_CORE * 2048)
#define LLC_WAYS        16

// Tunable parameters
#define SHCT_SIZE       4096         // Signature table entries (power of 2)
#define SHCT_CTR_MAX    3            // 2-bit saturating
#define SHCT_INIT       2            // Initial counter value
#define SHCT_THRESHOLD  2            // >=2 means “high reuse”
#define PSEL_BITS       10
#define PSEL_MAX        ((1 << PSEL_BITS) - 1)
#define BIP_EPSILON     32           // 1/32 chance of MRU in BIP

// Replacement state
static uint8_t  RRPV   [LLC_SETS][LLC_WAYS];
static bool     reuse  [LLC_SETS][LLC_WAYS];
static uint16_t sigtab [LLC_SETS][LLC_WAYS];
static uint8_t  SHCT   [SHCT_SIZE];
static uint16_t PSEL;
static bool     is_ship_leader[LLC_SETS];
static bool     is_bip_leader [LLC_SETS];

// Simple PC → signature hash
static inline uint32_t MakeSignature(uint64_t PC) {
    return (uint32_t)((PC ^ (PC >> 12)) & (SHCT_SIZE - 1));
}

void InitReplacementState() {
    // Initialize per‐block state
    for (uint32_t set = 0; set < LLC_SETS; set++) {
        for (uint32_t way = 0; way < LLC_WAYS; way++) {
            RRPV[set][way]   = SHCT_CTR_MAX;  // “far” re-reference
            reuse [set][way] = false;
            sigtab[set][way] = 0;
        }
        // Carve out ~1.5% of sets as SHiP leaders, next 1.5% as BIP leaders
        uint32_t block = LLC_SETS / 64;
        is_ship_leader[set] = (set <  block);
        is_bip_leader [set] = (set >= block && set < 2*block);
    }
    // Initialize global selectors and signature table
    PSEL = PSEL_MAX / 2;
    for (uint32_t i = 0; i < SHCT_SIZE; i++)
        SHCT[i] = SHCT_INIT;
}

uint32_t GetVictimInSet(
    uint32_t cpu, uint32_t set,
    const BLOCK *current_set,
    uint64_t PC, uint64_t paddr, uint32_t type
) {
    // Standard RRIP victim selection: evict any with RRPV == MAX,
    // otherwise age all lines by +1 and repeat.
    while (true) {
        for (uint32_t way = 0; way < LLC_WAYS; way++) {
            if (RRPV[set][way] == SHCT_CTR_MAX)
                return way;
        }
        for (uint32_t way = 0; way < LLC_WAYS; way++) {
            if (RRPV[set][way] < SHCT_CTR_MAX)
                RRPV[set][way]++;
        }
    }
    // unreachable
    return 0;
}

void UpdateReplacementState(
    uint32_t cpu, uint32_t set, uint32_t way,
    uint64_t paddr, uint64_t PC, uint64_t victim_addr,
    uint32_t type, uint8_t hit
) {
    if (hit) {
        // On a hit: promote to MRU
        RRPV[set][way]   = 0;
        reuse [set][way] = true;
        return;
    }
    // On miss → fill. First, train SHCT on the evicted block
    uint32_t oldsig = sigtab[set][way];
    if (reuse[set][way]) {
        if (SHCT[oldsig] < SHCT_CTR_MAX) SHCT[oldsig]++;
    } else {
        if (SHCT[oldsig] > 0)          SHCT[oldsig]--;
    }
    // If this set is a leader, punish the losing policy
    if (is_ship_leader[set]) {
        // SHiP leader missed → punish SHiP → move PSEL toward 0 (favor BIP)
        if (PSEL > 0) PSEL--;
    } else if (is_bip_leader[set]) {
        // BIP leader missed → punish BIP → move PSEL toward max (favor SHiP)
        if (PSEL < PSEL_MAX) PSEL++;
    }
    // Compute the signature for the new block
    uint32_t newsig = MakeSignature(PC);
    sigtab[set][way] = newsig;
    reuse [set][way] = false;
    // Decide insertion policy for this set
    bool use_ship;
    if      (is_ship_leader[set]) use_ship = true;
    else if (is_bip_leader [set]) use_ship = false;
    else                          use_ship = (PSEL > (PSEL_MAX/2));
    // Perform insertion
    if (use_ship) {
        // SHiP: if signature predicts high reuse → MRU, else far
        if (SHCT[newsig] >= SHCT_THRESHOLD)
            RRPV[set][way] = 0;
        else
            RRPV[set][way] = SHCT_CTR_MAX;
    } else {
        // BIP: mostly far, with rare MRU to capture occasional reuse
        if ((PC & (BIP_EPSILON - 1)) == 0)
            RRPV[set][way] = 0;
        else
            RRPV[set][way] = SHCT_CTR_MAX;
    }
}

void PrintStats() {
    std::cout << "### SigDIP-RRIP Statistics ###\n";
    std::cout << "  PSEL = " << PSEL << "\n";
}

void PrintStats_Heartbeat() {
    // Optional periodic reporting
    std::cout << "[Heartbeat] SigDIP-RRIP PSEL=" << PSEL << std::endl;
}