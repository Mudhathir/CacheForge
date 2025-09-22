#include <vector>
#include <cstdint>
#include <iostream>
#include "../inc/champsim_crc2.h"

#define NUM_CORE 1
#define LLC_SETS (NUM_CORE * 2048)
#define LLC_WAYS 16

// RRIP parameters
#define RRPV_BITS      2
#define RRPV_MAX       ((1 << RRPV_BITS) - 1)  // 3
#define RRPV_INIT      (RRPV_MAX - 1)          // 2

// SHiP parameters
#define SHIP_SIG_BITS  10
#define SIG_TABLE_SIZE (1 << SHIP_SIG_BITS)
#define SIG_TABLE_MASK (SIG_TABLE_SIZE - 1)

#define SHIP_CTR_BITS   3
#define SHIP_CTR_MAX    ((1 << SHIP_CTR_BITS) - 1) // 7
#define SHIP_CTR_INIT   (SHIP_CTR_MAX >> 1)         // 3
#define SHIP_CTR_THRESH (SHIP_CTR_MAX >> 1)         // 3

// Stream detection
#define STREAM_PC_SIZE 1024
#define STREAM_PC_MASK (STREAM_PC_SIZE - 1)

// DIP (set-dueling) parameters
#define DIP_SAMPLE_PERIOD 64
#define DIP_SHIP_SET       0
#define DIP_BIP_SET        1

// BRRIP (bimodal) insertion probability = 1/32
#define BRRIP_POW2       32

struct StreamEntry {
    int64_t last_blk;
    int64_t last_stride;
    uint8_t count;
};

// Replacement state
static uint8_t  repl_rrpv[LLC_SETS][LLC_WAYS];
static bool     reuse_flag[LLC_SETS][LLC_WAYS];
static uint32_t repl_sig[LLC_SETS][LLC_WAYS];

// SHiP predictor table
static uint8_t ship_ctr[SIG_TABLE_SIZE];

// Stream detection table
static StreamEntry stream_table[STREAM_PC_SIZE];

// Statistics
static uint64_t stat_hits, stat_misses;
static uint64_t ship_sample_hits, bip_sample_hits;
static bool     use_ship_insertion;

// Initialize replacement state
void InitReplacementState() {
    // Initialize RRIP state and SHiP tables
    for (int s = 0; s < LLC_SETS; s++) {
        for (int w = 0; w < LLC_WAYS; w++) {
            repl_rrpv[s][w]   = RRPV_INIT;
            reuse_flag[s][w]  = false;
            repl_sig[s][w]    = 0;
        }
    }
    for (int i = 0; i < SIG_TABLE_SIZE; i++) {
        ship_ctr[i] = SHIP_CTR_INIT;
    }
    for (int i = 0; i < STREAM_PC_SIZE; i++) {
        stream_table[i].last_blk    = -1;
        stream_table[i].last_stride = 0;
        stream_table[i].count       = 0;
    }
    // Stats
    stat_hits            = 0;
    stat_misses          = 0;
    ship_sample_hits     = 0;
    bip_sample_hits      = 0;
    use_ship_insertion   = true;
}

// Victim selection (SRRIP-style)
uint32_t GetVictimInSet(
    uint32_t /*cpu*/,
    uint32_t set,
    const BLOCK */*current_set*/,
    uint64_t /*PC*/,
    uint64_t /*paddr*/,
    uint32_t /*type*/
) {
    while (true) {
        // 1) Find any way with RRPV == MAX
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            if (repl_rrpv[set][w] == RRPV_MAX) {
                return w;
            }
        }
        // 2) Age all entries
        for (uint32_t w = 0; w < LLC_WAYS; w++) {
            if (repl_rrpv[set][w] < RRPV_MAX)
                repl_rrpv[set][w]++;
        }
    }
}

// Update replacement state on access
void UpdateReplacementState(
    uint32_t /*cpu*/,
    uint32_t set,
    uint32_t way,
    uint64_t paddr,
    uint64_t PC,
    uint64_t /*victim_addr*/,
    uint32_t /*type*/,
    uint8_t hit
) {
    // Stream detection
    uint32_t      st_idx   = (PC >> 2) & STREAM_PC_MASK;
    int64_t       curr_blk = paddr >> 6;
    bool          is_stream = false;
    StreamEntry & st       = stream_table[st_idx];
    if (st.last_blk != -1) {
        int64_t stride = curr_blk - st.last_blk;
        if (stride == st.last_stride) {
            if (++st.count >= 4) {
                is_stream = true;
            }
        } else {
            st.last_stride = stride;
            st.count       = 1;
        }
    }
    st.last_blk = curr_blk;

    if (hit) {
        // HIT: promote and update DIP samples
        stat_hits++;
        reuse_flag[set][way] = true;
        repl_rrpv[set][way]  = 0;
        // DIP sample accounting
        int mod = set % DIP_SAMPLE_PERIOD;
        if      (mod == DIP_SHIP_SET) ship_sample_hits++;
        else if (mod == DIP_BIP_SET)  bip_sample_hits++;
        // Update global choice
        use_ship_insertion = (ship_sample_hits >= bip_sample_hits);
        return;
    }

    // MISS & EVICT: update SHiP predictor on the evicted block
    stat_misses++;
    uint32_t old_sig = repl_sig[set][way];
    if (reuse_flag[set][way]) {
        if (ship_ctr[old_sig] < SHIP_CTR_MAX) ship_ctr[old_sig]++;
    } else {
        if (ship_ctr[old_sig] > 0) ship_ctr[old_sig]--;
    }
    reuse_flag[set][way] = false;

    // Install new block: record signature
    uint32_t sig = (PC >> 2) & SIG_TABLE_MASK;
    repl_sig[set][way] = sig;

    // Choose insertion policy via DIP
    int  mod      = set % DIP_SAMPLE_PERIOD;
    bool on_ship  = (mod == DIP_SHIP_SET);
    bool on_bip   = (mod == DIP_BIP_SET);
    bool do_ship  = on_ship || (!on_bip && use_ship_insertion);

    if (do_ship) {
        // SHiP-Stream insertion: medium or eviction candidate
        bool reuse_pred = (ship_ctr[sig] > SHIP_CTR_THRESH) || is_stream;
        repl_rrpv[set][way] = reuse_pred ? RRPV_INIT : RRPV_MAX;
    } else {
        // Bimodal insertion: mostly eviction candidate, rare medium
        if ( ((PC >> 6) & (BRRIP_POW2 - 1)) == 0 )
            repl_rrpv[set][way] = RRPV_INIT;
        else
            repl_rrpv[set][way] = RRPV_MAX;
    }
}

// Print end-of-simulation statistics
void PrintStats() {
    uint64_t total = stat_hits + stat_misses;
    double   hr    = total ? (100.0 * stat_hits / total) : 0.0;
    std::cout << "==== DIP-SHiP-Stream Statistics ====\n";
    std::cout << "Total refs      : " << total           << "\n";
    std::cout << "Hits            : " << stat_hits       << "\n";
    std::cout << "Misses          : " << stat_misses     << "\n";
    std::cout << "Hit Rate (%)    : " << hr              << "\n";
    std::cout << "SHiP-sample hits: " << ship_sample_hits << "\n";
    std::cout << "BIP-sample hits : " << bip_sample_hits  << "\n";
}

// Print periodic (heartbeat) statistics
void PrintStats_Heartbeat() {
    PrintStats();
}