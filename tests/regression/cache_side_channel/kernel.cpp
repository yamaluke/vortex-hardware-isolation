#include <vx_spawn2.h>
#include <vx_intrinsics.h>
#include <vx_barrier.h>
#include "common.h"

static constexpr uint32_t kPrimeBarrierId = 1;
static constexpr uint32_t kVictimBarrierId = 2;
static constexpr uint32_t kProbeBarrierId = 3;
static constexpr uint32_t kInitBarrierId = 4;

static bool victim_access(uint32_t trial, uint32_t secret_bits) {
  if (trial < CACHE_SC_WARMUP_TRIALS) {
    return (trial & 1) != 0;
  }
  trial -= CACHE_SC_WARMUP_TRIALS;
  if (trial < CACHE_SC_TRAIN_TRIALS) {
    return (trial & 1) != 0;
  }
  return ((secret_bits >> (trial - CACHE_SC_TRAIN_TRIALS)) & 1u) != 0;
}

__kernel void kernel_main(kernel_arg_t* __UNIFORM__ arg) {
  if (threadIdx.x != 0) {
    return;
  }

  vortex::gbarrier prime_done(kPrimeBarrierId);
  vortex::gbarrier victim_done(kVictimBarrierId);
  vortex::gbarrier probe_done(kProbeBarrierId);
  vortex::gbarrier init_done(kInitBarrierId);
  const bool normal_world = blockIdx.x == 0;
  volatile uint32_t* l1_thrash = reinterpret_cast<volatile uint32_t*>(arg->l1_thrash_addr);
  volatile uint64_t* samples = reinterpret_cast<volatile uint64_t*>(arg->samples_addr);
  uint32_t checksum = 0;
  uint32_t victim_touches = 0;

  if (normal_world) {
    for (uint32_t line = 0; line < CACHE_SC_PROBE_LINES; ++line) {
      auto attacker = reinterpret_cast<volatile uint32_t*>(arg->attacker_addrs[line]);
      *attacker = static_cast<uint32_t>(arg->attacker_addrs[(line + 1) % CACHE_SC_PROBE_LINES]);
    }
    vx_rdcycle_sync();
  }
  init_done.arrive_and_wait();

  for (uint32_t trial = 0; trial < CACHE_SC_KERNEL_TRIALS; ++trial) {
    const bool measured = trial >= CACHE_SC_WARMUP_TRIALS;
    const uint32_t measured_trial = measured ? trial - CACHE_SC_WARMUP_TRIALS : 0;
    for (uint32_t line = 0; line < arg->l1_thrash_lines; ++line) {
      checksum ^= l1_thrash[line * (VX_CFG_L1_LINE_SIZE / sizeof(uint32_t))];
    }
    vx_rdcycle_sync();

    if (normal_world) {
      // The preceding probe leaves these lines in the normal L1. Remove them
      // before priming so every prime reaches and refills the shared L2 set.
      uintptr_t probe = arg->attacker_addrs[0];
      for (uint32_t line = 0; line < CACHE_SC_PROBE_LINES; ++line) {
        probe = *reinterpret_cast<const volatile uintptr_t*>(probe);
        checksum ^= static_cast<uint32_t>(probe);
      }
      vx_rdcycle_sync();
    }
    prime_done.arrive_and_wait();

    if (!normal_world && victim_access(trial, arg->secret_bits)) {
      auto victim = reinterpret_cast<volatile uint32_t*>(arg->victim_addr);
      checksum ^= *victim;
      ++victim_touches;
      vx_rdcycle_sync();
    }

    victim_done.arrive_and_wait();

    if (normal_world) {
      // The normal CTA's prime populated its private L1. Flush it after the
      // secure access so the timed probe observes shared-L2 residency.
      for (uint32_t line = 0; line < arg->l1_thrash_lines; ++line) {
        checksum ^= l1_thrash[line * (VX_CFG_L1_LINE_SIZE / sizeof(uint32_t))];
      }
      vx_rdcycle_sync();

      uint64_t begin = vx_rdcycle_sync();
      uintptr_t probe = arg->attacker_addrs[0];
      for (uint32_t line = 0; line < CACHE_SC_PROBE_LINES; ++line) {
        probe = *reinterpret_cast<const volatile uintptr_t*>(probe);
        checksum ^= static_cast<uint32_t>(probe);
      }
      uint64_t end = vx_rdcycle_sync();
      if (measured) {
        samples[measured_trial] = end - begin;
      }
    }
    probe_done.arrive_and_wait();
  }

  // Keep all cache loads architecturally live without exposing victim state.
  if (normal_world) {
    samples[CACHE_SC_DIAG_NORMAL_CORE] = vx_core_id();
  } else {
    samples[CACHE_SC_DIAG_VICTIM_TOUCHES] = victim_touches;
  }
  if (normal_world && checksum == 0xffffffffu) {
    samples[CACHE_SC_DIAG_NORMAL_CORE] = checksum;
  }
}
