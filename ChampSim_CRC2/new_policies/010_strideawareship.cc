#include <vector>
#include <cstdint>
#include <iostream>
#include "../inc/champsim_crc2.h"

#define NUM_CORE            1
#define LLC_SETS            (NUM_CORE * 2048)
#define LLC_WAYS            16

// RRPV parameters
#define RRPV_BITS           2
#define MAX_RRPV            ((1 << RRPV_BITS) - 1)

// SHiP predictor parameters
#define SHIP_TABLE_BITS     14
#define SHIP_TABLE_SIZE     (1 << SHIP_TABLE_BITS)
#define SHIP_CTR_BITS       3
#define SHIP_CTR_MAX        ((1 << SHIP_CTR_BITS) - 1)
#define SHIP_CTR_INIT       (SHIP_CTR_MAX / 2)

// Stride‐run detection threshold
#define STRIDE_RUN_THRESHOLD 4

// Per‐block state
static uint8_t   rrpv      [NUM_CORE][LLC_SETS][LLC_WAYS];
static bool      valid     [NUM_CORE][LLC_SETS][LLC_WAYS];
static uint16_t  sig       [NUM_CORE][LLC_SETS][LLC_WAYS];
static bool      reused    [NUM_CORE][LLC_SETS][LLC_WAYS];

// SHiP temporal‐reuse table
static uint8_t   ship_table[SHIP_TABLE_SIZE];

// Stride‐run entries for streaming detection
struct StrideEntry {
    uint64_t last_addr;
    int64_t  last_stride;
    uint8_t  run;
};
static StrideEntry stride_table[SHIP_TABLE_SIZE];

// Statistics
static uint64_t total_accesses = 0, hit_accesses = 0, miss_accesses = 0;

void InitReplacementState() {
    total_accesses = hit_accesses = miss_accesses = 0;
    // Initialize cache state
    for (int c = 0; c < NUM_CORE; c++) {
        for (int s = 0; s < LLC_SETS; s++) {
            for (int w = 0; w < LLC_WAYS; w++) {
                valid[c][s][w]   = false;
                rrpv[c][s][w]    = MAX_RRPV;
                reused[c][s][w]  = false;
                sig[c][s][w]     = 0;
            }
        }
    }
    // Initialize SHiP and stride tables
    for (int i = 0; i < SHIP_TABLE_SIZE; i++) {
        ship_table[i]           = SHIP_CTR_INIT;
        stride_table[i].last_addr   = 0;
        stride_table[i].last_stride = 0;
        stride_table[i].run         = 0;
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
    // 1) Find invalid line
    for (uint32_t w = 0; w < LLC_WAYS; w++) {
        if (!valid[cpu][set][w]) {
            return w;
        }
    }
    // 2) SRRIP-style eviction
    while (true) {
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            if (rrpv[cpu][set][w] == MAX_RRPV) {
                return w;
            }
        }
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            if (rrpv[cpu][set][w] < MAX_RRPV) {
                rrpv[cpu][set][w]++;
            }
        }
    }
    return 0; // unreachable
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
    // Compute PC signature
    uint16_t curr_sig = (PC >> 2) & (SHIP_TABLE_SIZE - 1);

    // 1) Update stride-run predictor (for both hits and misses)
    int64_t cur_stride = (int64_t)paddr - (int64_t)stride_table[curr_sig].last_addr;
    if (stride_table[curr_sig].run > 0 &&
        cur_stride == stride_table[curr_sig].last_stride) {
        // Continuing a constant stride
        stride_table[curr_sig].run =
            std::min<uint8_t>(stride_table[curr_sig].run + 1,
                              STRIDE_RUN_THRESHOLD);
    } else {
        // New stride pattern
        stride_table[curr_sig].run = 1;
        stride_table[curr_sig].last_stride = cur_stride;
    }
    stride_table[curr_sig].last_addr = paddr;

    if (hit) {
        // On hit: promote to MRU
        hit_accesses++;
        rrpv[cpu][set][way] = 0;
        reused[cpu][set][way] = true;
        return;
    }

    // On miss
    miss_accesses++;

    // 2) Update SHiP on the evicted block
    if (valid[cpu][set][way]) {
        uint16_t old_sig = sig[cpu][set][way];
        if (reused[cpu][set][way]) {
            if (ship_table[old_sig] < SHIP_CTR_MAX) ship_table[old_sig]++;
        } else {
            if (ship_table[old_sig] > 0) ship_table[old_sig]--;
        }
    }

    // 3) Install new block
    valid[cpu][set][way]    = true;
    sig[cpu][set][way]      = curr_sig;
    reused[cpu][set][way]   = false;
    uint8_t ctr             = ship_table[curr_sig];

    // 4) Insertion decision:
    // If stride-run >= threshold → pure spatial stream → near-evict
    if (stride_table[curr_sig].run >= STRIDE_RUN_THRESHOLD) {
        rrpv[cpu][set][way] = MAX_RRPV;
    } else {
        // Temporal reuse classification
        if (ctr == SHIP_CTR_MAX) {
            // Heavy reuse → MRU
            rrpv[cpu][set][way] = 0;
        }
        else if (ctr > SHIP_CTR_INIT) {
            // Moderate reuse → near-MRU
            rrpv[cpu][set][way] = MAX_RRPV - 1;
        }
        else {
            // Low reuse → near-evict
            rrpv[cpu][set][way] = MAX_RRPV;
        }
    }
}

void PrintStats() {
    double hr = total_accesses
                ? (double)hit_accesses / total_accesses * 100.0
                : 0.0;
    std::cout << "==== StrideAwareSHiP Final Stats ====\n";
    std::cout << "Total Accesses : " << total_accesses << "\n";
    std::cout << " Hits          : " << hit_accesses   << "\n";
    std::cout << " Misses        : " << miss_accesses  << "\n";
    std::cout << "Hit Rate (%)   : " << hr             << "\n";
}

void PrintStats_Heartbeat() {
    double hr = total_accesses
                ? (double)hit_accesses / total_accesses * 100.0
                : 0.0;
    std::cout << "[StrideAwareSHiP HB] Acc="
              << total_accesses
              << " Hit="   << hit_accesses
              << " Miss="  << miss_accesses
              << " HR="    << hr << "%\n";
}