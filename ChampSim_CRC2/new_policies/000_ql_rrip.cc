#include <vector>
#include <cstdint>
#include <iostream>
#include <cstdlib>
#include "../inc/champsim_crc2.h"

#define NUM_CORE    1
#define LLC_SETS    (NUM_CORE * 2048)
#define LLC_WAYS    16

// RRIP parameters
#define RRPV_BITS   2
#define MAX_RRPV    ((1 << RRPV_BITS) - 1)

// Q-learning parameters
static constexpr int SIG_COUNT = 1024;    // Number of PC signatures
static constexpr int ACTIONS   = 3;       // 0: RRPV=0, 1: RRPV=MAX-1, 2: RRPV=MAX
static constexpr float ALPHA   = 0.1f;    // Learning rate
static constexpr float GAMMA   = 0.0f;    // No lookahead (can tune up to 0.9)
static constexpr float EPSILON = 0.1f;    // Exploration probability

// Replacement state
static uint8_t  rrpv   [NUM_CORE][LLC_SETS][LLC_WAYS];
static bool     valid  [NUM_CORE][LLC_SETS][LLC_WAYS];
static uint16_t sig    [NUM_CORE][LLC_SETS][LLC_WAYS];
static uint8_t  action_meta[NUM_CORE][LLC_SETS][LLC_WAYS];
static bool     hit_flag[NUM_CORE][LLC_SETS][LLC_WAYS];

// Q-table and statistics
static float    Q_table[SIG_COUNT][ACTIONS];
static uint64_t total_accesses = 0, hit_accesses = 0, miss_accesses = 0, q_updates = 0;

// Simple PC → signature hash
static inline int GetSignature(uint64_t PC) {
    return int(((PC >> 2) ^ (PC >> 12)) & (SIG_COUNT - 1));
}

void InitReplacementState() {
    std::srand(42);
    for (int c = 0; c < NUM_CORE; c++) {
        for (int s = 0; s < LLC_SETS; s++) {
            for (int w = 0; w < LLC_WAYS; w++) {
                rrpv[c][s][w]      = MAX_RRPV;
                valid[c][s][w]     = false;
                hit_flag[c][s][w]  = false;
                sig[c][s][w]       = 0;
                action_meta[c][s][w] = 0;
            }
        }
    }
    // Zero Q-table
    for (int i = 0; i < SIG_COUNT; i++)
      for (int a = 0; a < ACTIONS; a++)
        Q_table[i][a] = 0.0f;
}

uint32_t GetVictimInSet(
    uint32_t cpu,
    uint32_t set,
    const BLOCK *current_set,
    uint64_t PC,
    uint64_t paddr,
    uint32_t type
) {
    // 1) If there's an invalid way, take it
    for (uint32_t w = 0; w < LLC_WAYS; w++) {
        if (!valid[cpu][set][w]) {
            return w;
        }
    }
    // 2) Standard SRRIP victim selection
    while (true) {
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            if (rrpv[cpu][set][w] == MAX_RRPV) {
                return w;
            }
        }
        // Increment all RRPVs (aging)
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            if (rrpv[cpu][set][w] < MAX_RRPV)
                rrpv[cpu][set][w]++;
        }
    }
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
    total_accesses++;
    if (hit) {
        // On hit: promote to MRU and mark reuse
        hit_accesses++;
        rrpv[cpu][set][way]     = 0;
        hit_flag[cpu][set][way] = true;
        return;
    }
    // Miss path: we're about to insert at [set][way], but first update Q for evicted
    miss_accesses++;
    int new_sig = GetSignature(PC);

    // If we evicted a valid block, update its Q-value
    if (valid[cpu][set][way]) {
        int old_sig    = sig[cpu][set][way];
        int old_action = action_meta[cpu][set][way];
        float reward   = hit_flag[cpu][set][way] ? 1.0f : 0.0f;
        // Compute max future Q (simplified gamma=0 ⇒ ignore future)
        float max_next_Q = 0.0f;
        if (GAMMA > 0.0f) {
            max_next_Q = Q_table[new_sig][0];
            for (int a = 1; a < ACTIONS; a++)
                if (Q_table[new_sig][a] > max_next_Q)
                    max_next_Q = Q_table[new_sig][a];
        }
        float &Qold = Q_table[old_sig][old_action];
        float delta = reward + GAMMA * max_next_Q - Qold;
        Qold += ALPHA * delta;
        q_updates++;
    }

    // Choose an insertion action via ε-greedy
    int chosen_action;
    float r = float(std::rand()) / float(RAND_MAX);
    if (r < EPSILON) {
        chosen_action = std::rand() % ACTIONS;
    } else {
        // greedy
        float best = Q_table[new_sig][0];
        chosen_action = 0;
        for (int a = 1; a < ACTIONS; a++) {
            if (Q_table[new_sig][a] > best) {
                best = Q_table[new_sig][a];
                chosen_action = a;
            }
        }
    }
    // Install new block metadata
    sig[cpu][set][way]         = new_sig;
    action_meta[cpu][set][way] = chosen_action;
    hit_flag[cpu][set][way]    = false;
    valid[cpu][set][way]       = true;

    // Map action → insertion RRPV
    if (chosen_action == 0) {
        rrpv[cpu][set][way] = 0;                   // MRU
    } else if (chosen_action == 1) {
        rrpv[cpu][set][way] = MAX_RRPV - 1;        // mid-age
    } else {
        rrpv[cpu][set][way] = MAX_RRPV;            // distant
    }
}

void PrintStats() {
    double hr = total_accesses ? (double)hit_accesses / total_accesses * 100.0 : 0.0;
    std::cout << "==== QL-RRIP Final Stats ====\n";
    std::cout << "Total Accesses : " << total_accesses << "\n";
    std::cout << " Hits          : " << hit_accesses  << "\n";
    std::cout << " Misses        : " << miss_accesses << "\n";
    std::cout << "Hit Rate (%)   : " << hr            << "\n";
    std::cout << "Q-Updates      : " << q_updates     << "\n";
}

void PrintStats_Heartbeat() {
    double hr = total_accesses ? (double)hit_accesses / total_accesses * 100.0 : 0.0;
    std::cout << "[QL-RRIP HB] Acc=" << total_accesses
              << " Hit=" << hit_accesses
              << " Miss=" << miss_accesses
              << " HR=" << hr << "%\n";
}