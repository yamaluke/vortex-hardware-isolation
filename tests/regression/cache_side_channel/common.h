#ifndef CACHE_SIDE_CHANNEL_COMMON_H
#define CACHE_SIDE_CHANNEL_COMMON_H

#include <stdint.h>

#define CACHE_SC_PROBE_LINES 8u
#define CACHE_SC_VICTIM_LINES 1u
#define CACHE_SC_TRAIN_TRIALS 8u
#define CACHE_SC_TARGET_TRIALS 8u
#define CACHE_SC_WARMUP_TRIALS 2u
#define CACHE_SC_TOTAL_TRIALS (CACHE_SC_TRAIN_TRIALS + CACHE_SC_TARGET_TRIALS)
#define CACHE_SC_KERNEL_TRIALS (CACHE_SC_WARMUP_TRIALS + CACHE_SC_TOTAL_TRIALS)
#define CACHE_SC_DIAG_NORMAL_CORE CACHE_SC_TOTAL_TRIALS
#define CACHE_SC_DIAG_VICTIM_TOUCHES (CACHE_SC_TOTAL_TRIALS + 1u)
#define CACHE_SC_SAMPLE_COUNT (CACHE_SC_TOTAL_TRIALS + 2u)

typedef struct {
  uint64_t attacker_addrs[CACHE_SC_PROBE_LINES];
  uint64_t victim_addr;
  uint64_t l1_thrash_addr;
  uint64_t samples_addr;
  uint32_t l1_thrash_lines;
  uint32_t secret_bits;
} kernel_arg_t;

#endif
