#include <vector>
#include <cstdint>
#include <iostream>
#include "../inc/champsim_crc2.h"

#define NUM_CORE           1
#define LLC_SETS           (NUM_CORE * 2048)
#define LLC_WAYS           16

// RRIP parameters
#define RRPV_BITS          2
#define RRPV_MAX           ((1 << RRPV_BITS) - 1)   // 3
#define RRPV_INIT          (RRPV_MAX - 1)           // 2

// DRRIP dueling parameters
#define SAMPLE_PERIOD      64                       // one SRRIP sample at set%64==0, one BRRIP at set%64==1
#define SRRIP_SAMPLE_SET   0
#define BRRIP_SAMPLE_SET   1
#define PSEL_BITS          10
#define PSEL_MAX           ((1 << PSEL_BITS) - 1)   // 1023
#define PSEL_THRESHOLD     (PSEL_MAX >> 1)          // 511

// Stream detection parameters
#define STREAM_PC_SIZE     1024
#define STREAM_PC_MASK     (STREAM_PC_SIZE - 1)
#define BRRIP_PROB         32                       // 1/32 probability for long re-reference

// Per-block RRPV array
static uint8_t repl_rrpv[LLC_SETS][LLC_WAYS];

// Global policy selector counter
static uint32_t psel;

// Stride‐stream tracker
struct StreamEntry {
    int64_t last_blk;
    int64_t last_stride;
    uint8_t count;
};
static StreamEntry stream_table[STREAM_PC_SIZE];

// Statistics
static uint64_t stat_hits, stat_misses;

// Initialize replacement state
void InitReplacementState() {
    // Initialize RRPVs
    for (int s = 0; s < LLC_SETS; s++) {
        for (int w = 0; w < LLC_WAYS; w++) {
            repl_rrpv[s][w] = RRPV_INIT;
        }
    }
    // Initialize stream tracker
    for (int i = 0; i < STREAM_PC_SIZE; i++) {
        stream_table[i].last_blk    = -1;
        stream_table[i].last_stride = 0;
        stream_table[i].count       = 0;
    }
    // Initialize PSEL to neutral
    psel = PSEL_MAX >> 1;
    // Zero stats
    stat_hits   = 0;
    stat_misses = 0;
}

// Victim selection (SRRIP-style aging)
uint32_t GetVictimInSet(
    uint32_t /*cpu*/,
    uint32_t set,
    const BLOCK */*current_set*/,
    uint64_t /*PC*/,
    uint64_t /*paddr*/,
    uint32_t /*type*/
) {
    while (true) {
        // 1) Search for any way with max RRPV
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

// Update on each access
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
    // Derive stream‐tracker index and current block
    uint32_t st_idx   = (PC >> 2) & STREAM_PC_MASK;
    int64_t  curr_blk = paddr >> 6;
    // Update stream detection
    StreamEntry &st = stream_table[st_idx];
    bool is_stream = false;
    if (st.last_blk != -1) {
        int64_t stride = curr_blk - st.last_blk;
        if (stride == st.last_stride) {
            st.count++;
            if (st.count >= 2) {
                is_stream = true;
            }
        } else {
            st.last_stride = stride;
            st.count       = 1;
        }
    }
    st.last_blk = curr_blk;

    if (hit) {
        // ---- HIT ----
        stat_hits++;
        repl_rrpv[set][way] = 0; // promotion on hit
    } else {
        // ---- MISS & INSERT ----
        stat_misses++;
        // Determine sample set type
        uint32_t sample = set & (SAMPLE_PERIOD - 1);
        bool     is_srrip_sample = (sample == SRRIP_SAMPLE_SET);
        bool     is_brrip_sample = (sample == BRRIP_SAMPLE_SET);

        // Decide insertion policy
        bool do_brrip;
        if (is_stream) {
            // Preserve spatial stream reuse
            do_brrip = false;
        }
        else if (is_srrip_sample) {
            // SRRIP sample: bias towards SRRIP
            do_brrip = false;
            if (psel > 0) psel--;
        }
        else if (is_brrip_sample) {
            // BRRIP sample: bias towards BRRIP
            do_brrip = true;
            if (psel < PSEL_MAX) psel++;
        }
        else {
            // Follower sets: use global policy
            do_brrip = (psel >= PSEL_THRESHOLD);
        }

        // Perform insertion
        if (do_brrip) {
            // BRRIP: mostly mid‐RRPV, rarely max
            if ((curr_blk & (BRRIP_PROB - 1)) == 0) {
                repl_rrpv[set][way] = RRPV_MAX;
            } else {
                repl_rrpv[set][way] = RRPV_INIT;
            }
        } else {
            // SRRIP or stream: always mid‐RRPV
            repl_rrpv[set][way] = RRPV_INIT;
        }
    }
}

// Print end‐of‐simulation statistics
void PrintStats() {
    uint64_t total = stat_hits + stat_misses;
    double   hr    = total ? (100.0 * stat_hits / total) : 0.0;
    std::cout << "==== DRRIP-Stream Statistics ====\n";
    std::cout << "Total refs   : " << total      << "\n";
    std::cout << "Hits         : " << stat_hits  << "\n";
    std::cout << "Misses       : " << stat_misses<< "\n";
    std::cout << "Hit Rate (%) : " << hr         << "\n";
}

// Print periodic (heartbeat) statistics
void PrintStats_Heartbeat() {
    PrintStats();
}